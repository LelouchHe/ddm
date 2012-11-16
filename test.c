#include <stdio.h>

#include "dyndict_manager.h"

char buf[1024];

void *ini(const char *name)
{
    printf("load me\n");

    FILE *file = fopen(name, "r");
    fgets(buf, 1024 - 1, file);
    fclose(file);

    return buf;
}

void fini(void *some)
{
    printf("unload me\n");
    return 0;
}

int main()
{
    struct dd_manager_t *ddm = ddm_ini(100);

    ddm_add(ddm, "test", 10, ini, "some", fini);

    const char *dict = (const char *)ddm_get(ddm, "test");
    printf("%s\n", dict);

    getchar();

    dict = (const char *)ddm_ref(ddm, "test");
    printf("%s\n", dict);

    ddm_fini(ddm);
    return 0;
}
