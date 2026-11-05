#include <stddef.h>

extern int module_start(size_t args, const void* argp);
extern int module_stop(size_t args, const void* argp);

__asm__(
    ".intel_syntax noprefix\n"
    ".align 0x8\n"
    ".section \".data.sce_module_param\"\n"
    "_sceProcessParam:\n"
    ".quad 0x18\n"
    ".quad 0x13c13f4bf\n"
    ".quad 0x4508101\n"
    ".att_syntax prefix\n");

__asm__(
    ".intel_syntax noprefix\n"
    ".align 0x8\n"
    ".data\n"
    "__dso_handle:\n"
    ".quad 0\n"
    "_sceLibc:\n"
    ".quad 0\n"
    ".att_syntax prefix\n");

int _init(size_t args, const void* argp) {
  return module_start(args, argp);
}

int _fini(size_t args, const void* argp) {
  return module_stop(args, argp);
}
