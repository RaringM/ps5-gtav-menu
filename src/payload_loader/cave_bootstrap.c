// Ptrace-free bootstrap probe. See include/gtavmenu/cave_bootstrap.h.
//
// Every target access here goes through the *_nostop primitives (mdbg) or
// kernel_set_vmem_protection (a vm_map entry write). Nothing in this file may attach: a single
// PT_ATTACH permanently breaks the game's streaming I/O, which is the reason this route exists at
// all.

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "gtavmenu/cave_bootstrap.h"

#include <string.h>
#include <unistd.h>

#include "gtavmenu/cave_stub.h"
#include "gtavmenu/log.h"
#include "gtavmenu/proc_backend.h"

// The rip-relative operand inside PLAYER_PED_ID's prologue (`mov rax,[rip+disp32]` at offset 6).
// Wrong offsets corrupt the ModRM byte, so this is stated, never guessed -- tests/test_cave_stub.py
// freezes it.
#define CAVE_RIPREL_OFF 6u
#define CAVE_RIPREL_LEN 7u

#define CAVE_POLL_USEC 50000u

// Page span the protect calls operate on. The stub's own stores are the only reason any protection
// has to change at all -- mdbg/DMAP writes ignore it -- so the flip covers the cave's pages and
// nothing else. It used to be applied to the hooked native's page instead, relying on the
// executable being a single 51.6 MB vm_map entry, so that "somewhere in .text" meant "all of
// .text". Hardware showed a 16 KB protect is sufficient: the stub ran, allocated and published with
// only its own pages flipped. So the narrow scope is both enough and preferable -- the page holding
// the hooked native is never touched. The RWX (aux) rows a maps diff shows after a run are an
// artifact of the kernel write path, not of this protect; see docs/ptrace-free-injection.md.
#define CAVE_PAGE_MASK 0x3fffu

static uintptr_t cave_page_base(uint64_t cave) {
  return (uintptr_t)(cave & ~(uint64_t)CAVE_PAGE_MASK);
}

static size_t cave_page_span(uint64_t cave, uint32_t stub_len) {
  const uint64_t first = cave & ~(uint64_t)CAVE_PAGE_MASK;
  const uint64_t last = (cave + stub_len + CAVE_PAGE_MASK) & ~(uint64_t)CAVE_PAGE_MASK;
  return (size_t)(last - first);
}

// Keep this identical to the target-side guard in cave_stub.h. Raw FreeBSD syscall failures may
// be small positive errno values (carry set by the gadget) rather than Linux-style negative
// values, so positivity alone is never enough to classify an mmap result as an address.
static int allocation_plausible(uint64_t allocation) {
  return allocation > 0xffffull && allocation <= (uint64_t)INT64_MAX &&
         (allocation & 0xfffull) == 0;
}

// Protection changes fail transiently (status=0xf0000001 observed once on hardware, succeeding on
// the next attempt), and leaving .text writable or the prologue detoured is far worse than a slow
// retry, so every protect on this path is retried and verified.
int gtav_cave_protect_retry(int pid, uintptr_t addr, size_t n, int prot, const char* what) {
  int attempt;
  for (attempt = 0; attempt < 6; ++attempt) {
    if (attempt) {
      usleep(100000);
    }
    if (gtav_proc_protect(pid, addr, n, prot) == 0) {
      return 0;
    }
    gtav_logf("cave: protect %s attempt %d failed: %s", what, attempt + 1, gtav_proc_last_error());
  }
  return -1;
}

// mdbg reports success when writing memory that has no backing, so a write is only believed after a
// readback (docs/ptrace-free-injection.md).
int gtav_cave_write_verified(int pid, uintptr_t addr, const uint8_t* buf, uint32_t n,
                             const char* what) {
  uint8_t check[GTAV_CAVE_STUB_MAX];
  if (n > sizeof(check)) {
    gtav_logf("cave: %s too large (%u)", what, n);
    return -1;
  }
  if (gtav_proc_write_nostop(pid, addr, buf, n) != 0) {
    gtav_logf("cave: %s write failed: %s", what, gtav_proc_last_error());
    return -1;
  }
  memset(check, 0, sizeof(check));
  if (gtav_proc_read_nostop(pid, addr, check, n) != 0) {
    gtav_logf("cave: %s readback failed: %s", what, gtav_proc_last_error());
    return -1;
  }
  if (memcmp(check, buf, n) != 0) {
    gtav_logf("cave: %s readback MISMATCH at 0x%llx (write did not stick)", what,
              (unsigned long long)addr);
    return -1;
  }
  return 0;
}

// Put back only what was actually changed, in the order that matters: the prologue first, because
// it is what the game executes. Restoring something that was never written would report a scary
// failure for a target that is in fact untouched, so each step is gated on having happened.
static int restore_prologue(int pid, const GtavBuildPin* pin, int armed) {
  if (!armed) return 0;
  if (gtav_cave_write_verified(pid, (uintptr_t)pin->broker_target, pin->broker_expected,
                               pin->broker_stolen_len, "prologue restore") != 0) {
    gtav_logf("cave: FAILED to restore the prologue -- the detour is still live");
    return -1;
  }
  return 0;
}

static int restore_all(int pid, const GtavBuildPin* pin, uint64_t cave, uint32_t stub_len,
                       int armed, int stub_written, int protected_rwx, int settle_after_entry) {
  const uintptr_t page = cave_page_base(cave);
  const size_t span = cave_page_span(cave, stub_len);
  uint8_t zero[GTAV_CAVE_STUB_MAX];
  int ok = 0;

  // Never destroy a possible detour destination while the entry still targets it. For the split
  // lane, wait one full poll interval after verified entry restoration: the winner has published
  // completion only after restoring all registers, leaving only the 22-byte replay/jump tail.
  if (restore_prologue(pid, pin, armed) != 0) return -1;
  if (armed && settle_after_entry) usleep(CAVE_POLL_USEC);
  memset(zero, 0, sizeof(zero));
  if (stub_written && stub_len != 0 &&
      gtav_cave_write_verified(pid, (uintptr_t)cave, zero, stub_len, "cave clear") != 0) {
    ok = -1;
  }
  if (!protected_rwx) {
    return ok;
  }
  if (gtav_cave_protect_retry(pid, page, span, GTAV_PROC_PROT_READ | GTAV_PROC_PROT_EXEC,
                              "cave->rx") != 0) {
    gtav_logf(
        "cave: FAILED to restore 0x%llx (%zu bytes) to R+X; that part of .text is still "
        "writable",
        (unsigned long long)page, span);
    ok = -1;
  }
  return ok;
}

static int cave_bootstrap_probe_internal(int pid, const GtavBuildPin* pin, uint64_t cave,
                                         uint32_t alloc_size, uint32_t split_readonly_size,
                                         uint32_t timeout_ms, GtavCaveBootstrapResult* out) {
  uint8_t stub[GTAV_CAVE_STUB_MAX];
  uint8_t jump[16];
  uint8_t live[GTAV_CAVE_BROKER_EXPECTED_MAX];
  GtavCaveStubLayout layout;
  GtavCaveRiprel riprel;
  uintptr_t gadget = 0;
  uint32_t stub_len;
  uint32_t jump_len;
  uint32_t waited = 0;
  int armed = 0;
  int stub_written = 0;
  int protected_rwx = 0;

  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if (split_readonly_size) out->split_result = INT64_MIN;
  out->cave = cave;
  gtav_logf("cave: allocation request size=0x%x hint=0x%llx flags=0x%x (non-fixed)", alloc_size,
            (unsigned long long)GTAV_CAVE_ALLOC_HINT, GTAV_CAVE_MAP_ANON_PRIVATE);
  if (pin == NULL || cave == 0 || alloc_size == 0) {
    gtav_logf("cave: invalid argument");
    return -1;
  }
  if (pin->broker_stolen_len > sizeof(live) || pin->broker_patch_len > sizeof(jump)) {
    gtav_logf("cave: pin contract does not fit (stolen=%u patch=%u)", pin->broker_stolen_len,
              pin->broker_patch_len);
    return -1;
  }

  // The gadget resolve is a kernel dynlib walk: no ptrace, safe against a live game.
  if (gtav_proc_resolve_syscall_gadget(pid, &gadget) != 0) {
    gtav_logf("cave: %s", gtav_proc_last_error());
    return -1;
  }
  out->gadget = (uint64_t)gadget;
  gtav_logf("cave: syscall gadget at 0x%llx", (unsigned long long)gadget);

  // Refuse a target whose prologue is not the pinned one -- a stale pin would detour the wrong
  // code.
  memset(live, 0, sizeof(live));
  if (gtav_proc_read_nostop(pid, (uintptr_t)pin->broker_target, live, pin->broker_stolen_len) !=
      0) {
    gtav_logf("cave: cannot read the prologue: %s", gtav_proc_last_error());
    return -1;
  }
  if (memcmp(live, pin->broker_expected, pin->broker_stolen_len) != 0) {
    gtav_logf("cave: live prologue does not match the pin; refusing to detour");
    return -1;
  }

  riprel.offset = CAVE_RIPREL_OFF;
  riprel.length = CAVE_RIPREL_LEN;
  stub_len = gtav_cave_stub_build_split_readonly(
      cave, (uint64_t)gadget, (uint64_t)pin->broker_target, live, pin->broker_stolen_len,
      (uint64_t)pin->broker_continuation, alloc_size, split_readonly_size, &riprel, stub,
      (uint32_t)sizeof(stub), &layout);
  if (stub_len == 0) {
    gtav_logf("cave: stub assembly refused");
    return -1;
  }
  jump_len = gtav_cave_jump_build(cave, jump, (uint32_t)sizeof(jump));
  if (jump_len != pin->broker_patch_len) {
    gtav_logf("cave: jump length %u != pinned patch length %u", jump_len, pin->broker_patch_len);
    return -1;
  }

  // The cave must be untouched padding. Anything else means the address is wrong or already in use.
  {
    uint8_t before[GTAV_CAVE_STUB_MAX];
    uint32_t i;
    memset(before, 0, sizeof(before));
    if (gtav_proc_read_nostop(pid, (uintptr_t)cave, before, stub_len) != 0) {
      gtav_logf("cave: cannot read the cave: %s", gtav_proc_last_error());
      return -1;
    }
    for (i = 0; i < stub_len; ++i) {
      if (before[i] != 0) {
        gtav_logf("cave: 0x%llx is not zero-filled (byte %u = 0x%02x); refusing",
                  (unsigned long long)cave, i, before[i]);
        return -1;
      }
    }
  }

  // The stub writes its own one-shot flag and result slot, both of which live in .text. mdbg/DMAP
  // writes ignore protection but the stub's own stores do not, so the cave has to be writable while
  // it runs -- and only the cave: nothing else here needs a protection change, and the page holding
  // the hooked native is the last thing to leave writable by accident.
  if (gtav_cave_protect_retry(pid, cave_page_base(cave), cave_page_span(cave, stub_len),
                              GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC,
                              "cave->rwx") != 0) {
    return -1;
  }
  protected_rwx = 1;

  if (gtav_cave_write_verified(pid, (uintptr_t)cave, stub, stub_len, "stub") != 0) {
    restore_all(pid, pin, cave, stub_len, armed, stub_written, protected_rwx, 0);
    return -1;
  }
  stub_written = 1;
  gtav_logf(
      "cave: stub installed at 0x%llx (%u bytes) result@0x%llx split@0x%llx "
      "entered@0x%llx complete@0x%llx",
      (unsigned long long)cave, stub_len, (unsigned long long)layout.result_addr,
      (unsigned long long)layout.protect_result_addr, (unsigned long long)layout.done_addr,
      (unsigned long long)layout.completion_addr);

  // A failed write/readback does not prove no patch bytes landed, so cleanup must restore after any
  // patch attempt rather than only after a verified complete write.
  armed = 1;
  if (gtav_cave_write_verified(pid, (uintptr_t)pin->broker_target, jump, jump_len, "detour") != 0) {
    if (split_readonly_size) {
      // A failed verified write may still have exposed a complete detour to an entrant. Restore the
      // entry, but leave its possible destination intact until this terminal process exits.
      restore_prologue(pid, pin, armed);
    } else {
      restore_all(pid, pin, cave, stub_len, armed, stub_written, protected_rwx, 0);
    }
    return -1;
  }
  gtav_logf("cave: detour armed on 0x%llx", (unsigned long long)pin->broker_target);

  // Poll for the stub to publish. The hook fires ~270x per frame, so this normally resolves within
  // a frame or two; a long wait means the hook is not being reached at all.
  while (waited < timeout_ms) {
    uint32_t done = 0;
    size_t done_size = split_readonly_size ? sizeof(done) : sizeof(uint8_t);
    uint64_t result = 0;
    if (gtav_proc_read_nostop(pid, (uintptr_t)layout.done_addr, &done, done_size) == 0 && done) {
      out->stub_ran = 1;
      if (gtav_proc_read_nostop(pid, (uintptr_t)layout.result_addr, &result, sizeof(result)) == 0) {
        out->allocation = result;
        if (layout.protect_result_addr) {
          uint8_t completed = 0;
          if (gtav_proc_read_nostop(pid, (uintptr_t)layout.completion_addr, &completed,
                                    sizeof(completed)) == 0 &&
              completed) {
            if (result != 0 && !allocation_plausible(result)) {
              // The mmap plausibility guard jumped directly to publication, so mprotect was never
              // entered. The completion byte proves register restoration still finished.
              out->split_completed = 1;
              out->waited_ms = waited;
              break;
            }
            if (allocation_plausible(result)) {
              int64_t split_result = INT64_MIN;
              if (gtav_proc_read_nostop(pid, (uintptr_t)layout.protect_result_addr, &split_result,
                                        sizeof(split_result)) == 0 &&
                  split_result != INT64_MIN) {
                out->split_result = split_result;
                out->split_completed = 1;
                out->waited_ms = waited;
                break;
              }
            }
          }
        } else if (result != 0) {
          out->waited_ms = waited;
          break;
        }
      }
    }
    usleep(CAVE_POLL_USEC);
    waited += CAVE_POLL_USEC / 1000u;
  }
  out->waited_ms = waited;

  if (!out->stub_ran) {
    gtav_logf("cave: stub never ran (one-shot flag still clear after %ums)", waited);
  } else if (split_readonly_size && !out->split_completed) {
    gtav_logf(
        "cave: split stub did not publish completion after %ums (allocation=0x%llx); preserving "
        "cave bytes/protection until process exit",
        waited, (unsigned long long)out->allocation);
  } else if (out->allocation == 0) {
    gtav_logf("cave: stub ran but published no allocation after %ums", waited);
  } else if (!allocation_plausible(out->allocation)) {
    const int64_t signed_result = (int64_t)out->allocation;
    if (out->allocation <= 0xffffull) {
      gtav_logf("cave: mmap FAILED, raw positive errno=%llu", (unsigned long long)out->allocation);
    } else if (signed_result < 0 && signed_result >= -0xffffll) {
      gtav_logf("cave: mmap FAILED, raw negative errno=%lld", -(long long)signed_result);
    } else {
      gtav_logf("cave: mmap returned implausible address/result 0x%llx",
                (unsigned long long)out->allocation);
    }
  } else {
    gtav_logf("cave: ALLOCATED 0x%llx (%u bytes) from the game thread after %ums",
              (unsigned long long)out->allocation, alloc_size, waited);
    if (split_readonly_size)
      gtav_logf("cave: target split first 0x%x bytes result=%lld", split_readonly_size,
                (long long)out->split_result);
  }

  if (split_readonly_size && !out->split_completed) {
    // An entrant may still be inside mprotect or may already have fetched the detour. Stop new
    // entrants by restoring the broker, but never clear/re-protect code an in-flight thread may
    // still execute. This process is terminal and must exit; the final cave image is not written.
    restore_prologue(pid, pin, armed);
    out->restored = 0;
  } else {
    out->restored = (restore_all(pid, pin, cave, stub_len, armed, stub_written, protected_rwx,
                                 split_readonly_size != 0) == 0);
  }
  gtav_logf("cave: restore %s", out->restored ? "ok" : "INCOMPLETE");

  return (out->stub_ran && out->restored && allocation_plausible(out->allocation) &&
          (!split_readonly_size || (out->split_completed && out->split_result == 0)))
             ? 0
             : -1;
}

int gtav_cave_bootstrap_probe(int pid, const GtavBuildPin* pin, uint64_t cave, uint32_t alloc_size,
                              uint32_t timeout_ms, GtavCaveBootstrapResult* out) {
  return cave_bootstrap_probe_internal(pid, pin, cave, alloc_size, 0, timeout_ms, out);
}

int gtav_cave_bootstrap_probe_split(int pid, const GtavBuildPin* pin, uint64_t cave,
                                    uint32_t alloc_size, uint32_t split_readonly_size,
                                    uint32_t timeout_ms, GtavCaveBootstrapResult* out) {
  return cave_bootstrap_probe_internal(pid, pin, cave, alloc_size, split_readonly_size, timeout_ms,
                                       out);
}
