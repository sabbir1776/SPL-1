#include <stdio.h>

int forever(int n)
{
    return forever(n + 1) + 1;
}

int main(void)
{
    printf("%d\n", forever(0));
    return 0;
}
