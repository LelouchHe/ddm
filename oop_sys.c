/* sys.c, liboop, copyright 1999 Dan Egnor

   This is free software; you can redistribute it and/or modify it under the
   terms of the GNU Lesser General Public License, version 2.1 or later.
   See the file COPYING for details. */

#include "oop.h"

#include <errno.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>   /* Needed on NetBSD1.1/SPARC due to bzero/FD_ZERO. */
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>  /* Needed on AIX 4.2 due to bzero/FD_ZERO. */
#endif

int _oop_continue, _oop_shortcut, _oop_error;

static void *(*oop_malloc)(size_t) = malloc;
static void (*oop_free)(void *) = free;
/* static void *(*oop_realloc)(void *, size_t) = realloc; */


#define MAGIC 0x9643
#define MAX_TIME_NODES	100

typedef struct sys_time sys_time;
typedef struct sys_time_pool sys_time_pool;

struct sys_time {
	struct sys_time *next;
	struct timeval tv;
	oop_time_fun *f;
	void *v;
};

struct sys_signal_handler {
	struct sys_signal_handler *next;
	oop_signal_fun *f;
	void *v;
};

struct sys_signal {
	struct sys_signal_handler *list,*ptr;
	struct sigaction old;
	volatile sig_atomic_t active;
};

struct sys_file_handler {
	oop_fd_fun *f;
	void *v;
};

struct sys_time_pool{
	sys_time_pool *next;
	sys_time nodes[MAX_TIME_NODES];
};

typedef struct sys_file_handler sys_file[OOP_NUM_EVENTS];

typedef struct oop_source_sys oop_source_sys;
struct oop_source_sys {
	oop_source_t oop;
	int magic;
	int in_run;
	int num_events;

	/* Timeout queue */
	struct sys_time *time_queue;
	struct sys_time *time_run;

	struct sys_time *time_empty;
	sys_time_pool *time_pool;

	/* Signal handling */
	struct sys_signal sig[OOP_NUM_SIGNALS];
	sigjmp_buf env;
	int do_jmp;
	int sig_active;
	int last_sig;

	/* File descriptors */
	int num_files;
	sys_file *files;
	int last_read;
	int last_write;
	int last_exception;
};

struct oop_source_sys *sys_sig_owner[OOP_NUM_SIGNALS];

static sys_time* _time_node_new(oop_source_sys *sys){
	sys_time *node;
	if(sys->time_empty == NULL){
		int i;
		sys_time_pool *pool = oop_malloc(sizeof(sys_time_pool));
		assert(pool);
		pool->next = sys->time_pool;
		sys->time_pool = pool;
		node = pool->nodes;
		node->next = NULL;
		for(i = 1; i < MAX_TIME_NODES; i ++){
			pool->nodes[i].next = node;
			node = pool->nodes + i;
		}
		sys->time_empty = node;
	}
	node = sys->time_empty;
	sys->time_empty = node->next;
	return node;
}

static void _time_node_free(oop_source_sys *sys, sys_time *node){
	node->next = sys->time_empty;
	sys->time_empty = node;
}

static oop_source_sys *verify_source(oop_source_t *source) {
	oop_source_sys *sys = (oop_source_sys *) source;
	assert((void *)MAGIC == source->xdata && MAGIC == sys->magic && "corrupt oop_source_t structure");
	return sys;
}

static void sys_add_fd(oop_source_t *source,int fd,oop_event_t ev,
                      oop_fd_fun *f,void *v) {
	oop_source_sys *sys = verify_source(source);
	assert(NULL != f && "callback must be non-NULL");
	if (fd >= sys->num_files) {
		int i,j,num_files = 1 + fd;
		sys_file *files = (sys_file *)oop_malloc(num_files * sizeof(sys_file));
		if (NULL == files) return; /* ugh */

		memcpy(files,sys->files,sizeof(sys_file) * sys->num_files);
		for (i = sys->num_files; i < num_files; ++i)
			for (j = 0; j < OOP_NUM_EVENTS; ++j)
				files[i][j].f = NULL;

		if (NULL != sys->files) oop_free(sys->files);
		sys->files = files;
		sys->num_files = num_files;
	}

	assert(NULL == sys->files[fd][ev].f && "multiple handlers registered for a file event");
	sys->files[fd][ev].f = f;
	sys->files[fd][ev].v = v;
	++sys->num_events;
}

static void sys_remove_fd(oop_source_t *source,int fd,oop_event_t ev) {
	oop_source_sys *sys = verify_source(source);
	if (fd < sys->num_files && NULL != sys->files[fd][ev].f) {
		sys->files[fd][ev].f = NULL;
		sys->files[fd][ev].v = NULL;
		--sys->num_events;
	}
}

static void sys_add_time(oop_source_t *source,struct timeval tv,
                        oop_time_fun *f,void *v) {
	oop_source_sys *sys = verify_source(source);
	struct sys_time **p = &sys->time_queue;
	struct sys_time *time = _time_node_new(sys);
	assert(tv.tv_usec >= 0 && "tv_usec must be positive");
	assert(tv.tv_usec < 1000000 && "tv_usec measures microseconds");
	assert(NULL != f && "callback must be non-NULL");
	if (NULL == time) return; /* ugh */
	time->tv = tv;
	time->f = f;
	time->v = v;

	while (NULL != *p
	&&    ((*p)->tv.tv_sec < tv.tv_sec
	||    ((*p)->tv.tv_sec == tv.tv_sec 
	&&     (*p)->tv.tv_usec <= tv.tv_usec))) p = &(*p)->next;
	time->next = *p;
	*p = time;

	++sys->num_events;
}

static int sys_rm_time(oop_source_sys *sys,
                           struct sys_time **p,struct timeval tv,
                           oop_time_fun *f,void *v) {
	while (NULL != *p
	&&    ((*p)->tv.tv_sec < tv.tv_sec
	||    ((*p)->tv.tv_sec == tv.tv_sec 
	&&     (*p)->tv.tv_usec < tv.tv_usec))) p = &(*p)->next;
	while (NULL != *p
	&&     (*p)->tv.tv_sec == tv.tv_sec
	&&     (*p)->tv.tv_usec == tv.tv_usec
	&&    ((*p)->f != f || (*p)->v != v)) p = &(*p)->next;
	if (NULL != *p 
	&& (*p)->tv.tv_sec == tv.tv_sec && (*p)->tv.tv_usec == tv.tv_usec) {
		struct sys_time *time = *p;
		assert(f == time->f);
		assert(v == time->v);
		*p = time->next;
		_time_node_free(sys, time);
		--sys->num_events;
		return 1;
	}
	return 0;
}

static void sys_remove_time(oop_source_t *source,struct timeval tv,
                            oop_time_fun *f,void *v) {
	oop_source_sys *sys = verify_source(source);
	if (!sys_rm_time(sys,&sys->time_run,tv,f,v))
		sys_rm_time(sys,&sys->time_queue,tv,f,v);
}

static void sys_signal_handler(int sig) {
	oop_source_sys *sys = sys_sig_owner[sig];
	struct sigaction act;
	assert(NULL != sys);

	/* Reset the handler, in case this is needed. */
	sigaction(sig,NULL,&act);
	act.sa_handler = sys_signal_handler;
	sigaction(sig,&act,NULL);

	assert(NULL != sys->sig[sig].list);
	sys->sig[sig].active = 1;
	sys->sig_active = 1;

	/* Break out of select() loop, if necessary. */
	if (sys->do_jmp) siglongjmp(sys->env,1);
}

static void sys_add_signal(oop_source_t *source,int sig,
                          oop_signal_fun *f,void *v) {
	oop_source_sys *sys = verify_source(source);
	struct sys_signal_handler *handler = (struct sys_signal_handler *)oop_malloc(sizeof(*handler));
	assert(NULL != f && "callback must be non-NULL");
	if (NULL == handler) return; /* ugh */

	assert(sig > 0 && sig < OOP_NUM_SIGNALS && "invalid signal number");

	handler->f = f;
	handler->v = v;
	handler->next = sys->sig[sig].list;
	sys->sig[sig].list = handler;
	++sys->num_events;

	if (NULL == handler->next) {
		struct sigaction act;

		assert(NULL == sys_sig_owner[sig]);
		sys_sig_owner[sig] = sys;

		assert(0 == sys->sig[sig].active);
		sigaction(sig,NULL,&act);
		sys->sig[sig].old = act;
		act.sa_handler = sys_signal_handler;
#ifdef SA_NODEFER /* BSD/OS doesn't have this, for one. */
		act.sa_flags &= ~SA_NODEFER;
#endif
		sigaction(sig,&act,NULL);
	}
}

static void sys_remove_signal(oop_source_t *source,int sig,
                              oop_signal_fun *f,void *v) {
	oop_source_sys *sys = verify_source(source);
	struct sys_signal_handler **pp = &sys->sig[sig].list;

	assert(sig > 0 && sig < OOP_NUM_SIGNALS && "invalid signal number");

	while (NULL != *pp && ((*pp)->f != f || (*pp)->v != v))
		pp = &(*pp)->next;

	if (NULL != *pp) {
		struct sys_signal_handler *p = *pp;

		if (NULL == p->next && &sys->sig[sig].list == pp) {
			sigaction(sig,&sys->sig[sig].old,NULL);
			sys->sig[sig].active = 0;
			sys_sig_owner[sig] = NULL;
		}

		*pp = p->next;
		if (sys->sig[sig].ptr == p) sys->sig[sig].ptr = *pp;
		--sys->num_events;
		oop_free(p);
	}
}

static void *sys_time_run(oop_source_sys *sys) {
	void *ret = OOP_CONTINUE;
	while (OOP_CONTINUE == ret && NULL != sys->time_run) {
		struct sys_time *p = sys->time_run;
		sys->time_run = sys->time_run->next;
		--sys->num_events;
		ret = p->f(&sys->oop,p->tv,p->v); /* reenter! */
		_time_node_free(sys, p);
	}
	return ret;
}

static void *sys_run_once(oop_source_sys *sys, int nonblock) {
	void * volatile ret = OOP_CONTINUE;
	struct timeval * volatile ptv = NULL;
	struct timeval tv;
	fd_set rfd,wfd,xfd;
	int i,rv;
	int k, last;

	assert(!sys->in_run && "oop_sys_run_once is not reentrant");
	sys->in_run = 1;

	if (NULL != sys->time_run) {
		/* interrupted, restart */
		ptv = &tv;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	} else if (NULL != sys->time_queue) {
		ptv = &tv;
		gettimeofday(ptv,NULL);
		if (sys->time_queue->tv.tv_usec < tv.tv_usec) {
			tv.tv_usec -= 1000000;
			tv.tv_sec ++;
		}
		tv.tv_sec = sys->time_queue->tv.tv_sec - tv.tv_sec;
		tv.tv_usec = sys->time_queue->tv.tv_usec - tv.tv_usec;
		if (tv.tv_sec < 0) {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
		}
	}

	if (!sys->sig_active) sys->do_jmp = !sigsetjmp(sys->env,1);
	if (sys->sig_active) {
		/* Still perform select(), but don't block. */
		ptv = &tv;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	/* select() fails on FreeBSD with EINVAL if tv_sec > 1000000000.
           The manual specifies the error code but not the limit.  We limit
	   the select() timeout to one hour for portability. */
	if (NULL != ptv && ptv->tv_sec >= 3600) ptv->tv_sec = 3599;
	assert(NULL == ptv 
	   || (ptv->tv_sec >= 0 && ptv->tv_sec < 3600
           &&  ptv->tv_usec >= 0 && ptv->tv_usec < 1000000));

	FD_ZERO(&rfd);
	FD_ZERO(&wfd);
	FD_ZERO(&xfd);
	for (i = 0; i < sys->num_files; ++i) {
		if (NULL != sys->files[i][OOP_READ].f) FD_SET(i,&rfd);
		if (NULL != sys->files[i][OOP_WRITE].f) FD_SET(i,&wfd);
		if (NULL != sys->files[i][OOP_EXCEPTION].f) FD_SET(i,&xfd);
	}

	do
		rv = select(sys->num_files,&rfd,&wfd,&xfd,ptv);
	while (0 > rv && EINTR == errno);

	sys->do_jmp = 0;

	if (0 > rv) { /* Error in select(). */
		fprintf(stderr, "select() failed: %s\n", strerror(errno));
		ret = OOP_ERROR;
		goto done; 
	}

	if (sys->sig_active)
	{
		sys->sig_active = 0;
		last = sys->last_sig + 1;
		for (k = 0; OOP_CONTINUE == ret && k < OOP_NUM_SIGNALS; ++k)
		{
			i = (k + last) % OOP_NUM_SIGNALS;
			if (sys->sig[i].active) {
				sys->sig[i].active = 0;
				sys->sig[i].ptr = sys->sig[i].list;
			}
			while (OOP_CONTINUE == ret && NULL != sys->sig[i].ptr) {
				struct sys_signal_handler *h;
				h = sys->sig[i].ptr;
				sys->sig[i].ptr = h->next;
				ret = h->f(&sys->oop,i,h->v);
			}
		}
		sys->last_sig = i;
		if (OOP_CONTINUE != ret)
		{
			sys->sig_active = 1; /* come back */
			if (OOP_SHORTCUT != ret)
				goto done;
		}
	}

	if (0 < rv)
	{
		last = sys->last_exception + 1;
		for (k = 0; OOP_CONTINUE == ret && k < sys->num_files; ++k)
		{
			i = (k + last) % sys->num_files;
			if (FD_ISSET(i,&xfd) 
			&&  NULL != sys->files[i][OOP_EXCEPTION].f)
				ret = sys->files[i][OOP_EXCEPTION].f(
					&sys->oop,i,OOP_EXCEPTION,
					 sys->files[i][OOP_EXCEPTION].v);
		}
		sys->last_exception = i;

		last = sys->last_write + 1;
		for (k = 0; OOP_CONTINUE == ret && k < sys->num_files; ++k)
		{
			i = (k + last) % sys->num_files;
			if (FD_ISSET(i,&wfd) 
			&&  NULL != sys->files[i][OOP_WRITE].f)
				ret = sys->files[i][OOP_WRITE].f(
					&sys->oop,i,OOP_WRITE,
					 sys->files[i][OOP_WRITE].v);
		}
		sys->last_write = i;

		last = sys->last_read + 1;
		for (k = 0; OOP_CONTINUE == ret && k < sys->num_files; ++k)
		{
			i = (k + last) % sys->num_files;
			if (FD_ISSET(i,&rfd) 
			&&  NULL != sys->files[i][OOP_READ].f)
				ret = sys->files[i][OOP_READ].f(
					&sys->oop,i,OOP_READ,
					 sys->files[i][OOP_READ].v);
		}
		sys->last_read = i;

		if (OOP_CONTINUE != ret && OOP_SHORTCUT != ret)
			goto done;
	}

	/* Catch any leftover timeout events. */
	ret = sys_time_run(sys);
	if (OOP_CONTINUE != ret)
		goto done;

	if (NULL != sys->time_queue)
	{
		struct sys_time *p,**pp = &sys->time_queue;
		gettimeofday(&tv,NULL);
		while (NULL != *pp 
		   && (tv.tv_sec > (*pp)->tv.tv_sec
		   || (tv.tv_sec == (*pp)->tv.tv_sec
		   &&  tv.tv_usec >= (*pp)->tv.tv_usec)))
			pp = &(*pp)->next;
		p = *pp;
		*pp = NULL;
		sys->time_run = sys->time_queue;
		sys->time_queue = p;
	}

	ret = sys_time_run(sys);

done:
	sys->in_run = 0;
	return ret;
}

static void *sys_run(oop_source_t *source, int flags) {
	void *ret = OOP_CONTINUE;
	oop_source_sys *sys = verify_source(source);
	int nonblock = flags & OOP_RUN_NONBLOCK;
	assert(!sys->in_run && "oop_sys_run is not reentrant");
	if (flags & OOP_RUN_ONCE)
	{
		if (0 != sys->num_events)
			ret = sys_run_once(sys, nonblock);
	}
	else
	{
		while (0 != sys->num_events && (OOP_CONTINUE == ret || OOP_SHORTCUT == ret))
			ret = sys_run_once(sys, 0);
	}
	return ret;
}

static void sys_delete(oop_source_t *source) {
	int i,j;
	sys_time_pool *pool, *tmp_pool;
	oop_source_sys *sys = verify_source(source);
	assert(!sys->in_run && "cannot delete while in oop_sys_run");
	assert(NULL == sys->time_queue 
	&&     NULL == sys->time_run
	&&     "cannot delete with timeout");

	for (i = 0; i < OOP_NUM_SIGNALS; ++i)
		assert(NULL == sys->sig[i].list && "cannot delete with signal handler");

	for (i = 0; i < sys->num_files; ++i)
		for (j = 0; j < OOP_NUM_EVENTS; ++j)
			assert(NULL == sys->files[i][j].f && "cannot delete with file handler");

	assert(0 == sys->num_events);
	if (NULL != sys->files) oop_free(sys->files);
	pool = sys->time_pool;
	while(pool){
		tmp_pool = pool;
		pool = pool->next;
		oop_free(tmp_pool);
	}
	oop_free(sys);
}

oop_source_t *oop_sys_new(void)
{
	int i;
	oop_source_sys *source = (oop_source_sys *)oop_malloc(sizeof(oop_source_sys));
	if (NULL == source)
		return NULL;
	memset(source, 0, sizeof(source[0]));
	source->oop.add_fd = sys_add_fd;
	source->oop.remove_fd = sys_remove_fd;
	source->oop.add_time = sys_add_time;
	source->oop.remove_time = sys_remove_time;
	source->oop.add_signal = sys_add_signal;
	source->oop.remove_signal = sys_remove_signal;
	source->oop.run = sys_run;
	source->oop.del = sys_delete;
	source->oop.xdata = (void *)MAGIC;
	source->magic = MAGIC;
	source->in_run = 0;
	source->num_events = 0;
	source->time_queue = source->time_run = NULL;

	source->do_jmp = 0;
	source->sig_active = 0;
	for (i = 0; i < OOP_NUM_SIGNALS; ++i) {
		source->sig[i].list = NULL;
		source->sig[i].ptr = NULL;
		source->sig[i].active = 0;
	}

	source->num_files = 0;
	source->files = NULL;

	return &source->oop;
}

