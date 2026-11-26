"""Benchmark: build n short strings, join them with a comma, split them back and
sum their lengths. Same algorithm as `strings.oskl`."""

n = 5000
parts = [f"item-{i}" for i in range(n)]
text = ",".join(parts)
words = text.split(",")
print(f"strings {len(text)} {len(words)} {sum(len(w) for w in words)}")
