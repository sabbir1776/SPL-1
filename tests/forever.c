#include <stdio.h>

int main(void)
{
    int i = 0;
    int count = 0;

    printf("starting loop\n");
    while (i < 10) {
        count++;
    }
    printf("done %d\n", count);
    return 0;
}
