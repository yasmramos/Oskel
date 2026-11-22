// A shared library no system library provides, so `--link-lib` is what makes
// the symbol resolve. The own linker reads this file's `.dynsym` and emits its
// DT_SONAME (`liboskel_link_lib_helper.so.1`); the test then runs the linked
// binary with `LD_LIBRARY_PATH` pointing at this directory.
extern "C" int oskel_link_lib_answer() { return 41; }
