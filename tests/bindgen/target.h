/* The C types whose width depends on the data model. `bindgen --target`
 * selects LP64 (the default) or LLP64; `long long` is 64-bit in both. */
long wide(void);
unsigned long uwide(void);
long long wider(void);
unsigned long long uwider(void);
wchar_t wide_char(void);
