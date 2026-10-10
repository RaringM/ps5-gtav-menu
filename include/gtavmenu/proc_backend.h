#pragma once

// Process-control / injection backend.
//
// One small C ABI the loader uses to reach the running game process: find it,
// elevate it enough to write its memory, resolve symbols inside it, read/write
// its memory, and (later) allocate executable memory and start a remote thread.
//
// The interface is deliberately enabler-agnostic. The default implementation
// (src/common/proc_backend_sdk.c) is built on the in-repo ps5-payload-sdk
// primitives (ps5/kernel.h) plus FreeBSD ptrace -- no etaHEN, no libhijacker.
//
// Pure helpers (gtav_proc_elevation_plan, gtav_proc_symbol_name) carry no SDK
// dependency so they compile and are unit-tested on the host. The remaining
// operations are hardware glue implemented by the SDK backend.
//
// Error convention: 0 on success, -1 on failure. gtav_proc_last_error() returns
// a human-readable string describing the most recent failure.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logical symbols the loader resolves inside the target process. Kept as an enum
// (not raw names at call sites) so the lib/symbol mapping lives in one table that
// host tests can validate.
typedef enum {
  GTAV_PROC_SYM_SCE_PAD_READ_STATE = 0,
  GTAV_PROC_SYM_LOAD_START_MODULE,
  GTAV_PROC_SYM_DLSYM,
  GTAV_PROC_SYM_MMAP,            // remote page allocation (gtav_proc_alloc_exec)
  GTAV_PROC_SYM_PTHREAD_CREATE,  // remote thread start (gtav_proc_start_thread)
  GTAV_PROC_SYM__COUNT
} GtavProcSym;

// Credentials applied to the target ucred so debug writes (ptrace / kernel R/W)
// are permitted. Mirrors the proven etaHEN loader values; applied through the
// SDK's kernel_set_ucred_* helpers, which abstract the fragile field offsets.
typedef struct GtavProcElevation {
  uint64_t authid;    // sce auth id granting debug authority
  uint8_t caps[16];   // sce capability bitmap (all-set == unrestricted)
  uint8_t attrs[32];  // sce attribute bitmap (unsandbox flag set in byte 0)
  int relocate_root;  // also point rootdir/jaildir at the root vnode (escape jail)
} GtavProcElevation;

// vm protection bits as understood by kernel_set_vmem_protection().
#define GTAV_PROC_PROT_READ 0x1
#define GTAV_PROC_PROT_WRITE 0x2
#define GTAV_PROC_PROT_EXEC 0x4

// ---- Pure helpers (host-testable, no SDK dependency) --------------------------

// Fill *out with the elevation parameters to apply to a target ucred.
int gtav_proc_elevation_plan(GtavProcElevation* out);

// Map a logical symbol to the (library basename, symbol name) the backend feeds
// to kernel_dynlib_handle()/kernel_dynlib_dlsym(). Returns 0 and sets *lib_out
// and *sym_out on success; -1 for an unknown id.
int gtav_proc_symbol_name(GtavProcSym sym, const char** lib_out, const char** sym_out);

// ---- Backend operations (implemented by the SDK backend) ----------------------

// Bind kernel R/W from the payload_args handed to _start(). Must be called once
// before any other backend op. Returns -1 if the payload was not launched with a
// valid kernel R/W context.
int gtav_proc_backend_init(void);

// Find the running big-app whose title id matches title_id (e.g. "PPSA04264").
// Writes its pid to *pid_out. Returns -1 if not running.
int gtav_proc_find_game(const char* title_id, int* pid_out);

// Read one coherent foreground big-app identity. Rechecks title, app id and pid
// after enumeration; a launch/foreground change fails instead of returning a mix.
int gtav_proc_foreground(char* title_id, size_t title_capacity, int* pid_out, uint64_t* token_out);

// Per-instance identity for `pid`: the system app id, which is unique per app launch. Lets a caller
// distinguish a recycled-same-pid NEW game instance from the original (the app id changes across a
// relaunch even if the pid is reused). Returns the app id (>0) or 0 if unknown -- the pid is not a
// running app, or the query failed. Callers treat 0 as "unknown" (never a mismatch).
uint64_t gtav_proc_app_id(int pid);

// Per-launch token derived from the target kernel proc's immutable pstats.p_start timestamp. This
// is the shared on-disk ownership identity for production and guarded ps5debug injection; app id
// remains the production transaction identity. The allproc walk is deliberately uncached so a
// recycled pid cannot inherit a stale proc pointer. Returns 0 when the process/start record cannot
// be verified.
uint64_t gtav_proc_start_token(int pid);

// Apply gtav_proc_elevation_plan() to the target's ucred.
int gtav_proc_elevate(int pid);

// Resolve a logical symbol to its runtime address inside the target.
int gtav_proc_resolve_sym(int pid, GtavProcSym sym, uintptr_t* addr_out);

// Resolve an arbitrary symbol by name, trying each candidate library basename (e.g.
// the injected ELF's DT_NEEDED list) in order. Writes the runtime address to
// *addr_out. Returns 0 on the first hit, -1 if no library exports it.
// Resolve libkernel's `syscall; ret` gadget inside the target. PS5 rejects a syscall instruction
// that does not originate from libkernel, so code injected into the game must call through this
// rather than emitting its own. Resolution is a kernel dynlib walk -- no ptrace, so it is safe
// against a live game. Returns 0 and writes *addr_out, or -1 when the gadget cannot be resolved.
int gtav_proc_resolve_syscall_gadget(int pid, uintptr_t* addr_out);

int gtav_proc_resolve_named(int pid, const char* const* libs, int nlibs, const char* name,
                            uintptr_t* addr_out);

// Read/write target memory (ptrace-backed). n may exceed one word.
int gtav_proc_read(int pid, uintptr_t addr, void* buf, size_t n);
int gtav_proc_write(int pid, uintptr_t addr, const void* buf, size_t n);

// Read/write target memory WITHOUT stopping it: the debug-memory path (mdbg), which copies through
// the kernel and never PT_ATTACHes. Measured on 01.010.002: a handful of PT_ATTACH/PT_DETACH cycles
// against a live GTA permanently jams its streaming I/O ring (reads stay submitted, completions
// never arrive), while hundreds of mdbg reads from the host disturb nothing -- see
// docs/ptrace-free-injection.md. These also ignore page protections, so a caller reading
// execute-only .text needs no gtav_proc_protect first. Returns 0 on success, -1 if the debug-memory
// path is unavailable or the transfer failed; callers decide whether to fall back.
int gtav_proc_read_nostop(int pid, uintptr_t addr, void* buf, size_t n);
int gtav_proc_write_nostop(int pid, uintptr_t addr, const void* buf, size_t n);

// Read a one-level pointer chain under a SINGLE ptrace attach (one whole-game stop instead of two):
//   base = *(uint64_t*)anchor;  then, if base != 0, read `n` bytes (1..8) at base + offset.
// This is the watch-lane SP-ready read *(*(anchor)+offset); folding both reads into one attach
// halves the per-poll PT_ATTACH freeze. Writes the intermediate base to *base_out (may be NULL);
// base == 0 means the pointer is still null (singleton not up) -- NOT an error. Writes the final
// value to *value_out only when base != 0 and the second read lands, setting *value_read (may be
// NULL) to 1 in that case. Returns 0 if the base read reached the target (reachable this poll, even
// if base is null or the value read failed), -1 if the base read itself failed (target
// unreachable).
int gtav_proc_read_chain(int pid, uintptr_t anchor, uintptr_t offset, size_t n, uint64_t* base_out,
                         void* value_out, int* value_read);

// Change protection on a range of the target's address space.
int gtav_proc_protect(int pid, uintptr_t addr, size_t n, int prot);

// Allocate an executable page inside the target: a remote anonymous mmap (via a
// ptrace register-injection call) made executable through kernel_set_vmem_protection
// (PS5 enforces W^X, so the RW page is re-protected RWX the same way pad_hook.c
// re-protects .text for its live patch). Initializes *addr_out and
// *allocation_may_exist_out (when non-NULL) to zero. If remote mmap returns an address, that
// address is published even when a later detach/protection step fails. allocation_may_exist is also
// set when the syscall may have executed but its return address could not be recovered, allowing a
// caller to quarantine the process even when *addr_out remains zero. Runtime-validated on console
// only.
int gtav_proc_alloc_exec(int pid, size_t n, uintptr_t* addr_out, int* allocation_may_exist_out);

// Start a remote thread in the target at entry(arg) via a ptrace register-injection
// call to scePthreadCreate. *tid_out (if non-NULL) receives the low bits of the
// returned ScePthread handle (informational, not a real tid). Runtime-validated on
// console only.
int gtav_proc_start_thread(int pid, uintptr_t entry, uintptr_t arg, int* tid_out);

// Load + start a PRX inside the target via sceKernelLoadStartModule (run on a
// PT_CONTINUE'd thread, since it does file I/O + module init). Writes the returned
// module id to *module_id_out. Used to load the game-thread command-hook PRX that
// drains queued vehicle spawns on the script thread -- without etaHEN.
int gtav_proc_load_module(int pid, const char* path, int* module_id_out);

// Runtime mapped base of a loaded module (kernel_dynlib_mapbase_addr), so a caller
// can read that module's globals at base+offset. Returns 0 on failure.
uintptr_t gtav_proc_module_base(int pid, int module_id);

// Human-readable description of the most recent failure ("none" before any).
const char* gtav_proc_last_error(void);

#ifdef __cplusplus
}
#endif
