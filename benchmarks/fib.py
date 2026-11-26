"""Benchmark: naive recursive fibonacci. Same algorithm as `fib.oskl`."""


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


print(f"fib {fib(30)}")
