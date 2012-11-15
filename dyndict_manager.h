#ifndef _DYNDICT_MANAGER_H
#define _DYNDICT_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#define DDM_OK 0
#define DDM_MEM -1
#define DDM_OVERFLOW -2
#define DDM_DUP -3
#define DDM_UNIMPLEMENTED -4
#define DDM_UNKNOWN -5

struct dd_manager_t;

struct dd_manager_t *ddm_ini(int max_num);
void ddm_fini(struct dd_manager_t *ddm);

// load dict: dict = ini_fun(ini_filename);
// rem  dict: fini(dict);
// 必须等待对应dd加载成功才返回(oop2dd)
int ddm_add(struct dd_manager_t *ddm, const char *name, void *(*ini_fun)(void *), void *ini_args, void (*fini_fun)(void *));
// 必须等待对应dd删除成功才返回(oop2dd)
int ddm_del(struct dd_manager_t *ddm, const char *name);

// 无需管理同步,只需在dd2oop中添加当前的dict即可
// 不直接处理count
// 只处理index(rlock)
void *ddm_ref(struct dd_manager_t *ddm, const char *name);
int ddm_unref(struct dd_manager_t *ddm, const char *name, void *dict);

#ifdef __cplusplus
}
#endif

#endif
