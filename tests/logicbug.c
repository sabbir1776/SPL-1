#include <stdio.h>

int sum_to(int n)
{
    int total = 0;

    for (int i = 1; i < n; i++)
        total += i;
    return total;
}

int main(void)
{
    int result = sum_to(5);

    printf("sum of 1..5 = %d\n", result);
    return 0;
}
