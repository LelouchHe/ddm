#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <oop.h>

#include "dyndict_manager.h"

#define DD_EMPTY 0x0
#define DD_ADD 0x1
#define DD_DEL 0x2
#define DD_DONE 0x4
#define DD_FAIL 0x8
#define DD_LOAD_FAIL 0x10
#define DD_NEED_RELOAD 0x20

#define DD_REF 'R'
#define DD_UNREF 'U'

#define DDM_ALIVE 
#define DDM_DEAD

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

    num--;
    void *ret = tq->data[tq->head];
    tq->head++;
    if (tq->head >= MAX_QUEUE_SIZE)
        tq->head = 0;

    return ret;
}

int tq_put(struct trival_queue_t *tq, void *d)
{
    if (num > MAX_QUEUE_SIZE)
        return -1;

    num++; 
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
    // 通知oop的dd的ref/unref操作
    // 队列为dict
    struct info_queue_t iq;
    // 通知ddm的dd首次加载完毕或卸载完毕
    int oop2dd[PIPE_NUM];

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

    struct info_queue_t iq;
    pthread_t oop_pid;

    uint64_t magic;
};

static int load_dd(oop_source_t *oop, struct dyndict_t *dd, int next)
{
    int ret = 0;
    if (dd->dicts[next] != NULL && dd->fini_fun != NULL)
        dd->fini_fun(dd->dict[next]);
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
        write(dd->oop2dd[PIPE_WRITE], CMD_DD, sizeof (char));

    gettimeofday(&dd->reload_tv, NULL);
    dd->reload_tv.tv_sec += dd->intval_s;
    oop_add_time(oop, dd->reload_tv, reload, dd);

    return ret;
}

static int check_if_reload(oop_source_t *oop, struct dyndict_t *dd, int pos)
{
    if (dd->stat & DD_NEED_RELOAD)
    {
        if (load_dd(oop, dd, pos) == 0)
            dd->stat &= ~DD_NEED_RELOAD;
    }

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
                count[i]++;
            else if (msg == DD_UNREF)
            {
                count[i]--;
                if (count[i] == 0)
                    check_if_reload(oop, dd, i);
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
        write(dd->oop2dd[PIPE_WRITE], CMD_DD, sizeof (char));
        return -1;
    }

    return 0;
}

static int remove_time(oop_source_t *oop, struct trival_queue_t *tq)
{
    struct dyndict_t *dd;
    while ((dd = (struct dyndict_t *)tq_get(tq)) != NULL)
        del_dd(oop, dd);

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
            remove_time(oop, &iq->tq);
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
        max_num = DEF_DICT_NUM;
    ddm->dds = (struct dyndict_t *)malloc(max_num * sizeof (struct dyndict_t));
    if (ddm->dds == NULL)
    {
        free(ddm);
        return NULL;
    }

    ddm->max = max_num;
    ddm->num = 0;

    pthread_rwlock_init(&ddm->rwlock, NULL);

    struct trival_queue_t *tq = ddm->iq.tq;
    tq->head = tq->tail = 0;
    tq->num = 0;
    pthread_mutex_init(&ddm->iq.mutex, NULL);
    pipe2(ddm->iq.pipefd, O_NONBLOCK); 

    pthread_create(&ddm->oop_pid, NULL, oop_thread, &ddm->iq);

    return ddm;
}

void ddm_fini(struct dd_manager_t *ddm);

// load dict: dict = ini_fun(ini_filename);
// rem  dict: fini(dict);

// add:
//
int ddm_add(struct dd_manager_t *ddm, const char *name, void *(*ini_fun)(void *), void *ini_args, void (*fini_fun)(void *));
int ddm_del(struct dd_manager_t *ddm, const char *name);

void *ddm_get(struct dd_manager_t *ddm, const char *name);






















