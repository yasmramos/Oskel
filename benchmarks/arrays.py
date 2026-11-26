"""Benchmark: append n integers to an array, then sum them.

Same algorithm as `arrays.oskl`: a list and the language's own append.
"""

n = 200000
xs = []
for i in range(n):
    xs.append(i)

print(f"arrays {sum(xs)} {len(xs)}")
