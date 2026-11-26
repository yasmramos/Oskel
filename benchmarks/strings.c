// Benchmark: build n short strings, join them with a comma, split them back and
// sum their lengths. Same algorithm as `strings.oskl`; the join and the split
// are written by hand because C has neither.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const int n = 5000;
  char** parts = malloc((size_t)n * sizeof *parts);
  if (parts == NULL) return 70;
  size_t total = 0;
  for (int i = 0; i < n; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof buf, "item-%d", i);
    parts[i] = malloc((size_t)len + 1);
    if (parts[i] == NULL) return 70;
    memcpy(parts[i], buf, (size_t)len + 1);
    total += (size_t)len;
  }

  size_t text_len = total + (size_t)(n - 1);
  char* text = malloc(text_len + 1);
  if (text == NULL) return 70;
  size_t at = 0;
  for (int i = 0; i < n; i++) {
    size_t len = strlen(parts[i]);
    memcpy(text + at, parts[i], len);
    at += len;
    if (i + 1 < n) text[at++] = ',';
  }
  text[at] = '\0';

  long words = 0;
  long chars = 0;
  size_t start = 0;
  for (size_t i = 0; i <= text_len; i++) {
    if (i == text_len || text[i] == ',') {
      words++;
      chars += (long)(i - start);
      start = i + 1;
    }
  }
  printf("strings %zu %ld %ld\n", text_len, words, chars);

  for (int i = 0; i < n; i++) free(parts[i]);
  free(parts);
  free(text);
  return 0;
}
