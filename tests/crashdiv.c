#include <stdio.h>

int divide(int a, int b)
{
    return a / b;
}

int main(void)
{
    int zero = 0;

    printf("10 / 2 = %d\n", divide(10, 2));
    printf("10 / 0 = %d\n", divide(10, zero));
    return 0;
}
