/* oop.h, liboop, copyright 1999 Dan Egnor

   This is free software; you can redistribute it and/or modify it under the
   terms of the GNU Lesser General Public License, version 2.1 or later.
   See the file COPYING for details. */

#ifndef OOP_H_
#define OOP_H_ 1

#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct oop_source_t oop_source_t;

/* File descriptor action types */
typedef enum {
	OOP_READ,
	OOP_WRITE,
	OOP_EXCEPTION,

	OOP_NUM_EVENTS
} oop_event_t;

static const struct timeval OOP_TIME_NOW = { 0, 0 };

/* Maximum signal number.  (The OS may have a stricter limit!) */
#define OOP_NUM_SIGNALS 	64


/* Callbacks may return one of these */
extern int _oop_continue;	/* internal only */
extern int _oop_shortcut;	/* internal only */
extern int _oop_error; 		/* internal only */

#define OOP_CONTINUE 	((void *) &_oop_continue)
#define OOP_SHORTCUT	((void *) &_oop_shortcut)
#define OOP_ERROR	((void *) &_oop_error)
#define OOP_HALT 	((void *) NULL)


/* Callback function prototypes */
typedef void *oop_fd_fun(oop_source_t *, int fd, oop_event_t, void *);
typedef void *oop_time_fun(oop_source_t *, struct timeval, void *);
typedef void *oop_signal_fun(oop_source_t *, int sig, void *);

#define OOP_RUN_ONCE		0x01
#define OOP_RUN_NONBLOCK	0x02
struct oop_source_t {
	void (*add_fd)(oop_source_t *, int fd, oop_event_t, oop_fd_fun *, void *arg);
	void (*remove_fd)(oop_source_t *, int fd, oop_event_t);

	void (*add_time)(oop_source_t *, struct timeval, oop_time_fun *, void *arg);
	void (*remove_time)(oop_source_t *, struct timeval, oop_time_fun *, void *arg);

	void (*add_signal)(oop_source_t *, int sig, oop_signal_fun *, void *arg);
	void (*remove_signal)(oop_source_t *, int sig, oop_signal_fun *, void *arg);

	void *(*run)(oop_source_t *, int flags);
	void (*del)(oop_source_t *);
	
	/* private data */
	void *xdata;
};

#define oop_add_fd(es, fd, event, fun, arg)	((es)->add_fd((es), fd, event, fun, arg))
#define oop_remove_fd(es, fd, event)		((es)->remove_fd((es), fd, event))
#define oop_add_time(es, tv, fun, arg)		((es)->add_time((es), tv, fun, arg))
#define oop_remove_time(es, tv, fun, arg)	((es)->remove_time((es), tv, fun, arg))
#define oop_add_signal(es, sig, fun, arg)	((es)->add_signal((es), sig, fun, arg))
#define oop_remove_signal(es, sig, fun, arg)	((es)->remove_signal((es), sig, fun, arg))
#define oop_run(es, flags)			((es)->run((es), flags))
#define oop_del(es)				((es)->del((es)))


/* Create a system event source.  Returns NULL on failure. */
oop_source_t *oop_sys_new();


#ifdef __cplusplus
}
#endif

#endif
