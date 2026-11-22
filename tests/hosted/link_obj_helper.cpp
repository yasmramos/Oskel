// A plain C-ABI helper object for `oskel.build_hosted_own_link_obj`.
//
// `oskel build --emit-object` produces the main object; `oskelld` links this
// object beside it, and `oskel build --link-object` does the same through the
// in-process linker. It deliberately has no data, no calls and no string
// literals: PHASE 10.2 loads `.text` and `.rodata` only.
extern "C" int oskel_link_obj_helper() { return 42; }
