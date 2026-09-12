# Python 版本，用于和 Lux 做性能对比（算法完全一致）
def is_prime(n):
    if n < 2:
        return False
    if n == 2:
        return True
    if n % 2 == 0:
        return False
    i = 3
    while i * i <= n:
        if n % i == 0:
            return False
        i += 2
    return True


count = sum(1 for n in range(2, 2000001) if is_prime(n))
print(f"2..2000000 之间共有 {count} 个素数")
