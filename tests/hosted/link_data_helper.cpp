// A C-ABI helper with .rodata, .data and .bss for PHASE 10.2b.
//
// `oskel_link_message` returns a string literal (a .rodata section symbol plus
// an addend), `oskel_link_counter` is an initialised .data global,
// `oskel_link_zero` is a .bss global, and `oskel_link_names` is a pointer table
// in .data whose entries are R_X86_64_64 relocations against .rodata. The
// second copy of this object is built with -fPIC so the globals travel through
// the GOT (R_X86_64_REX_GOTPCRELX).
extern "C" {
const char *oskel_link_message(void) { return "data ok"; }
int oskel_link_counter = 7;
int oskel_link_bump(void) { return ++oskel_link_counter; }
int oskel_link_zero;
int oskel_link_zero_value(void) { return oskel_link_zero; }
const char *oskel_link_names[] = {"alpha", "beta"};
const char *oskel_link_name(int index) { return oskel_link_names[index]; }
}
