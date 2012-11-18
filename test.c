#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "dyndict_manager.h"

#define MAX_BUF_SIZE 1024

struct dict_t
{
    char str[MAX_BUF_SIZE];
};

void *ini(void *args)
{
    printf("load me\n");

    struct dict_t *d = (struct dict_t *)malloc(sizeof (struct dict_t));

    char *name = (char *)args;

    FILE *file = fopen(name, "r");
    fgets(d->str, MAX_BUF_SIZE, file);
    d->str[strlen(d->str) - 1] = '\0';
    printf("in ini\t%s\n", d->str);
    fclose(file);

    return d;
}

void fini(void *args)
{
    printf("unload me\n");

    struct dict_t *d = (struct dict_t *)args;
    free(d);
}

void *another_thread(void *args)
{
    struct dd_manager_t *ddm = (struct dd_manager_t *)args;
    while (1)
    {
        struct dict_t *d = ddm_ref(ddm, "test");
        printf("%s\n", d->str);
        sleep(5);
        ddm_unref(ddm, "test", d);
    }

    return NULL;
}

int main()
{
    struct dd_manager_t *ddm = ddm_ini(100);

    ddm_add(ddm, "test", 1, ini, "sample.dict", fini);
    ddm_add(ddm, "test2", 1, ini, "sample.dict", fini);

    pthread_t pid;
    pthread_create(&pid, NULL, another_thread, ddm);

    while (1)
    {
        struct dict_t *d = ddm_ref(ddm, "test");
        printf("%s\n", d->str);
        sleep(2);
        ddm_unref(ddm, "test", d);
    }

    printf("before ddm_fini\n");
    ddm_fini(ddm);
    printf("after ddm_fini\n");
    return 0;
}
