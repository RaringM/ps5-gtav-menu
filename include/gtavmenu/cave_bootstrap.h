#pragma once

// Ptrace-free bootstrap probe (docs/ptrace-free-injection.md).
//
// PT_ATTACH permanently breaks GTA's streaming I/O, so the loader cannot obtain memory in the
// target the way it does today. This installs the `cave_stub.h` bootstrap into the executable
// segment's tail padding over mdbg, detours PLAYER_PED_ID to it, and waits for the stub to publish
// the result of an mmap it performed on the game thread. Everything it does -- symbol resolution,
// memory reads/writes, protection changes -- is already verified stop-free against a live game.
//
// It is a PROBE: it restores the prologue on every armed path. Once target-side completion is
// proven it also re-zeroes the cave and puts .text back. If split completion remains ambiguous it
// deliberately preserves the possible execution destination and RWX protection until this
// terminal process exits. It does not map the worker. Its job is to answer whether an allocation
// can be made from inside the frame hook.

#include <stddef.h>
#include <stdint.h>

#include "gtavmenu/build_pin.h"

// Largest pinned prologue this probe will handle; matches the build-pin contract's own bound.
#define GTAV_CAVE_BROKER_EXPECTED_MAX GTAV_BUILD_PIN_BROKER_EXPECTED_BYTES_N

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavCaveBootstrapResult {
  uint64_t allocation;   // what the stub's mmap returned, 0 if it never ran
  int64_t split_result;  // target mprotect result; INT64_MIN while split completion is unknown
  uint64_t gadget;       // the resolved libkernel syscall gadget
  uint64_t cave;         // where the stub was installed
  uint32_t waited_ms;    // how long the poll took to see a result
  int stub_ran;          // the one-shot flag latched, so the stub executed
  int split_completed;   // split syscall returned and its result was fully published
  int restored;          // prologue, cave and .text protection were all put back
} GtavCaveBootstrapResult;

// Change a protection with retries, logging each failed attempt. Protection changes fail
// transiently on this firmware -- `status=0xf0000001` once, and `errno=14` (EFAULT) on the freshly
// mmap'd bootstrap region during an otherwise healthy inject -- and leaving the target half-way
// through a protection change is far worse than a slow retry, so nothing on this path may
// fire-and-forget. Returns 0 once a change succeeds, -1 when every attempt failed.
int gtav_cave_protect_retry(int pid, uintptr_t addr, size_t n, int prot, const char* what);
int gtav_cave_write_verified(int pid, uintptr_t addr, const uint8_t* buf, uint32_t n,
                             const char* what);

// Run the probe against `pid` using `pin`'s frame-hook contract. `cave` is the install address
// inside the executable segment's padding, `alloc_size` the number of bytes to ask mmap for, and
// `timeout_ms` how long to wait for the stub to publish. *out is always populated. Returns 0 when
// the stub ran AND published a plausible allocation, -1 otherwise.
int gtav_cave_bootstrap_probe(int pid, const GtavBuildPin* pin, uint64_t cave, uint32_t alloc_size,
                              uint32_t timeout_ms, GtavCaveBootstrapResult* out);

// Same allocation bootstrap, but after prefaulting asks the target's own mprotect path to make the
// first split_readonly_size bytes read-only. That operation creates a real vm_map boundary; the
// host can then add execute only to the first entry without affecting the trailing RW data entry.
// Used only by the disabled cave-prefix equivalence lane.
int gtav_cave_bootstrap_probe_split(int pid, const GtavBuildPin* pin, uint64_t cave,
                                    uint32_t alloc_size, uint32_t split_readonly_size,
                                    uint32_t timeout_ms, GtavCaveBootstrapResult* out);

#ifdef __cplusplus
}
#endif
