#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <oop.h>

#include "dyndict_manager.h"

#define DD_STAT 0xF
#define DD_EMPTY 0x0
#define DD_ADD 0x1
#define DD_DEL 0x2
#define DD_DONE 0x4
#define DD_FAIL 0x8

#define DD_LOAD_FAIL 0x10
#define DD_NEED_RELOAD 0x20

#define DD_REF 'R'
#define DD_UNREF 'U'

#define DDM_LIVE 0x4C495645 /* "LIVE" */
#define DDM_FINI 0x46494E49 /* "FINI" */
#define DDM_DEAD 0x44454144 /* "DEAD" */

#define MAX_QUEUE_SIZE 1024
#define MAX_DICT_NUM 2

#define PIPE_NUM 2
#define PIPE_READ 0
#define PIPE_WRITE 1

#define DEF_DD_NUM 100

#define CMD_DD 'D'
#define CMD_EXIT 'E'

typedef void *(*ini_fun_t)(void *);
typedef void (*fini_fun_t)(void *);

struct trival_queue_t
{
    void *data[MAX_QUEUE_SIZE];
    int head;
    int tail;
    int num;
};

void *tq_get(struct trival_queue_t *tq)
{
    if (tq->num <= 0)
        return NULL;

    tq->num--;
    void *ret = tq->data[tq->head];
    tq->head++;
    if (tq->head >= MAX_QUEUE_SIZE)
        tq->head = 0;

    return ret;
}

int tq_put(struct trival_queue_t *tq, void *d)
{
    if (tq->num > MAX_QUEUE_SIZE)
        return -1;

    tq->num++; 
    tq->data[tq->tail] = d;
    tq->tail++;
    if (tq->tail >= MAX_QUEUE_SIZE)
        tq->tail = 0;

    return 0;
}

// 单向传递,dd/ddm -> oop
struct info_queue_t
{
    struct trival_queue_t tq;
    pthread_mutex_t mutex;
    int pipefd[PIPE_NUM];
};

struct dyndict_t
{
    int stat;
    const char *name;

    ini_fun_t ini_fun;
    void *ini_args;
    fini_fun_t fini_fun;
    // fini_args is dict itself
    // 上面的数据主要由ddm操作

    // 初始化由ddm,操作改变由oop
    int intval_s;
    pthread_rwlock_t rwlock;
    // 通知ddm的dd首次加载完毕或卸载完毕
    int oop2dd[PIPE_NUM];
    // 通知oop的dd的ref/unref操作
    // 队列为dict
    struct info_queue_t iq;

    // 下面的数据主要由oop操作
    struct timeval reload_tv;
    void *dicts[MAX_DICT_NUM];
    int count[MAX_DICT_NUM];
    int index;
};

struct dd_manager_t
{
    struct dyndict_t *dds;
    int num;
    int max;

    pthread_rwlock_t rwlock;

    // need to be nonblock 
    struct info_queue_t iq;
    pthread_t oop_pid;

    uint32_t magic;
};

static void *reload(oop_source_t *oop, struct timeval tv, void *args);

static int load_dd(oop_source_t *oop, struct dyndict_t *dd, int next)
{
    int ret = 0;
    if (dd->dicts[next] != NULL && dd->fini_fun != NULL)
        dd->fini_fun(dd->dicts[next]);
    dd->dicts[next] = dd->ini_fun(dd->ini_args);
    if (dd->dicts[next] == NULL)
    {
        dd->stat |= DD_LOAD_FAIL;
        ret = -1;
        goto LB_DONE;
    }

    pthread_rwlock_wrlock(&dd->rwlock);
    dd->index = next;
    pthread_rwlock_unlock(&dd->rwlock);

LB_DONE:
    if (dd->stat & DD_ADD)
    {
        char msg = CMD_DD;
        write(dd->oop2dd[PIPE_WRITE], &msg, sizeof (char));
    }

    gettimeofday(&dd->reload_tv, NULL);
    dd->reload_tv.tv_sec += dd->intval_s;
    oop_add_time(oop, dd->reload_tv, reload, dd);

    return ret;
}

static int del_dd(oop_source_t *oop, struct dyndict_t *dd)
{
    oop_remove_time(oop, dd->reload_tv, reload, dd);
    int i;
    int over = 1;
    for (i = 0; i < MAX_DICT_NUM; i++)
    {
        if (dd->dicts[i] != NULL)
        {
            if (dd->count[i] > 0)
                over = 0;
            else if (dd->fini_fun != NULL)
            {
                dd->fini_fun(dd->dicts[i]);
                dd->dicts[i] = NULL;
            }
        }
    }

    if (over == 1)
    {
        oop_remove_fd(oop, dd->iq.pipefd[PIPE_READ], OOP_READ);
        char msg = CMD_DD;
        write(dd->oop2dd[PIPE_WRITE], &msg, sizeof (char));
        return -1;
    }

    return 0;
}


static int check(oop_source_t *oop, struct dyndict_t *dd, int pos)
{
    if (dd->stat & DD_NEED_RELOAD)
    {
        if (load_dd(oop, dd, pos) == 0)
            dd->stat &= ~DD_NEED_RELOAD;
    }
    else if ((dd->stat & DD_STAT) == DD_DEL)
        del_dd(oop, dd);

    return 0;
}

// 应该需要触发条件吧?
// reload需要count降为0
static void *check_dd(oop_source_t *oop, int fd, oop_event_t event, void *args)
{
    struct dyndict_t *dd = (struct dyndict_t *)args;
    char msg;
    read(fd, &msg, sizeof (char));

    void *dict;
    pthread_mutex_lock(&dd->iq.mutex);
    dict = tq_get(&dd->iq.tq);
    pthread_mutex_unlock(&dd->iq.mutex);

    if (dict == NULL)
        return OOP_CONTINUE;

    int i;
    for (i = 0; i < MAX_DICT_NUM; i++)
    {
        if (dict == dd->dicts[i])
        {
            if (msg == DD_REF)
                dd->count[i]++;
            else if (msg == DD_UNREF)
            {
                dd->count[i]--;
                if (dd->count[i] == 0)
                    check(oop, dd, i);
            }
            break;
        }
    }

    return OOP_CONTINUE;
}


static int find_next_dict(struct dyndict_t *dd)
{
    int next = (dd->index + 1) % MAX_DICT_NUM;
    while (next != dd->index && dd->count[next] > 0)
        next = (next + 1) % MAX_DICT_NUM;

    if (next == dd->index)
        return -1;
    else
        return next;
}


// 资源正在使用,怎么办?
// 不进行加载,直到发现空余词典之前,使用旧词典
static void *reload(oop_source_t *oop, struct timeval tv, void *args)
{
    struct dyndict_t *dd = (struct dyndict_t *)args;

    if (dd->stat & DD_ADD)
    {
        load_dd(oop, dd, 0);
        return OOP_CONTINUE;
    }

    // 当前使用index,不使用next
    // 所以无需担心同步问题
    int next = find_next_dict(dd);
    if (next == -1)
    {
        dd->stat |= DD_NEED_RELOAD;
        return OOP_CONTINUE;
    }

    load_dd(oop, dd, next);

    return OOP_CONTINUE;
}

static int add_dd(oop_source_t *oop, struct dyndict_t *dd)
{
    dd->index = 0;
    memset(dd->dicts, 0, sizeof (MAX_DICT_NUM * sizeof (void *)));
    memset(dd->count, 0, sizeof (MAX_DICT_NUM * sizeof (int)));

    oop_add_fd(oop, dd->iq.pipefd[OOP_READ], OOP_READ, check_dd, dd);
    gettimeofday(&dd->reload_tv, NULL);
    oop_add_time(oop, dd->reload_tv, reload, dd);

    return 0;
}

static void *in_notify(oop_source_t *oop, int fd, oop_event_t event, void *args)
{
    char msg;
    struct info_queue_t *iq = (struct info_queue_t *)args;

    pthread_mutex_lock(&iq->mutex);

    while (read(fd, &msg, sizeof (char)) != -1)
    {
        if (msg == CMD_EXIT)
        {
            oop_remove_fd(oop, fd, OOP_READ);
            break;
        }
        else if (msg == CMD_DD)
        {
            struct dyndict_t *dd = (struct dyndict_t *)tq_get(&iq->tq);
            if (dd->stat == DD_ADD)
                add_dd(oop, dd);
            else if (dd->stat == DD_DEL)
                del_dd(oop, dd);
        }
    }

    pthread_mutex_unlock(&iq->mutex);

    return OOP_CONTINUE;
}

static void *oop_thread(void *args)
{
    struct info_queue_t *iq = (struct info_queue_t *)args;
    struct oop_source_t *oop = oop_sys_new();

    oop_add_fd(oop, iq->pipefd[PIPE_READ], OOP_READ, in_notify, iq);

    oop_run(oop, 0);

    oop_del(oop);

    return NULL;
}

struct dd_manager_t *ddm_ini(int max_num)
{
    struct dd_manager_t *ddm = (struct dd_manager_t *)malloc(sizeof (struct dd_manager_t));
    if (ddm == NULL)
        return NULL;

    if (max_num <= 0)
        max_num = DEF_DD_NUM;
    ddm->dds = (struct dyndict_t *)malloc(max_num * sizeof (struct dyndict_t));
    if (ddm->dds == NULL)
    {
        free(ddm);
        return NULL;
    }

    ddm->max = max_num;
    ddm->num = 0;

    pthread_rwlock_init(&ddm->rwlock, NULL);

    struct trival_queue_t *tq = &ddm->iq.tq;
    tq->head = tq->tail = 0;
    tq->num = 0;
    pthread_mutex_init(&ddm->iq.mutex, NULL);
    pipe(ddm->iq.pipefd); 
    fcntl(ddm->iq.pipefd[PIPE_READ], F_SETFL, O_NONBLOCK);
    fcntl(ddm->iq.pipefd[PIPE_WRITE], F_SETFL, O_NONBLOCK);

    pthread_create(&ddm->oop_pid, NULL, oop_thread, &ddm->iq);

    ddm->magic = DDM_LIVE;

    return ddm;
}

// block until del complete
void ddm_fini(struct dd_manager_t *ddm)
{
    if (ddm == NULL)
        return;

    pthread_rwlock_wrlock(&ddm->rwlock);

    ddm->magic = DDM_FINI;

    int i;
    int check_num;
    struct dyndict_t *dd = NULL;
    char msg;
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        dd = &ddm->dds[i];
        // dd->stat == DD_DONE || DD_EMPTY only
        // or it won't release the lock to let us in
        if ((dd->stat & DD_STAT) == DD_DONE)
        {
            check_num++;

            dd->stat = DD_DEL;
            pthread_mutex_lock(&ddm->iq.mutex);

            tq_put(&ddm->iq.tq, dd);
            msg = CMD_DD;
            write(ddm->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));

            pthread_mutex_unlock(&ddm->iq.mutex);
        }
    }

    // CMD_EXIT is not CMD_DD
    // CMD_DD used in ddm_del, since we need to delete all dict
    // not care about dd->oop2dd, since we just close them
    // might cause a problem when it blocks
    pthread_mutex_lock(&ddm->iq.mutex);
    msg = CMD_EXIT;
    write(ddm->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));
    pthread_mutex_unlock(&ddm->iq.mutex);

    pthread_rwlock_unlock(&ddm->rwlock);

    // just wait for oop_thread exit
    // can't lock since unref might use ddm->rwlock
    pthread_join(ddm->oop_pid, NULL);
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        dd = &ddm->dds[i];
        if ((dd->stat & DD_STAT) == DD_DEL)
        {
            check_num++;

            close(dd->oop2dd[PIPE_READ]);
            close(dd->oop2dd[PIPE_WRITE]);
            pthread_rwlock_destroy(&dd->rwlock);

            close(dd->iq.pipefd[PIPE_READ]);
            close(dd->iq.pipefd[PIPE_WRITE]);
            pthread_mutex_destroy(&dd->iq.mutex);
        }
    }

    pthread_rwlock_destroy(&ddm->rwlock);
    ddm->magic = DDM_DEAD;
    free(ddm);
}

// load dict: dict = ini_fun(ini_filename);
// rem  dict: fini(dict);

// add:
// block until add is DD_DONE of DD_FAIL
// dict map to uniq slot in dds
int ddm_add(struct dd_manager_t *ddm, const char *name, int intval_s, void *(*ini_fun)(void *), void *ini_args, void (*fini_fun)(void *))
{
    if (ddm == NULL || ddm->magic != DDM_LIVE)
        return DDM_MEM;

    pthread_rwlock_wrlock(&ddm->rwlock);

    if (ddm->magic != DDM_LIVE)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_MEM;
    }

    if (ddm->num == ddm->max)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_OVERFLOW;
    }

    int i;
    int check_num;
    struct dyndict_t *target = NULL;
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        struct dyndict_t *dd = &ddm->dds[i];
        if (dd->stat == DD_EMPTY && target == NULL)
            target = dd;
        else if ((dd->stat & DD_STAT) == DD_DONE)
        {
            check_num++;
            if (strcmp(dd->name, name) == 0)
            {
                pthread_rwlock_unlock(&ddm->rwlock);
                return DDM_DUP;
            }
        }
    }

    if (target == NULL)
        target = &ddm->dds[check_num];


    ddm->num++;
    target->stat = DD_ADD;

    pthread_rwlock_unlock(&ddm->rwlock);

    target->name = name;
    target->ini_fun = ini_fun;
    target->ini_args = ini_args;
    target->fini_fun = fini_fun;
    target->intval_s = intval_s;

    pipe(target->oop2dd);
    pipe(target->iq.pipefd);

    char msg;

    pthread_mutex_lock(&ddm->iq.mutex);
    tq_put(&ddm->iq.tq, target);
    msg = CMD_DD;
    write(ddm->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));
    pthread_mutex_unlock(&ddm->iq.mutex);

    while (read(target->oop2dd[PIPE_READ], &msg, sizeof (char)))
    {
        if (msg == CMD_DD)
            break;
    }

    if (target->stat & DD_LOAD_FAIL)
    {
        close(target->oop2dd[PIPE_READ]);
        close(target->oop2dd[PIPE_WRITE]);
        close(target->iq.pipefd[PIPE_READ]);
        close(target->iq.pipefd[PIPE_WRITE]);
        target->stat = DD_FAIL;
        return DDM_MEM;
    }
    else
    {
        pthread_rwlock_init(&target->rwlock, NULL);
        pthread_mutex_init(&target->iq.mutex, NULL);
        target->iq.tq.head = target->iq.tq.tail = 0;
        target->iq.tq.num = 0;
        target->stat = DD_DONE;
        return DDM_OK;
    }
}

static int del_dd_from_ddm(struct dd_manager_t *ddm, struct dyndict_t *dd)
{
    char msg;
    pthread_mutex_lock(&ddm->iq.mutex);

    tq_put(&ddm->iq.tq, dd);
    msg = CMD_DD;
    write(ddm->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));

    pthread_mutex_unlock(&ddm->iq.mutex);

    while (read(dd->oop2dd[PIPE_READ], &msg, sizeof (char)))
    {
        if (msg == CMD_DD)
            break;
    }

    close(dd->oop2dd[PIPE_READ]);
    close(dd->oop2dd[PIPE_WRITE]);
    pthread_rwlock_destroy(&dd->rwlock);
    pthread_mutex_destroy(&dd->iq.mutex);
    close(dd->iq.pipefd[PIPE_READ]);
    close(dd->iq.pipefd[PIPE_WRITE]);
    return 0;
}

int ddm_del(struct dd_manager_t *ddm, const char *name)
{
    if (ddm == NULL || ddm->magic != DDM_LIVE)
        return DDM_MEM;
    pthread_rwlock_wrlock(&ddm->rwlock);

    if (ddm->magic != DDM_LIVE)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_MEM;
    }

    if (ddm->num == 0)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_NODICT;
    }

    int i;
    int check_num;
    struct dyndict_t *dd = NULL;
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        dd = &ddm->dds[i];
        if (dd->stat == DD_DONE)
        {
            check_num++;
            if (strcmp(dd->name, name) == 0)
                break;
        }
    }

    if (i < ddm->max && check_num < ddm->num)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_NODICT;
    }

    dd->stat = DD_DEL;
    pthread_rwlock_unlock(&ddm->rwlock);

    del_dd_from_ddm(ddm, dd);

    dd->stat = DD_EMPTY;

    return DDM_OK;
}


void *ddm_ref(struct dd_manager_t *ddm, const char *name)
{
    if (ddm == NULL || ddm->magic != DDM_LIVE)
        return NULL;

    pthread_rwlock_rdlock(&ddm->rwlock);

    if (ddm->magic != DDM_LIVE)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return NULL;
    }

    if (ddm->num == 0)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return NULL;
    }

    int i;
    int check_num;
    struct dyndict_t *dd = NULL;
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        dd = &ddm->dds[i];
        if ((dd->stat & DD_STAT) == DD_DONE)
        {
            check_num++;
            if (strcmp(dd->name, name) == 0)
                break;
        }
    }

    if (i < ddm->max && check_num < ddm->num)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return NULL;
    }

    void *dict;
    char msg;
    pthread_rwlock_rdlock(&dd->rwlock);

    dict = dd->dicts[dd->index];
    pthread_mutex_lock(&dd->iq.mutex);
    tq_put(&dd->iq.tq, dict);
    msg = DD_REF;
    write(dd->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));
    pthread_mutex_unlock(&dd->iq.mutex);

    pthread_rwlock_unlock(&dd->rwlock);

    pthread_rwlock_unlock(&ddm->rwlock);

    return dict;
}

// unref 可以在DD_FINI下操作,因为fini需要unref来减少索引
// 以便可以安全的删除词典索引
int ddm_unref(struct dd_manager_t *ddm, const char *name, void *dict)
{
    if (ddm == NULL || ddm->magic == DDM_DEAD)
        return DDM_MEM;

    pthread_rwlock_rdlock(&ddm->rwlock);

    if (ddm->magic == DDM_DEAD)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_MEM;
    }

    if (ddm->num == 0)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_NODICT;
    }

    int i;
    int check_num;
    struct dyndict_t *dd = NULL;
    for (i = 0, check_num = 0; i < ddm->max && check_num < ddm->num; i++)
    {
        dd = &ddm->dds[i];
        int stat = dd->stat & DD_STAT;
        if (stat == DD_DONE || (stat == DD_DEL && ddm->magic == DDM_FINI))
        {
            check_num++;
            if (strcmp(dd->name, name) == 0)
                break;
        }
    }

    if (i < ddm->max && check_num < ddm->num)
    {
        pthread_rwlock_unlock(&ddm->rwlock);
        return DDM_NODICT;
    }

    char msg;

    pthread_mutex_lock(&dd->iq.mutex);
    tq_put(&dd->iq.tq, dict);
    msg = DD_UNREF;
    write(dd->iq.pipefd[PIPE_WRITE], &msg, sizeof (char));
    pthread_mutex_unlock(&dd->iq.mutex);

    pthread_rwlock_unlock(&ddm->rwlock);

    return DDM_OK;
}

