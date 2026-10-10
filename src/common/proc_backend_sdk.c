// ps5-payload-sdk implementation of the process-control backend.
//
// Built only in the on-console payload build (it includes <ps5/kernel.h> and
// uses FreeBSD ptrace); host CI compiles the pure half in proc_backend.c. This
// is the etaHEN/libhijacker-free replacement: process discovery via sysctl,
// ucred elevation and symbol resolution via the SDK kernel_* helpers, and
// cross-process memory access via ptrace PT_IO.

#include "gtavmenu/proc_backend.h"

#include "gtavmenu/inject_lock.h"
#include "gtavmenu/log.h"

#include <errno.h>
#include <machine/reg.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/resourcevar.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

// Foreground big-app discovery (libSceSystemService + libSceSysCore). This is the
// method the proven etaHEN loader uses on this console; the earlier sysctl +
// sceKernelGetAppInfo enumeration did not match the running game.
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char* title_id);
int _sceApplicationGetAppId(int pid, int* app_id);

// Route plain reads/writes through the debug-memory path instead of ptrace. Every PT_ATTACH stops
// and resumes every thread in the target, and on 01.010.002 a handful of those cycles permanently
// jams GTA's streaming I/O (docs/ptrace-free-injection.md). With this on, the only remaining
// stops in an inject are the two that genuinely need one: alloc_exec's remote mmap and
// start_thread's injected call. 0 = classic ptrace-only behaviour.
#ifndef GTAV_PROC_NOSTOP_IO
#define GTAV_PROC_NOSTOP_IO 0
#endif

// Never stop the target, at all. On the ptrace-free lane a failed kernel-path transfer must fail
// rather than quietly reach for ptrace: the fallback performs the exact damage the lane exists to
// avoid, and inject_write's six retries then make it look like a success. Enforced at pt_attach,
// the one point every stop passes through, so a path nobody thought to audit cannot stop the game
// either. Requires GTAV_PROC_NOSTOP_IO -- there would otherwise be no kernel path to be strict
// about. 0 = classic behaviour: warn once and fall back.
#ifndef GTAV_PROC_NOSTOP_STRICT
#define GTAV_PROC_NOSTOP_STRICT 0
#endif

#if GTAV_PROC_NOSTOP_STRICT && !GTAV_PROC_NOSTOP_IO
#error "GTAV_PROC_NOSTOP_STRICT needs GTAV_PROC_NOSTOP_IO=1 (no kernel path to be strict about)"
#endif

static char g_last_error[192] = "none";

static int fail(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
  va_end(args);
  g_last_error[sizeof(g_last_error) - 1] = '\0';
  return -1;
}

const char* gtav_proc_last_error(void) {
  return g_last_error;
}

int gtav_proc_backend_init(void) {
  // The SDK CRT (__kernel_init) binds kernel R/W before main(); a zero firmware
  // version means that binding did not happen (payload not launched with a valid
  // kernel R/W context).
  if (kernel_get_fw_version() == 0) {
    return fail("kernel R/W unavailable (payload_args missing)");
  }
  return 0;
}

int gtav_proc_foreground(char* title_id, size_t title_capacity, int* pid_out, uint64_t* token_out) {
  char tid[128], check[128];
  int appid;
  int pid;
  if (!title_id || title_capacity < 10u || !pid_out || !token_out)
    return fail("foreground: invalid argument");
  title_id[0] = 0;
  *pid_out = -1;
  *token_out = 0;
  appid = sceSystemServiceGetAppIdOfRunningBigApp();
  if (appid <= 0) return fail("foreground: no running big app (appid=%d)", appid);
  memset(tid, 0, sizeof(tid));
  if (sceSystemServiceGetAppTitleId(appid, tid) != 0)
    return fail("foreground: title query failed appid=0x%x", appid);
  const size_t title_length = strnlen(tid, sizeof(tid));
  if (!title_length || title_length >= sizeof(tid) || title_length >= title_capacity)
    return fail("foreground: invalid title appid=0x%x", appid);
  for (pid = 1; pid <= 9999; ++pid) {
    int current = 0;
    if (_sceApplicationGetAppId(pid, &current) < 0 || current != appid) continue;
    memset(check, 0, sizeof(check));
    if (sceSystemServiceGetAppIdOfRunningBigApp() != appid ||
        sceSystemServiceGetAppTitleId(appid, check) != 0 || memcmp(tid, check, sizeof(tid)) != 0 ||
        _sceApplicationGetAppId(pid, &current) < 0 || current != appid)
      return fail("foreground: instance changed during discovery");
    memcpy(title_id, tid, title_length + 1u);
    *pid_out = pid;
    *token_out = (uint64_t)appid;
    return 0;
  }
  return fail("foreground: no pid for appid 0x%x (%s)", appid, tid);
}

int gtav_proc_find_game(const char* title_id, int* pid_out) {
  char foreground[128];
  uint64_t token;
  if (!title_id || !pid_out) return fail("find_game: null argument");
  if (gtav_proc_foreground(foreground, sizeof(foreground), pid_out, &token) != 0) return -1;
  if (strcmp(foreground, title_id) != 0) {
    *pid_out = -1;
    return fail("find_game: foreground app is %s, not %s", foreground, title_id);
  }
  return 0;
}

uint64_t gtav_proc_app_id(int pid) {
  int appid = 0;
  if (pid <= 0) {
    return 0;
  }
  // _sceApplicationGetAppId maps a pid to its (per-launch unique) app id. No ptrace involved, so
  // it is safe against a dying/recycled target. <0 / non-positive => unknown.
  if (_sceApplicationGetAppId(pid, &appid) < 0 || appid <= 0) {
    return 0;
  }
  return (uint64_t)appid;
}

// p_start is immutable for the process lifetime. Do not use kernel_get_proc() here: the SDK
// intentionally caches pid->proc results and therefore cannot distinguish a fast pid reuse, which
// is exactly the transition this token must detect. Freeze the target SDK ABI used by the host-side
// ps5debug reader as well; an SDK layout change must fail the build instead of silently changing
// the cross-route token.
_Static_assert(offsetof(struct proc, p_stats) == 0x58u, "unexpected target struct proc.p_stats");
_Static_assert(offsetof(struct pstats, p_start) == 0x110u, "unexpected target pstats.p_start");
#define GTAV_KERNEL_PROC_P_STATS_OFFSET ((uintptr_t)offsetof(struct proc, p_stats))
#define GTAV_KERNEL_PSTATS_P_START_OFFSET ((uintptr_t)offsetof(struct pstats, p_start))

static intptr_t gtav_proc_find_uncached(int pid) {
  intptr_t proc = 0;
  unsigned int guard;
  if (pid <= 0 || kernel_copyout(KERNEL_ADDRESS_ALLPROC, &proc, sizeof(proc)) != 0) {
    return 0;
  }
  for (guard = 0; proc != 0 && guard < 4096u; ++guard) {
    int current_pid = 0;
    intptr_t next = 0;
    if (kernel_copyout(proc + KERNEL_OFFSET_PROC_P_PID, &current_pid, sizeof(current_pid)) != 0) {
      return 0;
    }
    if (current_pid == pid) {
      return proc;
    }
    if (kernel_copyout(proc, &next, sizeof(next)) != 0 || next == proc) {
      return 0;
    }
    proc = next;
  }
  return 0;
}

uint64_t gtav_proc_start_token(int pid) {
  intptr_t proc = gtav_proc_find_uncached(pid);
  intptr_t pstats = 0;
  int64_t started[2] = {0, 0};
  if (proc == 0 ||
      kernel_copyout(proc + GTAV_KERNEL_PROC_P_STATS_OFFSET, &pstats, sizeof(pstats)) != 0 ||
      pstats == 0 ||
      kernel_copyout(pstats + GTAV_KERNEL_PSTATS_P_START_OFFSET, started, sizeof(started)) != 0) {
    return 0;
  }
  return gtav_inject_process_start_token(started[0], started[1]);
}

int gtav_proc_elevate(int pid) {
  GtavProcElevation plan;
  if (gtav_proc_elevation_plan(&plan) != 0) {
    return fail("elevate: no elevation plan");
  }
  if (kernel_set_ucred_authid(pid, plan.authid) != 0) {
    return fail("elevate: set authid failed pid=%d", pid);
  }
  if (kernel_set_ucred_caps(pid, plan.caps) != 0) {
    return fail("elevate: set caps failed pid=%d", pid);
  }
  if (kernel_set_ucred_attrs(pid, plan.attrs) != 0) {
    return fail("elevate: set attrs failed pid=%d", pid);
  }
  if (plan.relocate_root) {
    intptr_t root = kernel_get_root_vnode();
    if (root != 0) {
      kernel_set_proc_rootdir(pid, root);
      kernel_set_proc_jaildir(pid, root);
    }
  }
  return 0;
}

// Best-effort: log every loaded module's handle + path for the target, so a failed
// resolve shows the real on-console basenames to match against. Mirrors the SDK's
// dynlib walk (list head at kproc+0x3e8; per node: next@0, path@8, handle@40). Logs
// at most once per process. Read-only (kernel copyout); never fatal.
static void log_module_list_once(int pid) {
  static int logged = 0;
  unsigned long kproc;
  unsigned long node;
  unsigned long kpath;
  unsigned long handle;
  char path[256];
  int i;

  if (logged) {
    return;
  }
  logged = 1;

  kproc = (unsigned long)kernel_get_proc(pid);
  if (kproc == 0) {
    gtav_logf("modlist: kernel_get_proc failed");
    return;
  }
  if (kernel_copyout((intptr_t)(kproc + 0x3e8), &node, sizeof(node)) < 0 || node == 0) {
    gtav_logf("modlist: dynlib list head unreadable");
    return;
  }
  gtav_logf("modlist: loaded modules for pid=%d:", pid);
  for (i = 0; i < 256; ++i) {
    if (kernel_copyout((intptr_t)node, &node, sizeof(node)) < 0 || node == 0) {
      break;
    }
    if (kernel_copyout((intptr_t)(node + 8), &kpath, sizeof(kpath)) < 0) {
      break;
    }
    memset(path, 0, sizeof(path));
    if (kpath == 0 || kernel_copyout((intptr_t)kpath, path, sizeof(path) - 1) < 0) {
      path[0] = '?';
    }
    if (kernel_copyout((intptr_t)(node + 40), &handle, sizeof(handle)) < 0) {
      break;
    }
    gtav_logf("modlist:   handle=0x%lx %s", handle, path);
  }
}

int gtav_proc_resolve_sym(int pid, GtavProcSym sym, uintptr_t* addr_out) {
  const char* lib;
  const char* name;
  const char* candidates[3];
  int ncand = 0;
  int i;

  if (addr_out == NULL) {
    return fail("resolve_sym: null out");
  }
  if (gtav_proc_symbol_name(sym, &lib, &name) != 0) {
    return fail("resolve_sym: unknown symbol id %d", (int)sym);
  }

  // Primary basename from the table, plus the libkernel split-module variants:
  // a symbol may live in (or only be exported from) libkernel_web / libkernel_sys.
  candidates[ncand++] = lib;
  if (strcmp(lib, "libkernel.sprx") == 0) {
    candidates[ncand++] = "libkernel_web.sprx";
    candidates[ncand++] = "libkernel_sys.sprx";
  }

  for (i = 0; i < ncand; ++i) {
    uint32_t handle = 0;
    intptr_t addr;
    if (kernel_dynlib_handle(pid, candidates[i], &handle) != 0) {
      continue;
    }
    addr = kernel_dynlib_dlsym(pid, handle, name);
    if (addr != 0) {
      *addr_out = (uintptr_t)addr;
      return 0;
    }
  }

  log_module_list_once(pid);
  return fail("resolve_sym: %s not found (tried %s + libkernel variants)", name, lib);
}

int gtav_proc_resolve_named(int pid, const char* const* libs, int nlibs, const char* name,
                            uintptr_t* addr_out) {
  // libkernel split-module fallbacks, appended after the caller's candidate list so
  // a libkernel symbol resolves even if DT_NEEDED only names one variant.
  static const char* const kFallback[] = {"libkernel.sprx", "libkernel_web.sprx",
                                          "libkernel_sys.sprx"};
  int i;

  if (libs == NULL || name == NULL || addr_out == NULL) {
    return fail("resolve_named: null argument");
  }

  for (i = 0; i < nlibs + (int)(sizeof(kFallback) / sizeof(kFallback[0])); ++i) {
    const char* lib = (i < nlibs) ? libs[i] : kFallback[i - nlibs];
    uint32_t handle = 0;
    intptr_t addr;
    if (lib == NULL || kernel_dynlib_handle(pid, lib, &handle) != 0) {
      continue;
    }
    addr = kernel_dynlib_dlsym(pid, handle, name);
    if (addr != 0) {
      *addr_out = (uintptr_t)addr;
      return 0;
    }
  }
  return fail("resolve_named: %s not found in any candidate library", name);
}

// PS5 ptrace requires the CALLER to carry debug authority, not just the target.
// Mirror the proven NineS approach: borrow the debugger authid for the duration of
// each ptrace syscall, then restore our own. Without this PT_ATTACH/PT_GETREGS/...
// fail with EPERM even though the target ucred is already elevated.
#define BT_PTRACE_AUTHID 0x4800000000010003ULL

// Upper bound on single-steps for one injected call (see pt_syscall). The syscall
// gadget is only ~2 instructions, so this is just a runaway backstop; it also keeps
// any mis-step from freezing the game for long. Calls that can block must use
// pt_call_bp (continue-to-breakpoint) instead of single-stepping.
#define PT_CALL_MAX_STEPS 200000ULL

static int bt_ptrace(int req, pid_t pid, caddr_t addr, int data) {
  pid_t self = getpid();
  uint64_t saved = kernel_get_ucred_authid(self);
  int ret;

  if (saved == 0) {
    return -1;
  }
  if (kernel_set_ucred_authid(self, BT_PTRACE_AUTHID) != 0) {
    return -1;
  }
  ret = (int)ptrace(req, pid, addr, data);
  kernel_set_ucred_authid(self, saved);
  return ret;
}

// Streaming-quiescence gate for PT_ATTACH. Stopping the target destroys the file reads that are
// IN FLIGHT at that instant: their completions never arrive, the streamer's ring consumer parks on
// the first incomplete slot, and every read queued behind it is stranded for the life of the
// process (docs/ptrace-free-injection.md). Reads already completed and reads not yet submitted
// are unaffected -- so attaching while nothing is in flight should be harmless. The counter is
// build-specific, so it arrives as an address define; 0 disables the gate entirely.
#ifndef GTAV_LOADER_QUIESCE_ADDR
#define GTAV_LOADER_QUIESCE_ADDR 0
#endif
#ifndef GTAV_LOADER_QUIESCE_TIMEOUT_MS
#define GTAV_LOADER_QUIESCE_TIMEOUT_MS 15000
#endif
// Consecutive zero reads required. One zero can be the gap between two submissions; a short run of
// them means the streamer genuinely has nothing outstanding.
#define GTAV_LOADER_QUIESCE_RUN 4
#define GTAV_LOADER_QUIESCE_POLL_USEC 20000

// Wait (via mdbg, which never stops the target) until no streaming read is in flight. Returns 0
// when quiescent, -1 on timeout -- the caller proceeds either way, because failing the inject is
// not obviously better than a jammed streamer, but the log records which one happened.
static int await_stream_quiescent(pid_t pid) {
  unsigned long waited = 0;
  int run = 0;
  uint32_t inflight = 0;

  if ((uintptr_t)(GTAV_LOADER_QUIESCE_ADDR) == 0) {
    return 0;
  }
  while (waited < (unsigned long)(GTAV_LOADER_QUIESCE_TIMEOUT_MS) * 1000UL) {
    if (mdbg_copyout(pid, (intptr_t)(GTAV_LOADER_QUIESCE_ADDR), &inflight, sizeof(inflight)) != 0) {
      gtav_logf("quiesce: cannot read the in-flight counter; attaching without the gate");
      return -1;
    }
    run = (inflight == 0) ? run + 1 : 0;
    if (run >= GTAV_LOADER_QUIESCE_RUN) {
      if (waited != 0) {
        gtav_logf("quiesce: streaming idle after %lums; attaching", waited / 1000UL);
      }
      return 0;
    }
    usleep(GTAV_LOADER_QUIESCE_POLL_USEC);
    waited += GTAV_LOADER_QUIESCE_POLL_USEC;
  }
  gtav_logf("quiesce: still %u reads in flight after %dms; attaching anyway (streamer may jam)",
            inflight, (int)(GTAV_LOADER_QUIESCE_TIMEOUT_MS));
  return -1;
}

// Read the in-flight counter with the target already stopped, which makes the reading exact: the
// game cannot submit or retire anything while every thread is held. This is the discriminator
// between the two explanations for a post-attach jam. Zero here means nothing was outstanding when
// we stopped it, so the damage is to the completion machinery itself rather than to a destroyed
// in-flight read -- and no amount of waiting before the attach will avoid it. Non-zero means the
// pre-attach gate lost a race it could be made to win.
static void log_inflight_while_stopped(pid_t pid, int quiesced) {
  static int logged = 0;
  uint32_t inflight = 0;

  if ((uintptr_t)(GTAV_LOADER_QUIESCE_ADDR) == 0 || logged >= 4) {
    return;
  }
  logged++;
  if (mdbg_copyout(pid, (intptr_t)(GTAV_LOADER_QUIESCE_ADDR), &inflight, sizeof(inflight)) != 0) {
    gtav_logf("quiesce: post-stop in-flight unreadable");
    return;
  }
  gtav_logf("quiesce: post-stop in-flight=%u (pre-attach gate %s)", inflight,
            quiesced == 0 ? "reported idle" : "did NOT reach idle");
}

// PT_ATTACH stops the whole target, so transfers and register injection run while
// no target thread executes -- a multi-byte write (e.g. an inline detour) cannot be
// torn by a concurrent game-thread read, and an injected call runs single-threaded.
static int pt_attach(pid_t pid) {
  int status = 0;
  int quiesced;
#if GTAV_PROC_NOSTOP_STRICT
  // Refuse before anything observable happens. Every stop in this file funnels through here, so
  // this one return is what makes "no ptrace" a property of the build rather than a review promise.
  return fail("attach: refused, this build may not stop the target (NOSTOP_STRICT) pid=%d", pid);
#endif
  quiesced = await_stream_quiescent(pid);
  if (bt_ptrace(PT_ATTACH, pid, NULL, 0) != 0) {
    return fail("attach: PT_ATTACH failed pid=%d errno=%d", pid, errno);
  }
  if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
    bt_ptrace(PT_DETACH, pid, NULL, 0);
    return fail("attach: target did not stop pid=%d errno=%d", pid, errno);
  }
  log_inflight_while_stopped(pid, quiesced);
  return 0;
}

static int pt_detach(pid_t pid) {
  if (bt_ptrace(PT_DETACH, pid, NULL, 0) != 0) {
    return fail("detach: PT_DETACH failed pid=%d errno=%d", pid, errno);
  }
  return 0;
}

// One PT_IO transfer on an already-attached, stopped target.
static int pt_io(pid_t pid, int op, uintptr_t addr, void* buf, size_t n, const char* what) {
  struct ptrace_io_desc desc;
  desc.piod_op = op;
  desc.piod_offs = (void*)addr;
  desc.piod_addr = buf;
  desc.piod_len = n;
  if (bt_ptrace(PT_IO, pid, (caddr_t)&desc, 0) != 0 || desc.piod_len != n) {
    return fail("%s: PT_IO failed pid=%d addr=0x%lx errno=%d", what, pid, (unsigned long)addr,
                errno);
  }
  return 0;
}

static int pt_step(pid_t pid) {
  int status = 0;
  if (bt_ptrace(PT_STEP, pid, (caddr_t)1, 0) != 0) {
    return -1;
  }
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return 0;
}

// Resolve the libkernel `syscall` instruction gadget (NID "HoLVWNanBBc" + 0xa),
// trying the usual module handles. Lets us invoke a raw syscall in the target
// instead of single-stepping a whole libc wrapper.
static intptr_t resolve_syscall_gadget(int pid) {
  static const char* nid = "HoLVWNanBBc";
  intptr_t addr = kernel_dynlib_resolve(pid, 0x1, nid);
  if (addr == 0) {
    addr = kernel_dynlib_resolve(pid, 0x2001, nid);
  }
  return addr ? addr + 0xa : 0;
}

// Public wrapper: injected code needs this address baked into it, and the resolve is a kernel
// dynlib walk, so it costs the game nothing.
int gtav_proc_resolve_syscall_gadget(int pid, uintptr_t* addr_out) {
  intptr_t gadget;
  if (addr_out == NULL) {
    return fail("resolve_syscall_gadget: invalid argument");
  }
  *addr_out = 0;
  gadget = resolve_syscall_gadget(pid);
  if (gadget == 0) {
    return fail("resolve_syscall_gadget: not found in pid=%d", pid);
  }
  *addr_out = (uintptr_t)gadget;
  return 0;
}

// Invoke a raw syscall in the stopped target via the syscall gadget (NineS
// pt_syscall). Only the `syscall; ret` gadget is single-stepped (~2 steps), so the
// return value in RAX is reliable -- unlike single-stepping a whole libc wrapper,
// whose RSP-return heuristic can misfire across the syscall and yield a bogus value.
// Note the syscall ABI: the 4th argument is passed in r10, not rcx. The target must
// already be PT_ATTACH-stopped. Returns 0 only when the syscall return and register restoration are
// both verified. On failure, *may_have_run_out distinguishes a pre-dispatch failure from a step
// whose side effects are unknown; *ret_observed_out says whether *ret_out captured RAX before a
// later restore failed. This distinction is critical for mmap: an allocation must not disappear
// behind the same -1 value used for transport/control failures.
static int pt_syscall(pid_t pid, int sysno, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, const char* what, long* ret_out,
                      int* may_have_run_out, int* ret_observed_out) {
  struct reg saved;
  struct reg regs;
  intptr_t gadget = resolve_syscall_gadget(pid);

  if (ret_out == NULL || may_have_run_out == NULL || ret_observed_out == NULL) {
    return fail("%s: invalid syscall result arguments", what);
  }
  *ret_out = -1;
  *may_have_run_out = 0;
  *ret_observed_out = 0;

  if (gadget == 0) {
    return fail("%s: syscall gadget not found", what);
  }
  if (bt_ptrace(PT_GETREGS, pid, (caddr_t)&saved, 0) != 0) {
    return fail("%s: PT_GETREGS failed errno=%d", what, errno);
  }
  memcpy(&regs, &saved, sizeof(regs));
  regs.r_rip = (long)gadget;
  regs.r_rax = sysno;
  regs.r_rdi = (long)a0;
  regs.r_rsi = (long)a1;
  regs.r_rdx = (long)a2;
  regs.r_r10 = (long)a3;
  regs.r_r8 = (long)a4;
  regs.r_r9 = (long)a5;
  if (bt_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) != 0) {
    return fail("%s: PT_SETREGS failed errno=%d", what, errno);
  }

  for (uint64_t steps = 0; (uint64_t)regs.r_rsp <= (uint64_t)saved.r_rsp; ++steps) {
    if (steps >= PT_CALL_MAX_STEPS) {
      bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
      return fail("%s: single-step cap hit", what);
    }
    *may_have_run_out = 1;  // PT_STEP may execute the syscall even if wait/control fails afterward
    if (pt_step(pid) != 0) {
      bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
      return fail("%s: single-step failed errno=%d", what, errno);
    }
    if (bt_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) {
      bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
      return fail("%s: PT_GETREGS(step) failed errno=%d", what, errno);
    }
  }

  *ret_out = regs.r_rax;
  *ret_observed_out = 1;
  if (bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0) != 0) {
    return fail("%s: restore PT_SETREGS failed errno=%d", what, errno);
  }
  return 0;
}

// Call a (<=6 arg) function inside the stopped target, returning via a breakpoint
// instead of single-stepping. We push trap_addr (an int3 byte in an executable page
// the caller controls) as the return address, then PT_CONTINUE the whole process:
// the callee runs at full speed with all threads live (so it can take locks / spawn
// threads), and when it returns it jumps to trap_addr and faults SIGTRAP back to us.
// This is the variant for callees that block or depend on other threads (e.g.
// scePthreadCreate), which single-step deadlocks because the other threads are frozen.
// The target must already be PT_ATTACH-stopped.
static long pt_call_bp(pid_t pid, uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uintptr_t trap_addr, const char* what) {
  struct reg saved;
  struct reg regs;
  uint64_t sp;
  int status = 0;

  if (bt_ptrace(PT_GETREGS, pid, (caddr_t)&saved, 0) != 0) {
    return fail("%s: PT_GETREGS failed errno=%d", what, errno);
  }
  memcpy(&regs, &saved, sizeof(regs));
  regs.r_rip = (long)fn;
  regs.r_rdi = (long)a0;
  regs.r_rsi = (long)a1;
  regs.r_rdx = (long)a2;
  regs.r_rcx = (long)a3;
  regs.r_r8 = (long)a4;
  regs.r_r9 = (long)a5;

  // 16-byte align the stack then make room for the return address, so the callee
  // sees ABI-correct alignment (rsp % 16 == 8 at entry, after the pushed retaddr).
  sp = ((uint64_t)saved.r_rsp & ~0xFULL) - 8;
  if (pt_io(pid, PIOD_WRITE_D, (uintptr_t)sp, &trap_addr, sizeof(trap_addr), what) != 0) {
    return -1;
  }
  regs.r_rsp = (long)sp;

  if (bt_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) != 0) {
    return fail("%s: PT_SETREGS failed errno=%d", what, errno);
  }
  if (bt_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0) != 0) {
    bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return fail("%s: PT_CONTINUE failed errno=%d", what, errno);
  }
  if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
    bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return fail("%s: target did not stop at trap errno=%d", what, errno);
  }
  if (bt_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) {
    bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return fail("%s: PT_GETREGS(trap) failed errno=%d", what, errno);
  }

  long ret = regs.r_rax;
  if (bt_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0) != 0) {
    return fail("%s: restore PT_SETREGS failed errno=%d", what, errno);
  }
  return ret;
}

typedef struct RemoteMmapResult {
  intptr_t address;
  int address_observed;
  int allocation_may_exist;
} RemoteMmapResult;

// Remote anonymous RW page on an already-attached target. Returns 0 on a verified address, 1 only
// when mmap itself definitively returned MAP_FAILED (safe for a bounded retry), and -1 for a
// ptrace/control failure. `out` preserves a known address or an unknown-side-effect marker even
// when register restoration/stepping fails after syscall dispatch.
static int remote_mmap_rw(pid_t pid, size_t len, RemoteMmapResult* out) {
  long ret = -1;
  int may_have_run = 0;
  int ret_observed = 0;
  int syscall_rc;
  if (out == NULL) {
    return fail("remote_mmap: null result");
  }
  memset(out, 0, sizeof(*out));
  // Raw SYS_mmap via the syscall gadget (reliable return value), rather than
  // single-stepping the libc mmap wrapper. MAP_FAILED is (void*)-1.
  syscall_rc = pt_syscall(pid, SYS_mmap, 0, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                          (uint64_t)-1, 0, "remote_mmap", &ret, &may_have_run, &ret_observed);
  if (ret_observed && ret != -1) {
    out->address = ret;
    out->address_observed = 1;
    out->allocation_may_exist = 1;
  } else if (may_have_run && !ret_observed) {
    out->allocation_may_exist = 1;  // syscall dispatched, but RAX could not be recovered
  }
  if (syscall_rc != 0) {
    return -1;  // g_last_error names the control failure; out retains any mmap side-effect evidence
  }
  if (ret == -1) {
    fail("remote_mmap: mmap returned MAP_FAILED");
    return 1;  // explicit syscall failure, no allocation; caller may retry
  }
  if (ret == 0) {
    return fail("remote_mmap: mmap returned null; treating allocation as ambiguous");
  }
  return 0;
}

// Public read/write: attach, transfer, detach. Attaching and detaching per call
// means the target runs again between a read and a later write -- fine for stable
// .text (a prologue is not self-modifying); a path that must validate-then-write
// atomically should hold one attach across both.
static int proc_io(int pid, int op, uintptr_t addr, void* buf, size_t n, const char* what) {
  int rc;
  if (pid <= 0 || buf == NULL) {
    return fail("%s: invalid argument pid=%d", what, pid);
  }
  if (n == 0) {
    return 0;
  }
  if (pt_attach(pid) != 0) {
    return -1;
  }
  rc = pt_io(pid, op, addr, buf, n, what);
  if (pt_detach(pid) != 0) {
    return -1;
  }
  return rc;
}

// Routing for plain reads/writes (GTAV_PROC_NOSTOP_IO) and the refusal to fall back
// (GTAV_PROC_NOSTOP_STRICT) are both declared at the top of this file, because the strict gate has
// to be visible to pt_attach.

#if GTAV_PROC_NOSTOP_IO && !GTAV_PROC_NOSTOP_STRICT
// Log the first fallback only. A fallback means we are about to stop the game -- the exact thing
// this path exists to avoid -- and 800+ identical lines during a relocation pass would bury it.
static void nostop_fallback_warn(const char* what, uintptr_t addr) {
  static int warned = 0;
  if (warned) {
    return;
  }
  warned = 1;
  gtav_logf(
      "nostop: mdbg unavailable for %s @0x%lx (%s); falling back to ptrace -- THIS STOPS THE GAME",
      what, (unsigned long)addr, gtav_proc_last_error());
}
#endif

#if GTAV_PROC_NOSTOP_STRICT
// Refuse the transfer rather than stopping the target. The last error is left exactly as the kernel
// path set it, so the caller reports why the transfer failed (e.g. a non-resident page) rather than
// a generic refusal; only the first refusal is logged, for the same reason as above.
static int nostop_strict_refuse(const char* what, uintptr_t addr) {
  static int refused = 0;
  if (!refused) {
    refused = 1;
    gtav_logf("nostop: STRICT refuses ptrace for %s @0x%lx (%s); failing the transfer instead",
              what, (unsigned long)addr, gtav_proc_last_error());
  }
  return -1;
}
#endif

int gtav_proc_read(int pid, uintptr_t addr, void* buf, size_t n) {
#if GTAV_PROC_NOSTOP_IO
  if (pid > 0 && buf != NULL && n != 0) {
    if (gtav_proc_read_nostop(pid, addr, buf, n) == 0) {
      return 0;
    }
#if GTAV_PROC_NOSTOP_STRICT
    return nostop_strict_refuse("read", addr);
#else
    nostop_fallback_warn("read", addr);
#endif
  }
#endif
  return proc_io(pid, PIOD_READ_D, addr, buf, n, "read");
}

// Copy through the kernel rather than ptrace: the target never stops, and page protections are
// ignored, so execute-only .text is readable and writable without a protect call.
//
// kernel_proc_copyin/copyout try the debug-memory service first and fall back to walking the
// target's page tables and copying through the kernel direct map. That fallback is not optional on
// firmware >= 8.40, where the debug-memory WRITE service returns EPERM outright -- the reason the
// bootstrap probe failed with `mdbg_copyin failed ... errno=1` (docs/ptrace-free-injection.md).
// Older SDKs expose only the mdbg half; GTAV_SDK_HAS_PROC_COPYIO says which is available, so the
// loader still builds against either. Note the DMAP fallback reaches RESIDENT pages only.
#ifndef GTAV_SDK_HAS_PROC_COPYIO
#define GTAV_SDK_HAS_PROC_COPYIO 0
#endif

int gtav_proc_read_nostop(int pid, uintptr_t addr, void* buf, size_t n) {
  if (pid <= 0 || buf == NULL) {
    return fail("read_nostop: invalid argument pid=%d", pid);
  }
  if (n == 0) {
    return 0;
  }
#if GTAV_SDK_HAS_PROC_COPYIO
  if (kernel_proc_copyout(pid, (intptr_t)addr, buf, n) != 0) {
    return fail("read_nostop: kernel_proc_copyout failed pid=%d addr=0x%lx errno=%d", pid,
                (unsigned long)addr, errno);
  }
#else
  if (mdbg_copyout(pid, (intptr_t)addr, buf, n) != 0) {
    return fail("read_nostop: mdbg_copyout failed pid=%d addr=0x%lx errno=%d", pid,
                (unsigned long)addr, errno);
  }
#endif
  return 0;
}

int gtav_proc_write_nostop(int pid, uintptr_t addr, const void* buf, size_t n) {
  if (pid <= 0 || buf == NULL) {
    return fail("write_nostop: invalid argument pid=%d", pid);
  }
  if (n == 0) {
    return 0;
  }
#if GTAV_SDK_HAS_PROC_COPYIO
  if (kernel_proc_copyin(pid, buf, (intptr_t)addr, n) != 0) {
    return fail("write_nostop: kernel_proc_copyin failed pid=%d addr=0x%lx errno=%d", pid,
                (unsigned long)addr, errno);
  }
#else
  if (mdbg_copyin(pid, buf, (intptr_t)addr, n) != 0) {
    return fail(
        "write_nostop: mdbg_copyin failed pid=%d addr=0x%lx errno=%d (this SDK has no "
        "page-table fallback; firmware >= 8.40 refuses debug-memory writes)",
        pid, (unsigned long)addr, errno);
  }
#endif
  return 0;
}

int gtav_proc_write(int pid, uintptr_t addr, const void* buf, size_t n) {
#if GTAV_PROC_NOSTOP_IO
  if (pid > 0 && buf != NULL && n != 0) {
    if (gtav_proc_write_nostop(pid, addr, buf, n) == 0) {
      return 0;
    }
#if GTAV_PROC_NOSTOP_STRICT
    return nostop_strict_refuse("write", addr);
#else
    nostop_fallback_warn("write", addr);
#endif
  }
#endif
  return proc_io(pid, PIOD_WRITE_D, addr, (void*)(uintptr_t)buf, n, "write");
}

// One-level pointer-chain read under a SINGLE attach: both PT_IO reads run while the target is
// stopped once, so a DEREF SP-ready poll costs one whole-game freeze instead of two. Semantics
// (reachable vs null-base vs value-read) are documented in proc_backend.h.
int gtav_proc_read_chain(int pid, uintptr_t anchor, uintptr_t offset, size_t n, uint64_t* base_out,
                         void* value_out, int* value_read) {
  uint64_t base = 0;
  int base_ok;
  int val_ok = 0;

  if (base_out != NULL) {
    *base_out = 0;
  }
  if (value_read != NULL) {
    *value_read = 0;
  }
  if (pid <= 0 || value_out == NULL || n == 0 || n > sizeof(uint64_t)) {
    return fail("read_chain: invalid argument pid=%d n=%zu", pid, n);
  }
#if GTAV_PROC_NOSTOP_IO
  // Two mdbg reads and no stop at all, so the watch lane's readiness poll costs the game nothing.
  if (gtav_proc_read_nostop(pid, anchor, &base, sizeof(base)) == 0) {
    if (base_out != NULL) {
      *base_out = base;
    }
    if (base != 0 && gtav_proc_read_nostop(pid, (uintptr_t)base + offset, value_out, n) == 0 &&
        value_read != NULL) {
      *value_read = 1;
    }
    return 0;
  }
#if GTAV_PROC_NOSTOP_STRICT
  return nostop_strict_refuse("read_chain base", anchor);
#else
  nostop_fallback_warn("read_chain base", anchor);
  base = 0;
#endif
#endif
  if (pt_attach(pid) != 0) {
    return -1;
  }
  base_ok = (pt_io(pid, PIOD_READ_D, anchor, &base, sizeof(base), "read_chain base") == 0);
  if (base_ok && base != 0) {
    val_ok =
        (pt_io(pid, PIOD_READ_D, (uintptr_t)base + offset, value_out, n, "read_chain value") == 0);
  }
  if (pt_detach(pid) != 0) {
    return -1;  // left the target stopped -> treat as unreachable; the caller must not inject
  }
  if (!base_ok) {
    return -1;  // base read failed -> target unreachable (maybe dead/recycled)
  }
  if (base_out != NULL) {
    *base_out = base;
  }
  if (val_ok && value_read != NULL) {
    *value_read = 1;
  }
  return 0;
}

int gtav_proc_protect(int pid, uintptr_t addr, size_t n, int prot) {
  if (kernel_set_vmem_protection(pid, (intptr_t)addr, n, prot) != 0) {
    return fail("protect: failed pid=%d addr=0x%lx errno=%d", pid, (unsigned long)addr, errno);
  }
  return 0;
}

#define GTAV_TARGET_PAGE 0x4000ULL

int gtav_proc_alloc_exec(int pid, size_t n, uintptr_t* addr_out, int* allocation_may_exist_out) {
  size_t len;
  int attempt;

  if (addr_out == NULL || n == 0) {
    return fail("alloc_exec: invalid argument");
  }
  *addr_out = 0;
  if (allocation_may_exist_out != NULL) {
    *allocation_may_exist_out = 0;
  }
  // Round to the target page: kernel_set_vmem_protection operates on whole pages, so
  // a sub-page length (e.g. a 0x1000 scratch on a 0x4000-page system) can EFAULT.
  len = (size_t)(((uint64_t)n + (GTAV_TARGET_PAGE - 1)) & ~(GTAV_TARGET_PAGE - 1));

  // Bounded retry applies only while remote mmap itself reports failure. Once mmap returns an
  // address, publish it immediately and never allocate a replacement on a detach/protect failure:
  // the first mapping may exist and callers must quarantine that process instead of hiding/leaking
  // several mappings behind a zero output.
  for (attempt = 0; attempt < 3; ++attempt) {
    RemoteMmapResult mmap_result;
    int mmap_rc;
    if (pt_attach(pid) != 0) {
      return -1;
    }
    mmap_rc = remote_mmap_rw(pid, len, &mmap_result);
    if (mmap_result.address_observed) {
      *addr_out = (uintptr_t)mmap_result.address;
    }
    if (mmap_result.allocation_may_exist && allocation_may_exist_out != NULL) {
      *allocation_may_exist_out = 1;
    }
    if (pt_detach(pid) != 0) {
      return -1;
    }
    if (mmap_rc > 0) {
      continue;  // mmap explicitly returned MAP_FAILED; safe to retry without leaking
    }
    if (mmap_rc < 0) {
      return -1;  // dispatch/control failed; result says whether an unknown allocation may exist
    }
    // PS5 enforces W^X; make the freshly-mapped RW page executable via the kernel
    // protection helper -- the same bypass pad_hook.c uses for its live .text patch.
    if (kernel_set_vmem_protection(
            pid, mmap_result.address, len,
            GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC) == 0) {
      return 0;
    }
    gtav_logf(
        "alloc_exec: full RWX failed pid=%d addr=0x%lx errno=%d len=0x%zx; trying page-at-a-time",
        pid, (unsigned long)mmap_result.address, errno, len);
    {
      int page_ok = 1;
      size_t off;
      for (off = 0; off < len; off += GTAV_TARGET_PAGE) {
        size_t chunk = (len - off) > GTAV_TARGET_PAGE ? GTAV_TARGET_PAGE : (len - off);
        if (kernel_set_vmem_protection(
                pid, mmap_result.address + off, chunk,
                GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC) != 0) {
          gtav_logf("alloc_exec: page RWX failed pid=%d addr=0x%lx len=0x%zx errno=%d", pid,
                    (unsigned long)(mmap_result.address + off), chunk, errno);
          page_ok = 0;
          break;
        }
      }
      if (page_ok) {
        gtav_logf("alloc_exec: page-at-a-time RWX succeeded pid=%d addr=0x%lx len=0x%zx", pid,
                  (unsigned long)mmap_result.address, len);
        return 0;
      }
    }
    fail("alloc_exec: set RWX failed pid=%d addr=0x%lx errno=%d (attempt %d)", pid,
         (unsigned long)mmap_result.address, errno, attempt + 1);
    return -1;  // do not leak a second mapping; *addr_out preserves the uncertain first one
  }
  return -1;  // g_last_error holds the last failure
}

int gtav_proc_start_thread(int pid, uintptr_t entry, uintptr_t arg, int* tid_out) {
  static const char kThreadName[] = "gtavmenu";
  // Layout within the scratch page: ScePthread out-handle at [0..8), thread name at
  // [16..), int3 return-trap at [0x200]. The page is RWX so the trap byte executes.
  const uintptr_t kNameOff = 16;
  const uintptr_t kTrapOff = 0x200;
  const uint8_t int3 = 0xCC;
  uintptr_t create_addr;
  uintptr_t scratch = 0;
  uint64_t handle = 0;
  long ret;

  if (entry == 0) {
    return fail("start_thread: null entry");
  }
  if (gtav_proc_resolve_sym(pid, GTAV_PROC_SYM_PTHREAD_CREATE, &create_addr) != 0) {
    return -1;  // g_last_error set by resolve
  }
  // Executable scratch: scePthreadCreate must return through an int3 in this page.
  if (gtav_proc_alloc_exec(pid, 0x1000, &scratch, NULL) != 0) {
    return -1;  // g_last_error set by alloc_exec
  }

  if (pt_attach(pid) != 0) {
    return -1;
  }
  if (pt_io(pid, PIOD_WRITE_D, scratch + kNameOff, (void*)(uintptr_t)kThreadName,
            sizeof(kThreadName), "start_thread name") != 0 ||
      pt_io(pid, PIOD_WRITE_D, scratch + kTrapOff, (void*)(uintptr_t)&int3, 1,
            "start_thread trap") != 0 ||
      pt_io(pid, PIOD_WRITE_D, scratch, &handle, sizeof(handle), "start_thread clear") != 0) {
    pt_detach(pid);
    return -1;
  }

  // int scePthreadCreate(ScePthread *thread, const ScePthreadAttr *attr,
  //                      void *(*entry)(void *), void *arg, const char *name)
  // Continue-to-breakpoint (not single-step): scePthreadCreate takes locks and starts
  // a thread, which deadlocks under single-step while the other threads are frozen.
  ret = pt_call_bp(pid, create_addr, scratch, 0, entry, arg, scratch + kNameOff, 0,
                   scratch + kTrapOff, "scePthreadCreate");
  pt_io(pid, PIOD_READ_D, scratch, &handle, sizeof(handle), "start_thread handle");

  if (pt_detach(pid) != 0) {
    return -1;
  }
  if (ret != 0) {
    return fail("start_thread: scePthreadCreate returned %ld", ret);
  }
  if (tid_out != NULL) {
    *tid_out = (int)(handle & 0x7fffffff);
  }
  return 0;
}

int gtav_proc_load_module(int pid, const char* path, int* module_id_out) {
  // Scratch page: path string at [0..), int3 return-trap at [0x800], module_start
  // result (pRes) at [0x900]. RWX so the trap byte executes.
  const uintptr_t kTrapOff = 0x800;
  const uintptr_t kResOff = 0x900;
  const uint8_t int3 = 0xCC;
  const uint32_t res_sentinel = 0xC0DEFADEu;  // unchanged => module_start never ran
  uint32_t res = res_sentinel;
  uintptr_t fn;
  uintptr_t scratch = 0;
  size_t pathlen;
  long ret;

  if (path == NULL) {
    return fail("load_module: null path");
  }
  pathlen = strlen(path) + 1;
  if (pathlen > kTrapOff) {
    return fail("load_module: path too long (%zu)", pathlen);
  }
  if (gtav_proc_resolve_sym(pid, GTAV_PROC_SYM_LOAD_START_MODULE, &fn) != 0) {
    return -1;  // g_last_error set by resolve
  }
  if (gtav_proc_alloc_exec(pid, 0x1000, &scratch, NULL) != 0) {
    return -1;  // g_last_error set by alloc_exec
  }

  if (pt_attach(pid) != 0) {
    return -1;
  }
  if (pt_io(pid, PIOD_WRITE_D, scratch, (void*)(uintptr_t)path, pathlen, "load_module path") != 0 ||
      pt_io(pid, PIOD_WRITE_D, scratch + kTrapOff, (void*)(uintptr_t)&int3, 1,
            "load_module trap") != 0 ||
      pt_io(pid, PIOD_WRITE_D, scratch + kResOff, &res, sizeof(res), "load_module res") != 0) {
    pt_detach(pid);
    return -1;
  }

  // SceKernelModule sceKernelLoadStartModule(const char* name, size_t argc,
  //   const void* argv, uint32_t flags, const void* opt, int* pRes)
  // Continue-to-breakpoint: it does file I/O + runs the module's start (which installs
  // the per-frame command-drain hook), so it must run at full speed, not single-stepped.
  // pRes (6th arg) receives module_start's return; the pre-seeded sentinel tells
  // "start ran" (pRes changes) from "loaded but start skipped" (sentinel intact).
  ret = pt_call_bp(pid, fn, scratch, 0, 0, 0, 0, scratch + kResOff, scratch + kTrapOff,
                   "sceKernelLoadStartModule");
  pt_io(pid, PIOD_READ_D, scratch + kResOff, &res, sizeof(res), "load_module res read");
  if (pt_detach(pid) != 0) {
    return -1;
  }
  gtav_logf("load_module: ret=0x%x pRes=0x%x (sentinel 0x%x => start did NOT run)",
            (unsigned)(ret & 0xffffffff), (unsigned)res, (unsigned)res_sentinel);
  if (ret < 0) {
    return fail("load_module: sceKernelLoadStartModule returned 0x%x",
                (unsigned)(ret & 0xffffffff));
  }
  if (module_id_out != NULL) {
    *module_id_out = (int)ret;
  }
  return 0;
}

uintptr_t gtav_proc_module_base(int pid, int module_id) {
  intptr_t base = kernel_dynlib_mapbase_addr(pid, (uint32_t)module_id);
  return base > 0 ? (uintptr_t)base : 0;
}
