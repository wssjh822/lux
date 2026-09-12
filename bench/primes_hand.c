// 手写 C 版本，用于和 Lux 生成的代码做性能对比（算法完全一致）
#include <stdio.h>
#include <stdbool.h>

static bool is_prime(long long n) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (n % 2 == 0) return false;
    long long i = 3;
    while (i * i <= n) {
        if (n % i == 0) return false;
        i += 2;
    }
    return true;
}

int main(void) {
    long long count = 0;
    for (long long n = 2; n <= 2000000; n++) {
        if (is_prime(n)) count++;
    }
    printf("2..2000000 之间共有 %lld 个素数\n", count);
    return 0;
}
