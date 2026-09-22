#pragma once

// SDK-native ELF injector.
//
// Maps a position-independent ELF (the feature-menu worker) into a target process
// and starts its exported entry on a new thread -- the etaHEN/libhijacker-free
// replacement for the old elf_injector.cpp + ps5debug inject path. It does the work
// a full module loader would: reserve an executable image, copy each PT_LOAD, apply
// R_X86_64_RELATIVE relocations, resolve dynamic imports against the ELF's
// DT_NEEDED libraries, then start_thread at the requested export.
//
// All target access goes through the process-control backend (proc_backend.h):
// gtav_proc_alloc_exec / gtav_proc_write / gtav_proc_resolve_named /
// gtav_proc_start_thread. The ELF parsing + relocation math is pure, so the whole
// path is host-tested with a stub backend over a real ELF (test_elf_inject_static.py).
//
// Mirrors the host-side plan tool research/tools/offline/elf_map_plan.py. Supports little-endian
// ELF64 x86-64 with relocation types RELATIVE / GLOB_DAT / JUMP_SLOT / 64 and a
// 0x4000 page size. Returns 0 on success and -1 on failure (details in *out).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavElfInjectResult {
  int status;                     // 0 ok, nonzero stage that failed (see source)
  uintptr_t base;                 // load bias / image base, or zero when its return was unobserved
  uint32_t allocation_may_exist;  // remote mmap succeeded or may have run; quarantine this pid
  uintptr_t entry;                // resolved entry address in the target
  uint32_t image_size;            // bytes reserved
  uint32_t relocs_applied;        // R_X86_64_RELATIVE count
  uint32_t imports_resolved;      // dynamic imports written
  int tid;                        // start_thread handle (informational)
  char error[160];                // human-readable failure description ("" on success)
} GtavElfInjectResult;

// Inject the ELF image at elf[0..len) into pid and start entry_symbol on a new thread. arg is
// passed to the entry (use 0 for the menu worker). expected_app_id binds every remote stage to one
// process launch and must be the nonzero gtav_proc_app_id(pid) captured by the caller; 0 disables
// this guard only for host tests/standalone users. *out is always populated. Returns 0 on success,
// -1 on failure.
int gtav_elf_inject(int pid, const uint8_t* elf, size_t len, const char* entry_symbol,
                    uintptr_t arg, uint64_t expected_app_id, GtavElfInjectResult* out);

// Stop points inside the map half, for the injection-stage measurement lane (GTAV_LOADER_STAGE).
#define GTAV_ELF_MAP_FULL 0
#define GTAV_ELF_MAP_STOP_AFTER_ALLOC 1

// As gtav_elf_map_and_relocate, but stop_after selects how far to go. GTAV_ELF_MAP_STOP_AFTER_ALLOC
// returns as soon as the image reserve exists (remote mmap + the RWX protection change), with no
// segment bytes written and no relocations applied -- the boundary that separates "the allocation
// damaged the host" from "writing the image damaged the host". *out carries base/image_size as
// usual. Production callers use GTAV_ELF_MAP_FULL.
int gtav_elf_map_and_relocate_staged(int pid, const uint8_t* elf, size_t len,
                                     uint64_t expected_app_id, int stop_after,
                                     GtavElfInjectResult* out);

// Map and relocate into memory the caller already owns in the target, skipping allocation
// entirely. On firmware where PT_ATTACH breaks the game (docs/ptrace-free-injection.md) the base
// comes from the cave bootstrap, and with GTAV_PROC_NOSTOP_IO the segment and relocation writes go
// through the kernel too, so the whole map runs without ever stopping the target. `base` must be at
// least image_size bytes and writable+executable. `reserved` is the size of that region: the image
// is refused before a single byte is written if it does not fit (0 = no bound known). Returns 0 on
// success, -1 on failure.
int gtav_elf_map_and_relocate_at(int pid, const uint8_t* elf, size_t len, uint64_t expected_app_id,
                                 uintptr_t base, uint64_t reserved, GtavElfInjectResult* out);

// Shared implementation behind the map entry points: `preallocated` of 0 allocates as before, and
// `reserved` bounds a preallocated region (ignored when allocating).
int gtav_elf_map_and_relocate_into(int pid, const uint8_t* elf, size_t len,
                                   uint64_t expected_app_id, int stop_after, uintptr_t preallocated,
                                   uint64_t reserved, GtavElfInjectResult* out);

// Map and relocate the ELF image, but do not start any thread. This is the first half of the
// broker-first/worker-last split: it leaves the image fully resolved in the target address space
// so the caller can read loader-derived symbols (e.g. gtav_frame_hook_thunk_value) from target
// memory before deciding whether to start the worker. *out is populated with base, image_size,
// relocs_applied, imports_resolved. Returns 0 on success, -1 on failure.
int gtav_elf_map_and_relocate(int pid, const uint8_t* elf, size_t len, uint64_t expected_app_id,
                              GtavElfInjectResult* out);

// Start the already-mapped image's entry_symbol on a new thread. *out must come from a prior
// successful gtav_elf_map_and_relocate call. This is the second half of the
// broker-first/worker-last split; it performs find_export + start_thread only. Returns 0 on
// success, -1 on failure.
int gtav_elf_start_thread(int pid, const uint8_t* elf, size_t len, const char* entry_symbol,
                          uintptr_t arg, uint64_t expected_app_id, GtavElfInjectResult* out);

// Look up a defined .dynsym symbol's vaddr (st_value) in the ELF image. Returns 0
// and writes *value_out on success, -1 if absent. Used to probe an injected image.
int gtav_elf_symbol_value(const uint8_t* elf, size_t len, const char* name, uint64_t* value_out);

#ifdef __cplusplus
}
#endif
