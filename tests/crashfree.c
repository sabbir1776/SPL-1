#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int *p = malloc(sizeof(int) * 4);

    p[0] = 1;
    free(p);
    free(p);
    printf("done\n");
    return 0;
}
