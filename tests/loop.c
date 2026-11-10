#include <stdio.h>

int square(int x)
{
    return x * x;
}

int main(void)
{
    int sum = 0;

    for (int i = 1; i <= 3; i++) {
        sum += square(i);
        printf("i=%d sum=%d\n", i, sum);
    }
    return 0;
}
