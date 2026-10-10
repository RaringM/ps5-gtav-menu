#include "gtavmenu/frame_hook.h"

#include "gtavmenu/detour.h"
#include "gtavmenu/native_job_queue.h"
#include "gtavmenu/render_cycle_probe.h"
#include "gtavmenu/render_diag.h"
#include "gtavmenu/render_path_probe.h"
#include "gtavmenu/render_phase_intercept.h"
#include "gtavmenu/tls_layout.h"

#include <string.h>

#ifndef GTAV_FRAME_HOOK_DEFAULT_MAX_JOBS
#define GTAV_FRAME_HOOK_DEFAULT_MAX_JOBS 4u
#endif

// External-install mode (the .text patch is delivered by an external kernel-write
// agent, e.g. ps5debug, instead of in-process). Off by default.
#ifndef GTAV_FRAME_HOOK_EXTERNAL_INSTALL
#define GTAV_FRAME_HOOK_EXTERNAL_INSTALL 0
#endif

#ifndef GTAV_FRAME_HOOK_INTERNAL_ABI
#define GTAV_FRAME_HOOK_INTERNAL_ABI 0
#endif

#ifndef GTAV_FRAME_HOOK_CAPTURE_CONTEXT
#define GTAV_FRAME_HOOK_CAPTURE_CONTEXT 0
#endif

// Read the object the script context points at (ctx + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET).
//
// Despite the name -- which predates knowing what the slot is, and which is now also a manifest
// key (tlsCtxNativeThreadOffset) and a test constant, so it is NOT renamed -- offline RE of the
// 01.010.002 eboot identified this chain exactly: fs:[0] - GTAV_TLS_GAME_CTX_OFFSET is the
// active rage scrThread, and +GTAV_TLS_CTX_NATIVE_THREAD_OFFSET on it is that script's
// CGameScriptHandler. The model-streaming natives resolve their resource owner through the very
// same two loads (REQUEST_MODEL's helper at 0x1c07fb0, SET_MODEL_AS_NO_LONGER_NEEDED at
// 0x1c09760), so this is the pointer that decides WHICH script owns a streaming request.
//
// That makes it more than diagnostics: the feature layer censuses it to find a long-lived script
// to attribute streaming requests to (include/gtavmenu/script_handler_census.h). Still nothing in
// the job gate or the drain consumes it -- the gate needs the context pointer alone.
//
// One guarded load per fire, at the hook's full rate, so it stays off by default.
#ifndef GTAV_FRAME_HOOK_CAPTURE_SCRIPT_HANDLER
#define GTAV_FRAME_HOOK_CAPTURE_SCRIPT_HANDLER 0
#endif

// Walk one link FURTHER, to the handler's vtable, and report it too. A second dereference whose
// only use is confirming once that every observed handler shares a vtable, so it is separately
// gated and off by default.
#ifndef GTAV_FRAME_HOOK_CAPTURE_NATIVE_THREAD
#define GTAV_FRAME_HOOK_CAPTURE_NATIVE_THREAD 0
#endif

// Runtime TLS calibration (diagnostic): when the compile-time GTAV_TLS_GAME_CTX_OFFSET
// is not yet pinned for this build, scan a bounded window below the thread's
// real FS base (rdfsbase) for the game-context slot and cache the winning magnitude. The
// offset must be baked into the target manifest for a production build; a per-fire scan
// never ships. Gated here so the stable worker-only lane stays byte-identical.
#ifndef GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
#define GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX 0
#endif

// Fallback deref of the context slot at the REAL FS base register (rdfsbase - OFFSET),
// for a hypothetical build where `fs:0` is not the TLS self-pointer. It is NOT needed on
// either known build: `movq %fs:0` returns the thread's TLS base on both, which two live
// PS5 fault dumps confirm directly (the faulting frames hold the value just read from
// fs:0 -- r14=0x6fd630000 and rax=0x6fd660000, both plausible SceKernelTlsBlkApp bases).
// The earlier "fs:0 holds 0x6f on 01.010.002" reading was an artifact of the status-event
// ring truncating the telemetry line at GTAV_MENU_STATUS_EVENT_LEN (80), which cut the
// hex mid-number; see tick_frame_hook_telemetry in features.cpp. Default OFF: on FreeBSD
// amd64 the FS base sits at the start of the TLS allocation, so a negative offset off the
// register can land on an unmapped guard page and SIGSEGV GTA on the first fire
// (live-crashed 2026-09-16).
#ifndef GTAV_FRAME_HOOK_TLS_DEREF_REAL_FS
#define GTAV_FRAME_HOOK_TLS_DEREF_REAL_FS 0
#endif

// Bounds either side of the base for the calibration search AND the
// GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC sweep, so neither can walk far off an unmapped
// TLS guard page.
#ifndef GTAV_FRAME_HOOK_TLS_SCAN_WINDOW
// Diagnostic sweep bound. The game-thread TLS block on 01.010.002 is a full
// SceKernelTlsBlkApp* region (0x18000 bytes, e.g. App4 @ 0x6fd060000), so a
// 0x2000 window only sampled the block's first pages and missed any slot the
// game keeps at a large negative offset from the block base. 0x10000 covers a
// contiguous span across several App/Sys TLS blocks without risking a long walk
// past them.
#define GTAV_FRAME_HOOK_TLS_SCAN_WINDOW 0x10000u
#endif

#ifndef GTAV_FRAME_HOOK_DRAIN_JOBS
#define GTAV_FRAME_HOOK_DRAIN_JOBS 1
#endif

#ifndef GTAV_FRAME_HOOK_REQUIRE_CONTEXT_FOR_JOBS
#define GTAV_FRAME_HOOK_REQUIRE_CONTEXT_FOR_JOBS 0
#endif

// REPLACE mode (the experimental prologue-replace model): overwrite a per-frame function's
// entry with a jump to a plain thunk that does the work and RETURNS a passthrough value
// -- NO gateway, NO chaining back into the original. It exists because the IN-PROCESS
// trampoline install (gtav_frame_hook_install below, which reads + rewrites the target's
// own .text from inside the host) crashes on PS5: module .text is execute-only/no-read and
// an in-process .text write is unprivileged (proven 2026-06-13).
// NOTE: the SHIPPING live lane is NOT REPLACE -- it is the external-install CHAIN gateway
// (GTAV_FRAME_HOOK_EXTERNAL_INSTALL), where the privileged .text write is done out-of-process
// by the loader via kernel R/W, so the chain/gateway works. REPLACE is only valid on a
// REPLACE-SAFE target where skipping the original is harmless (a simple predicate), and the
// DOES_CAM_EXIST replace path was itself live-confirmed unsafe 2026-06-14 (see frame_hook.h).
// The replaced fn's first integer arg arrives in rdi and is returned by default (truthy for
// a predicate); override the return constant with GTAV_FRAME_HOOK_REPLACE_RET.
#ifndef GTAV_FRAME_HOOK_REPLACE
#define GTAV_FRAME_HOOK_REPLACE 0
#endif

#ifndef GTAV_FRAME_HOOK_PATCH_LEN
#define GTAV_FRAME_HOOK_PATCH_LEN 14u
#endif

#if GTAV_FRAME_HOOK_REPLACE && (GTAV_FRAME_HOOK_PATCH_LEN != 16u)
#error "GTAV_FRAME_HOOK_REPLACE requires GTAV_FRAME_HOOK_PATCH_LEN == 16 for xotext-safe install"
#endif

#ifndef GTAV_FRAME_HOOK_STOLEN_LEN
#define GTAV_FRAME_HOOK_STOLEN_LEN 15u
#endif

// The exact prologue bytes the gateway replicates before resuming the original
// handler at GTAV_FRAME_HOOK_CONTINUATION. Default is GET_VEHICLE_PED_IS_IN's
// 15-byte prologue; override (with STOLEN_LEN/PATCH_LEN/CONTINUATION/TARGET) to
// retarget the hook to a different clean-prologue native (e.g. the per-frame
// camera handler at 0x179d210, which fires while idle from a stable context).
#ifndef GTAV_FRAME_HOOK_STOLEN_BYTES
#define GTAV_FRAME_HOOK_STOLEN_BYTES \
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x8b, 0x47, 0x10
#endif

// Bytes the gateway actually emits to reproduce the stolen prologue. Defaults to
// STOLEN_BYTES (verbatim copy), but for a target whose prologue contains a
// RIP-relative instruction the gateway must emit a RELOCATED equivalent (a copy
// at a new address would compute the wrong absolute target). In that case
// GATEWAY_BYTES differs from STOLEN_BYTES (which stays the literal on-target bytes
// used for validation/restore), and CONTINUATION = target + original prologue len.
#ifndef GTAV_FRAME_HOOK_GATEWAY_BYTES
#define GTAV_FRAME_HOOK_GATEWAY_BYTES GTAV_FRAME_HOOK_STOLEN_BYTES
#endif

static GtavNativeJobQueue g_queue;
static GtavDetour g_detour;
// Exported chain pointer so the loader can pre-load it in broker-first/worker-last mode
// before the external .text patch lands. The thunk uses this to resume the gateway even
// before the worker thread has reached gtav_frame_hook_install_external().
void* g_gateway __attribute__((used, retain, visibility("default")));
static GtavFrameHookJobFn g_job_fn;
// Optional per-frame effect tick (continuous game-thread effects like explosive ammo).
// Managed solely by gtav_frame_hook_set_tick_fn(), independent of install mode, so its
// registration order relative to install does not matter.
static GtavFrameHookTickFn g_tick_fn;
static void* g_job_context;
static uint32_t g_max_jobs;
static volatile int g_active;
static volatile uint32_t g_call_count;
static volatile uint32_t g_jobs_run;
// Re-entrancy/concurrency guard for the drain: set while a drain is in progress so a
// re-entrant or cross-thread hook fire cannot drain the queue underneath an in-flight
// job (see gtav_frame_hook_run_pending_jobs).
static volatile uint32_t g_draining;
// Only the accepted callback owns this value. Last-fire probe fields below can be overwritten
// before g_draining rejects a nested/cross-thread fire and must never select a resource owner.
static uint64_t g_callback_script_handler;
static volatile uint64_t g_probe_calls;
static volatile uint64_t g_probe_calls_with_context;
static volatile uintptr_t g_probe_last_fsbase;
static volatile uintptr_t g_probe_last_real_fsbase;
static volatile uintptr_t g_probe_last_script_context;
static volatile uintptr_t g_probe_last_native_thread;
static volatile uintptr_t g_probe_last_native_thread_vtable;

#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL
extern uintptr_t gtav_frame_hook_gateway_continuation_value;
void gtav_frame_hook_gateway(void);
#endif

#if GTAV_FRAME_HOOK_INTERNAL_ABI && defined(__x86_64__)
void gtav_frame_hook_internal_entry(void);
#endif

#ifndef GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC
#define GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC 0
#endif

// Capture the FS base from the register (rdfsbase) alongside `fs:0`. Purely diagnostic:
// `fs:0` is the proven anchor on both known builds, and this costs a CPUID + RDFSBASE on
// the game thread's hot path (the hook fires ~8.5k/sec), so it stays off by default per
// the "hook code stays minimal and deterministic" rule. Turn it on only when bringing up
// a new build whose TLS anchor is in question.
#ifndef GTAV_FRAME_HOOK_USE_REAL_FS_BASE
#define GTAV_FRAME_HOOK_USE_REAL_FS_BASE 0
#endif

#if GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC || GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
#include "gtavmenu/status.h"
#endif

// Is this value plausibly a PS5 user-space HEAP/TLS pointer we may dereference?
//
// Deliberately narrow, because every caller is about to deref it ON THE GAME THREAD at
// the hook's full fire rate: the game's own TLS blocks, script contexts and thread
// objects all sit high (0x6fd060000 App4 TLS, 0x2.../0x3... heap), while the eboot image
// itself maps low at 0x400000. Rejecting the low range costs nothing here -- the probe
// never dereferences a module pointer, only stores one (the vtable) for reporting.
//
// The alignment test is the cheap half of the guard: every pointer in this chain is an
// aligned 8-byte slot, so an odd value is garbage from a thread whose TLS is not laid out
// the way this build expects, and must not be followed.
//
// Gated to match its only callers (the probe body), so a build without context capture
// does not carry an unused static.
#if GTAV_FRAME_HOOK_CAPTURE_CONTEXT && (defined(__x86_64__) || defined(__amd64__))
static int tls_ptr_is_deref_safe(uintptr_t p) {
  if (p < 0x100000000ull || p >= 0x8000000000000ull) return 0;
  return (p & 0x7u) == 0;
}
#endif

#if GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC || GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
// Range test for a scan/calibrate candidate.
//
// WARNING: passing this is NOT a licence to dereference. A sweep of the TLS block turns up
// many words that sit in the user range yet are not mapped -- and the only way to find out
// in-process is to fault. tls_candidate_plausible() below does dereference, which is why
// the sweep can kill the game and why it is off by default (see make/flags.mk). Read that
// note before enabling either gate.
static int tls_ptr_is_user(uintptr_t p) {
  return p >= 0x100000000ull && p < 0x8000000000000ull;
}
#endif

#if GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC
#define TLS_SCAN_SLOTS 16
static volatile int g_tls_scan_done;
static volatile uintptr_t g_tls_scan_off[TLS_SCAN_SLOTS];
static volatile uintptr_t g_tls_scan_val[TLS_SCAN_SLOTS];
static volatile uintptr_t g_tls_scan_nt[TLS_SCAN_SLOTS];
static volatile int g_tls_scan_count;

static int tls_candidate_plausible(uintptr_t val, uintptr_t base) {
  // A plausible rage::scrThreadContext: a pointer whose native-thread slot is also a
  // pointer. (The frame-hook probe reads the same two slots.)
  //
  // The filters below are as much as can be checked WITHOUT touching the candidate, so
  // they run first and reject the overwhelming majority: null, the base itself, anything
  // outside the user range, and anything misaligned (every slot in this chain is an
  // aligned qword, so an odd value is noise). What is left is still only a guess -- the
  // dereference that follows is the unsafe step this whole diagnostic rests on, and it is
  // what faulted GTA on 2026-09-17 when a 0x606cc0023066d word got this far.
  if (!val || val == base) return 0;
  if (!tls_ptr_is_user(val) || (val & 0x7u) != 0u) return 0;
  uintptr_t nt = *(volatile uintptr_t*)(val + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET);
  return nt && tls_ptr_is_user(nt);
}

static void tls_scan(uintptr_t fsbase) {
  if (g_tls_scan_done) return;
  g_tls_scan_done = 1;
  int n = 0;
  if (!fsbase) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "tls scan skipped fsbase=0");
    return;
  }
  // Negative offsets first (the game stores the script-context slot BELOW the FS base:
  // fs:-GTAV_TLS_GAME_CTX_OFFSET on both known builds), then positive. Each hit must look
  // like a scrThreadContext (kernel val with a kernel pointer at val+0x188). The game's TLS
  // block is contiguously mapped for at least a few pages; GTAV_FRAME_HOOK_TLS_SCAN_WINDOW
  // bounds the sweep so it cannot walk far off an unmapped guard page.
  for (int sign = -1; sign <= 1 && n < TLS_SCAN_SLOTS; sign += 2) {
    for (uintptr_t off = sizeof(uintptr_t);
         off < GTAV_FRAME_HOOK_TLS_SCAN_WINDOW && n < TLS_SCAN_SLOTS; off += sizeof(uintptr_t)) {
      if (sign < 0 && fsbase < off) break;
      uintptr_t addr = sign < 0 ? fsbase - off : fsbase + off;
      uintptr_t val = *(volatile uintptr_t*)addr;
      if (tls_candidate_plausible(val, fsbase)) {
        g_tls_scan_off[n] = (uintptr_t)(intptr_t)(sign < 0 ? -(intptr_t)off : (intptr_t)off);
        g_tls_scan_val[n] = val;
        g_tls_scan_nt[n] = *(volatile uintptr_t*)(val + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET);
        n++;
      }
    }
  }
  g_tls_scan_count = n;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "tls scan realfs=0x%llx candidates=%d",
                     (unsigned long long)fsbase, n);
  for (int i = 0; i < n && i < TLS_SCAN_SLOTS; i++) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                       "tls candidate %d off=%lld val=0x%llx nt=0x%llx", i,
                       (long long)(intptr_t)g_tls_scan_off[i],
                       (unsigned long long)g_tls_scan_val[i], (unsigned long long)g_tls_scan_nt[i]);
  }
}
#endif

#if GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
// Runtime re-discovery of the game-context TLS offset for a build whose compile-time
// GTAV_TLS_GAME_CTX_OFFSET is not yet known. Both known builds are pinned in their target
// manifest (see include/gtavmenu/tls_layout.h), so this never runs in a shipping build; it
// exists to bootstrap a new game version. The game stores the context slot at a NEGATIVE offset
// below the real FS base (fs:-OFF). Scans downward for a slot whose value looks like a
// scrThreadContext and caches the magnitude. Diagnostic only: the winner must be baked into the
// target manifest + GTAV_TLS_GAME_CTX_OFFSET for production.
static uintptr_t g_calibrated_ctx_offset;  // magnitude m where *(fsbase - m) holds the context

static uintptr_t calibrate_ctx_offset(uintptr_t fsbase) {
  if (!fsbase) return 0;
  if (g_calibrated_ctx_offset) return g_calibrated_ctx_offset;
  for (uintptr_t off = sizeof(uintptr_t); off <= GTAV_FRAME_HOOK_TLS_SCAN_WINDOW && fsbase >= off;
       off += sizeof(uintptr_t)) {
    uintptr_t ctx = *(volatile uintptr_t*)(fsbase - off);
    if (tls_candidate_plausible(ctx, fsbase)) {
      g_calibrated_ctx_offset = off;
      gtav_status_eventf(
          GTAV_MENU_EVENT_NATIVE_BRIDGE, "tls calibrate ctx_off=0x%llx ctx=0x%llx realfs=0x%llx",
          (unsigned long long)off, (unsigned long long)ctx, (unsigned long long)fsbase);
      return off;
    }
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "tls calibrate ctx_off none in -0x1..-0x%llx",
                     (unsigned long long)GTAV_FRAME_HOOK_TLS_SCAN_WINDOW);
  return 0;
}
#endif

#if GTAV_FRAME_HOOK_DRAIN_JOBS && !GTAV_RENDER_DIAG_ISOLATED
// Adapter so the queue's generic drain callback forwards to the installed job fn.
static void run_one_job(uint32_t action, uint32_t param, void* context) {
  (void)context;
  if (g_job_fn) {
    g_job_fn(action, param, g_job_context);
  }
}
#endif

static void prepare_queue_state(GtavFrameHookJobFn job_fn, void* job_context,
                                uint32_t max_jobs_per_frame, void* gateway) {
  gtav_job_queue_init(&g_queue);
  g_job_fn = job_fn;
  g_job_context = job_context;
  g_max_jobs = max_jobs_per_frame ? max_jobs_per_frame : GTAV_FRAME_HOOK_DEFAULT_MAX_JOBS;
  g_call_count = 0;
  g_jobs_run = 0;
  g_draining = 0;
  g_probe_calls = 0;
  g_probe_calls_with_context = 0;
  g_probe_last_fsbase = 0;
  g_probe_last_real_fsbase = 0;
  g_probe_last_script_context = 0;
  g_probe_last_native_thread = 0;
  g_probe_last_native_thread_vtable = 0;
  g_gateway = gateway;
}

#if GTAV_FRAME_HOOK_CAPTURE_CONTEXT && (defined(__x86_64__) || defined(__amd64__))
#if GTAV_FRAME_HOOK_USE_REAL_FS_BASE
// Diagnostic only (see GTAV_FRAME_HOOK_USE_REAL_FS_BASE). The fs:0-only probe shape is the
// one that ran crash-free at ~8.5k fires/sec on 01.010.002; keep the per-fire path
// minimal/deterministic and do not add work here without a reason.
// CPUID is serializing and the answer cannot change for the life of the process, so
// latch it on first use instead of paying for it on every hook fire.
static int cpu_has_fsgsbase(void) {
  static int cached = -1;
  if (cached < 0) {
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7u), "c"(0u));
    cached = (ebx & (1u << 0)) != 0;  // CPUID.(EAX=7,ECX=0).EBX.FSGSBASE
  }
  return cached;
}

static uintptr_t read_real_fs_base(void) {
  if (cpu_has_fsgsbase()) {
    uintptr_t base = 0;
    // rdfsbase %rax (F3 0F AE C0), emitted as bytes so no assembler-target dependency.
    __asm__ volatile(".byte 0xf3, 0x0f, 0xae, 0xc0" : "=a"(base));
    if (base > 0x1000u) {
      return base;  // plausible user TLS base (kernel-mapped, way above a scratch id)
    }
  }
  return 0;  // unsupported or implausible: caller keeps using the legacy fs:0 base
}
#endif  // GTAV_FRAME_HOOK_USE_REAL_FS_BASE
#endif  // GTAV_FRAME_HOOK_CAPTURE_CONTEXT && x86_64

static int capture_probe_context(GtavFrameHookProbeSnapshot* sample) {
  uintptr_t fsbase = 0;
  uintptr_t script_context = 0;
  uintptr_t native_thread = 0;
  uintptr_t native_thread_vtable = 0;

  g_probe_calls++;

#if GTAV_FRAME_HOOK_CAPTURE_CONTEXT && (defined(__x86_64__) || defined(__amd64__))
  // Legacy base first, so proven builds (01.005.000) behave exactly as before.
  __asm__ volatile("movq %%fs:0, %0" : "=r"(fsbase));

  // Real FS base via the register (the only reliable TLS anchor on 01.010.002).
  uintptr_t real_fsbase = 0;
#if GTAV_FRAME_HOOK_USE_REAL_FS_BASE
  real_fsbase = read_real_fs_base();
#endif

  // The hook CHAINS a native every caller reaches, so this runs on whatever thread called
  // it -- not necessarily a script thread with the TLS layout this build pins. Validate
  // before following each link: the probe is diagnostics only, and a probe that can fault
  // is strictly worse than one that reports nothing (a fault here kills GTA outright,
  // which is unrecoverable, whereas a missed sample costs one line of telemetry).
  if (tls_ptr_is_deref_safe(fsbase) && fsbase >= GTAV_TLS_GAME_CTX_OFFSET) {
    script_context = *(uintptr_t*)(fsbase - GTAV_TLS_GAME_CTX_OFFSET);
  }
#if GTAV_FRAME_HOOK_TLS_DEREF_REAL_FS
  if (!script_context && tls_ptr_is_deref_safe(real_fsbase) &&
      real_fsbase >= GTAV_TLS_GAME_CTX_OFFSET) {
    script_context = *(uintptr_t*)(real_fsbase - GTAV_TLS_GAME_CTX_OFFSET);
  }
#endif
#if GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
  // Build-specific offset mismatch: scan downward from the real base and cache the
  // magnitude so the context slot resolves every fire after the first.
  if (!script_context && tls_ptr_is_deref_safe(real_fsbase)) {
    uintptr_t off = calibrate_ctx_offset(real_fsbase);
    if (off && real_fsbase >= off) {
      script_context = *(uintptr_t*)(real_fsbase - off);
    }
  }
#endif
  // A slot that is non-zero but not a plausible pointer means this thread's TLS does not
  // match the pinned layout; keep the raw value for telemetry but do NOT walk it.
  if (!tls_ptr_is_deref_safe(script_context)) {
    script_context = 0;
  }
#if GTAV_FRAME_HOOK_CAPTURE_SCRIPT_HANDLER || GTAV_FRAME_HOOK_CAPTURE_NATIVE_THREAD
  if (script_context) {
    native_thread = *(uintptr_t*)(script_context + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET);
#if GTAV_FRAME_HOOK_CAPTURE_NATIVE_THREAD
    if (tls_ptr_is_deref_safe(native_thread)) {
      native_thread_vtable = *(uintptr_t*)native_thread;
    }
#endif
  }
#endif
#if GTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC
  tls_scan(real_fsbase ? real_fsbase : fsbase);
#endif
  g_probe_last_fsbase = fsbase;
  g_probe_last_real_fsbase = real_fsbase;
#else
  (void)fsbase;
#endif

  g_probe_last_script_context = script_context;
  g_probe_last_native_thread = native_thread;
  g_probe_last_native_thread_vtable = native_thread_vtable;
  // Stack-local attribution survives nested/cross-thread updates to the legacy last-fire probe.
  sample->last_fsbase = fsbase;
  sample->last_script_context = script_context;
  sample->last_native_thread = native_thread;
  if (script_context) {
    g_probe_calls_with_context++;
  }
  return script_context != 0;
}

// Start the worker thread from inside the game, on the first hook fire, instead of having the
// loader inject a thread by ptrace. PT_ATTACH permanently breaks GTA's streaming I/O on
// ppsa04264-01.010.002 (docs/ptrace-free-injection.md), and gtav_elf_start_thread was the last
// ptrace site in an inject; with the ptrace-free lane supplying the worker's memory from the cave
// bootstrap, this removes the other one. The worker keeps its own thread afterwards, so rendering
// and scePad input are unchanged -- only who calls pthread_create moves.
//
// Off by default: the loader still starts the thread on the classic lane, and a second starter
// would double-load the worker.
#ifndef GTAV_FRAME_HOOK_SELF_START_WORKER
#define GTAV_FRAME_HOOK_SELF_START_WORKER 0
#endif

#if GTAV_FRAME_HOOK_SELF_START_WORKER
int gtav_menu_start_thread(void);

static volatile uint32_t g_self_start_claimed;

// The loader opens this gate only after the complete broker transaction succeeds and it has
// retained lifecycle ownership. A detour may fire during jump verification or rollback; those
// calls must remain passthroughs without creating a worker. Keep this separate from broker.state:
// even a failed INSTALLED publication may have changed the remote bytes before returning failure.
volatile uint32_t gtav_frame_hook_self_start_authorized
    __attribute__((used, retain, visibility("default"))) = 0u;

// Exported so the loader can read the outcome over kernel R/W after the hook goes live: this is the
// only evidence that pthread_create from inside a chained native worked, and calling it mid-frame
// on the game thread is the one genuinely untested step of the ptrace-free lane. 0xFFFFFFFF = never
// attempted, 0 = the thread was created, anything else = the failure code.
volatile uint32_t gtav_frame_hook_self_start_rc
    __attribute__((used, retain, visibility("default"))) = 0xFFFFFFFFu;

// Exactly one fire may call it: the hook fires ~270x per frame, on a thread that can also re-enter
// itself, so the claim is taken with an atomic exchange BEFORE the call rather than after it. A
// failed start is not retried -- pthread_create failing here means the process is in no state for a
// second attempt, and retrying every fire would be ~8000 attempts a second.
static void self_start_worker_once(void) {
  if (__atomic_load_n(&gtav_frame_hook_self_start_authorized, __ATOMIC_ACQUIRE) != 1u) {
    return;
  }
  if (__atomic_load_n(&g_self_start_claimed, __ATOMIC_ACQUIRE)) {
    return;
  }
  if (__atomic_exchange_n(&g_self_start_claimed, 1u, __ATOMIC_ACQ_REL)) {
    return;
  }
  gtav_frame_hook_self_start_rc = (uint32_t)gtav_menu_start_thread();
}
#endif

uint32_t gtav_frame_hook_run_pending_jobs(void) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
  const uint64_t diag_start = gtav_render_diag_stamp();
  gtav_render_diag_hook_count(0);
#endif
#if GTAV_FRAME_HOOK_SELF_START_WORKER
  // Ahead of the g_active gate, and in this function rather than in frame_hook_thunk, because
  // g_active is set by the worker's own gtav_frame_hook_install_external() -- a self-start behind
  // that gate could never fire -- and because the internal-ABI install lane jumps straight here
  // without going through the thunk at all.
  self_start_worker_once();
#endif
  if (!g_active) {
    return 0;
  }
  g_call_count++;
  GtavFrameHookProbeSnapshot context;
  memset(&context, 0, sizeof(context));
#if GTAV_FRAME_HOOK_DRAIN_JOBS
  const int has_context = capture_probe_context(&context);
#if GTAV_FRAME_HOOK_REQUIRE_CONTEXT_FOR_JOBS
  if (!has_context) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_hook_count(3);
#endif
    return 0;
  }
#else
  (void)has_context;
#endif
  // Re-entrancy/concurrency guard, taken up front so the per-frame effect tick AND the
  // job drain are mutually exclusive with any re-entrant/cross-thread fire. A native run
  // from either (ADD_OWNED_EXPLOSION from the tick, CREATE_VEHICLE / GIVE_WEAPON_TO_PED
  // from a job) can re-enter the hooked native on this thread, or it can fire on another
  // thread; a nested run must not run the tick a second time nor drain the queue
  // underneath an in-flight job (head only advances after the job fn returns). Exactly
  // one tick-or-drain runs at a time; a contended fire returns immediately and recovers
  // on the next frame.
  if (__atomic_exchange_n(&g_draining, 1u, __ATOMIC_ACQUIRE)) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_hook_count(2);
#endif
    return 0;
  }
  __atomic_store_n(&g_callback_script_handler, context.last_native_thread, __ATOMIC_RELAXED);
#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_hook_count(1);
#endif
#if GTAV_RENDER_CYCLE_PROBE
  gtav_render_cycle_observe(context.last_fsbase, context.last_script_context);
#endif
#if GTAV_RENDER_PATH_PROBE
  gtav_render_path_observe(context.last_fsbase, context.last_script_context,
                           (uintptr_t)__builtin_frame_address(0));
#endif
#if GTAV_RENDER_PHASE_INTERCEPT
  gtav_render_phase_service();
#endif
#if !GTAV_RENDER_DIAG_ISOLATED
  // Per-frame effect tick: runs on EVERY fire (even with an empty queue) in valid script
  // context. Kept minimal/deterministic -- one indirect call when registered.
  if (g_tick_fn) {
    g_tick_fn();
  }
  uint32_t drained = 0;
  if (g_queue.head != g_queue.tail) {  // cheap idle-skip of the drain loop only
    drained = gtav_job_queue_drain(&g_queue, g_max_jobs, run_one_job, NULL);
    g_jobs_run += drained;
  }
#else
  const uint32_t drained = 0;
#endif
#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_hook_end(diag_start, context.last_fsbase, context.last_script_context);
#endif
  __atomic_store_n(&g_callback_script_handler, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_draining, 0u, __ATOMIC_RELEASE);
  return drained;
#else
  capture_probe_context(&context);
  return 0;
#endif
}

#if GTAV_FRAME_HOOK_INTERNAL_ABI && defined(__x86_64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl gtav_frame_hook_internal_entry\n"
    "gtav_frame_hook_internal_entry:\n"
    "  pushfq\n"
    "  pushq %rax\n"
    "  pushq %rbx\n"
    "  pushq %rcx\n"
    "  pushq %rdx\n"
    "  pushq %rsi\n"
    "  pushq %rdi\n"
    "  pushq %rbp\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  pushq %r10\n"
    "  pushq %r11\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  subq $8, %rsp\n"
    "  call gtav_frame_hook_run_pending_jobs\n"
    "  addq $8, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %r11\n"
    "  popq %r10\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rbp\n"
    "  popq %rdi\n"
    "  popq %rsi\n"
    "  popq %rdx\n"
    "  popq %rcx\n"
    "  popq %rbx\n"
    "  popq %rax\n"
    "  popfq\n"
    "  jmp *g_gateway(%rip)\n");
#endif

// Trampoline destination. Runs on the script/main thread because it replaces a
// native handler the game's scripts call. Signature matches a rage native
// handler: a single pointer argument (the scrNativeCallContext, in rdi). We must
// preserve that pointer and chain to the original handler via the gateway so the
// native still returns its real value.
void frame_hook_thunk(void* call_context) {
  // Keep the common per-fire path small: only queued jobs pay for the drain loop.
  gtav_frame_hook_run_pending_jobs();
  // Chain to the original handler (gateway = stolen prologue + jump back). If the
  // gateway is somehow unset, returning without chaining is safer than calling
  // through a null pointer; publish() guarantees it is set before the patch goes
  // live, so this guard should never trigger.
  void* gateway = g_gateway;
  if (gateway) {
    ((void (*)(void*))gateway)(call_context);
  }
}

// When the REPLACE target is a rage native handler (not a normal int(int) fn),
// the thunk must write the result into ctx->returnValue. Off by default (a plain
// internal-fn replace returns in rax); the DOES_CAM_EXIST handler build sets it.
#ifndef GTAV_FRAME_HOOK_REPLACE_NATIVE_CTX
#define GTAV_FRAME_HOOK_REPLACE_NATIVE_CTX 0
#endif

#if GTAV_FRAME_HOOK_REPLACE
#ifndef GTAV_FRAME_HOOK_REPLACE_RET
// Default: return the first arg (rdi) -- a passthrough that is truthy for a
// predicate like DoesCamExist(cam). -1 means "return arg0".
#define GTAV_FRAME_HOOK_REPLACE_RET ((uintptr_t)-1)
#endif
// REPLACE-mode destination: the jump overwrites the target's entry and lands here.
// We do NOT chain to the original. We run the per-frame work (drain the queue,
// calling gameplay natives FRESH via the job fn -> invoke path, like god mode) and
// return a passthrough value so the replaced predicate's callers tolerate us.
static uintptr_t frame_hook_replace_thunk(uintptr_t a0) {
  gtav_frame_hook_run_pending_jobs();
#if GTAV_FRAME_HOOK_REPLACE_NATIVE_CTX
  // The replaced target is a rage NATIVE HANDLER: a0 is the scrNativeCallContext*.
  // The script VM reads the result from ctx->returnValue (NOT our rax), so leaving
  // it stale feeds the caller heap garbage -- a script that then uses a bogus
  // handle can crash in a downstream native. Mirror the real handler's effective
  // behaviour (the replaced DOES_CAM_EXIST returns its cam arg, which the real
  // handler stores) by writing arg0 back into ctx->returnValue. NativeArg layout:
  // returnValue* at offset 0, argValues* at offset 0x10 (u64 indices 0 and 2).
  if (a0) {
    uint64_t* ctx = (uint64_t*)a0;
    uint64_t* ret = (uint64_t*)ctx[0];
    uint64_t* args = (uint64_t*)ctx[2];
    if (ret) {
      *ret = args ? args[0] : 0;
    }
  }
#endif
  if ((uintptr_t)GTAV_FRAME_HOOK_REPLACE_RET == (uintptr_t)-1) {
    return a0;  // passthrough
  }
  return (uintptr_t)GTAV_FRAME_HOOK_REPLACE_RET;
}
#endif

// Called by the detour layer with the gateway pointer BEFORE the patch is
// committed, so the thunk never observes a null gateway once it can fire.
static void publish_gateway(void* original, void* context) {
  (void)context;
  g_gateway = original;
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

int gtav_frame_hook_install(uintptr_t target_handler, GtavFrameHookJobFn job_fn, void* job_context,
                            uint32_t max_jobs_per_frame) {
  if (g_active) return GTAV_FRAME_HOOK_INSTALL_ERR_ALREADY_ACTIVE;
  if (!target_handler) return GTAV_FRAME_HOOK_INSTALL_ERR_NULL_TARGET;

  uint8_t prologue[GTAV_MENU_DETOUR_MAX_LEN];
  memcpy(prologue, (const void*)target_handler, sizeof(prologue));

  const int patch_len = gtav_detour_safe_patch_len(prologue, sizeof(prologue));
  if (patch_len < (int)GTAV_MENU_DETOUR_JUMP_LEN) {
    return GTAV_FRAME_HOOK_INSTALL_ERR_UNSAFE_PROLOGUE;
  }

  gtav_job_queue_init(&g_queue);
  g_job_fn = job_fn;
  g_job_context = job_context;
  g_max_jobs = max_jobs_per_frame ? max_jobs_per_frame : GTAV_FRAME_HOOK_DEFAULT_MAX_JOBS;
  g_call_count = 0;
  g_jobs_run = 0;
  g_gateway = NULL;

  void* original = NULL;
  const int rc = gtav_detour_install_trampoline_prevalidated_publish(
      &g_detour, target_handler, (void*)frame_hook_thunk, (size_t)patch_len, prologue,
      (size_t)patch_len, publish_gateway, NULL, &original);
  if (rc != 0) {
    return GTAV_FRAME_HOOK_INSTALL_ERR_DETOUR_FAILED;
  }
  g_gateway = original;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_active = 1;
  return GTAV_FRAME_HOOK_INSTALL_OK;
}

#if GTAV_FRAME_HOOK_REPLACE
int gtav_frame_hook_install_replace(uintptr_t target_handler, GtavFrameHookJobFn job_fn,
                                    void* job_context, uint32_t max_jobs_per_frame) {
  if (g_active) return -1;
  if (!target_handler) return -2;

  prepare_queue_state(job_fn, job_context, max_jobs_per_frame, NULL);

  uint32_t expected_len = 0;
  const uint8_t* expected = gtav_frame_hook_expected_bytes(&expected_len);

  // PS5 game text is execute-only from the module: reading target bytes here
  // faults before the patch lands. The live prologue is externally verified, then
  // supplied through GTAV_FRAME_HOOK_STOLEN_BYTES so install/restore can use only
  // prevalidated bytes and an atomic 16-byte write.
  const int rc = gtav_detour_install_abs_jump_prevalidated(
      &g_detour, target_handler, (void*)frame_hook_replace_thunk, (size_t)GTAV_FRAME_HOOK_PATCH_LEN,
      expected, expected_len, /*dry_run=*/0);
  if (rc != 0) {
    return -4;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_active = 1;
  return 0;
}
#else
int gtav_frame_hook_install_replace(uintptr_t target_handler, GtavFrameHookJobFn job_fn,
                                    void* job_context, uint32_t max_jobs_per_frame) {
  (void)target_handler;
  (void)job_fn;
  (void)job_context;
  (void)max_jobs_per_frame;
  return -1;
}
#endif

int gtav_frame_hook_restore(void) {
  if (!g_active) return 0;
  g_active = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  const int rc = g_detour.installed ? gtav_detour_restore(&g_detour) : 0;
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL
  // External-install lane (the live menu): the .text jump on the target native was
  // written out-of-process by the loader via kernel R/W, and g_detour was never
  // installed in-process -- so the restore above is a no-op and the jump stays live
  // for the rest of the session (the worker has no privilege to un-patch .text, and
  // the loader has long since exited). The still-live jump keeps routing the target
  // native through frame_hook_thunk, which is a transparent passthrough once g_active
  // is 0 (the drain early-returns) PROVIDED it can still chain to the gateway. Nulling
  // g_gateway here would turn that passthrough into a dead end: the thunk would stop
  // chaining to the original handler, so e.g. PLAYER_PED_ID would return garbage and
  // never set ctx->returnValue -- bricking the player ped and hanging the game on a
  // loading screen after Stop Runtime. Keep the compile-time gateway stub (which lives
  // in this ELF's .text and stays mapped after the worker thread exits) so the target
  // native keeps returning correctly.
#else
  g_gateway = NULL;
#endif
  return rc;
}

uintptr_t gtav_frame_hook_thunk_address(void) __attribute__((used, retain, visibility("default")));
uintptr_t gtav_frame_hook_thunk_address(void) {
#if GTAV_FRAME_HOOK_INTERNAL_ABI && defined(__x86_64__)
  return (uintptr_t)&gtav_frame_hook_internal_entry;
#else
#if GTAV_FRAME_HOOK_REPLACE
  return (uintptr_t)&frame_hook_replace_thunk;  // no-gateway replacement
#elif GTAV_FRAME_HOOK_EXTERNAL_INSTALL
  return (uintptr_t)&frame_hook_thunk;
#else
  return 0;
#endif
#endif
}

uintptr_t gtav_frame_hook_gateway_address(void)
    __attribute__((used, retain, visibility("default")));
uintptr_t gtav_frame_hook_gateway_address(void) {
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL && !GTAV_FRAME_HOOK_REPLACE
  return (uintptr_t)gtav_frame_hook_gateway;
#else
  return 0;  // REPLACE mode has no gateway
#endif
}

uintptr_t gtav_frame_hook_gateway_continuation(void)
    __attribute__((used, retain, visibility("default")));
uintptr_t gtav_frame_hook_gateway_continuation(void) {
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL && !GTAV_FRAME_HOOK_REPLACE
  return gtav_frame_hook_gateway_continuation_value;
#else
  return 0;
#endif
}

uintptr_t gtav_frame_hook_gateway_continuation_slot(void)
    __attribute__((used, retain, visibility("default")));
uintptr_t gtav_frame_hook_gateway_continuation_slot(void) {
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL && !GTAV_FRAME_HOOK_REPLACE
  return (uintptr_t)&gtav_frame_hook_gateway_continuation_value;
#else
  return 0;
#endif
}

uint32_t gtav_frame_hook_patch_len(void) {
  return GTAV_FRAME_HOOK_PATCH_LEN;
}

uint32_t gtav_frame_hook_stolen_len(void) {
  return GTAV_FRAME_HOOK_STOLEN_LEN;
}

const uint8_t* gtav_frame_hook_expected_bytes(uint32_t* len_out) {
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL || GTAV_FRAME_HOOK_REPLACE
  // Native ABI v2 PRXs can map read-only segments as no-read xotext. Keep this
  // table in writable data so the module can pass it to the detour installer.
  static uint8_t expected[GTAV_FRAME_HOOK_STOLEN_LEN] = {
      GTAV_FRAME_HOOK_STOLEN_BYTES,
  };
  if (len_out) {
    *len_out = (uint32_t)sizeof(expected);
  }
  return expected;
#else
  if (len_out) {
    *len_out = 0;
  }
  return NULL;
#endif
}

#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL && !GTAV_FRAME_HOOK_REPLACE
// Compile-time trampoline gateway, living in the payload's own .text (already
// executable, so we never need runtime executable memory). It replicates the
// known 15-byte stolen prologue of the hook target (GET_VEHICLE_PED_IS_IN) then
// jumps through a writable continuation pointer (= target + stolen length) to
// run the rest of the original handler. The byte sequence is target-specific; if
// the target changes, these bytes and the continuation must change with it.
// Variadic so a comma-separated byte list (GTAV_FRAME_HOOK_STOLEN_BYTES) stringifies
// to a single "0x..,0x.." argument for the asm .byte directive.
#define GTAV_FH_STR_(...) #__VA_ARGS__
#define GTAV_FH_STR(...) GTAV_FH_STR_(__VA_ARGS__)
uintptr_t gtav_frame_hook_gateway_continuation_value
    __attribute__((used, retain, visibility("default"))) = (uintptr_t)GTAV_FRAME_HOOK_CONTINUATION;
// ELF-resolvable thunk address: the worker-free broker split (broker-first, worker-last) needs the
// loader to land the frame-hook prologue jump from its own ELF image, so the jump destination must
// be derivable without asking the worker to publish it. Mirror the continuation `_value` pattern
// (frame_hook.c:503) AND its lane fork (frame_hook.c:427-439): the initializer must return the SAME
// thunk the worker publishes as st.thunk_addr, or the loader-derived jump destination will diverge
// from the byte the broker validated. Fork arm-for-arm with getter.
// Byte-mirror of the AUTHORITATIVE getter gtav_frame_hook_thunk_address() lane fork
// (frame_hook.c:427-439), so loader-derived thunk_addr is byte-equal by construction, in every
// lane, never "resolved against a different lane's symbol". This is the ELF-locator mirror the
// getter already provides for continuation; the thunk fork must fork arm-for-arm with it so the
// loader ELF-resolution and the worker-published st.thunk_addr are the SAME lane's bytes.
#if defined(__APPLE__)
#define GTAV_FRAME_HOOK_THUNK_DATA __attribute__((used, retain, visibility("default")))
#else
#define GTAV_FRAME_HOOK_THUNK_DATA \
  __attribute__((used, retain, visibility("default"), section(".data.rel.ro")))
#endif
uintptr_t gtav_frame_hook_thunk_value GTAV_FRAME_HOOK_THUNK_DATA =
#if GTAV_FRAME_HOOK_INTERNAL_ABI && defined(__x86_64__)
    (uintptr_t)&gtav_frame_hook_internal_entry;  // internal-handler ABI
#else
#if GTAV_FRAME_HOOK_REPLACE
    (uintptr_t)&frame_hook_replace_thunk;  // no-gateway replacement
#elif GTAV_FRAME_HOOK_EXTERNAL_INSTALL
    (uintptr_t)&frame_hook_thunk;
#else
    0;
#endif
#endif
__attribute__((used, retain, visibility("default"), naked)) void gtav_frame_hook_gateway(void) {
  __asm__ volatile(
      ".p2align 4\n"
      ".byte " GTAV_FH_STR(GTAV_FRAME_HOOK_GATEWAY_BYTES) "\n"
      "jmp *gtav_frame_hook_gateway_continuation_value(%rip)\n");
}
#endif

#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL
__attribute__((used, retain, visibility("default"))) int gtav_frame_hook_install_external(
    GtavFrameHookJobFn job_fn, void* job_context, uint32_t max_jobs_per_frame) {
  if (g_active) return -1;
#if GTAV_FRAME_HOOK_REPLACE
  // REPLACE mode: no gateway/chain. The jump lands on frame_hook_replace_thunk,
  // which does the work and returns a passthrough -- it never resumes the original.
  prepare_queue_state(job_fn, job_context, max_jobs_per_frame, NULL);
#else
  // The gateway is the compile-time stub; no runtime detour / .text write here.
  prepare_queue_state(job_fn, job_context, max_jobs_per_frame, (void*)gtav_frame_hook_gateway);
#endif
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_active = 1;
  return 0;
}
#else
int gtav_frame_hook_install_external(GtavFrameHookJobFn job_fn, void* job_context,
                                     uint32_t max_jobs_per_frame) {
  (void)job_fn;
  (void)job_context;
  (void)max_jobs_per_frame;
  return -1;
}
#endif

int gtav_frame_hook_prepare_jobs(GtavFrameHookJobFn job_fn, void* job_context,
                                 uint32_t max_jobs_per_frame) {
  if (g_active) return -1;
  prepare_queue_state(job_fn, job_context, max_jobs_per_frame, NULL);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_active = 1;
  return 0;
}

int gtav_frame_hook_is_active(void) {
  return g_active;
}

void gtav_frame_hook_set_tick_fn(GtavFrameHookTickFn tick_fn) {
  g_tick_fn = tick_fn;
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

int gtav_frame_hook_enqueue(uint32_t action, uint32_t param) {
#if GTAV_RENDER_DIAG_ISOLATED
  gtav_render_diag_blocked_job();
  return 0;
#endif
  if (!g_active) return 0;
  return gtav_job_queue_push(&g_queue, action, param);
}

void gtav_frame_hook_clear_jobs(void) {
  gtav_frame_hook_set_tick_fn(NULL);
  gtav_job_queue_init(&g_queue);
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

uint32_t gtav_frame_hook_call_count(void) {
  return g_call_count;
}

uint64_t gtav_frame_hook_current_script_handler(void) {
  // Callback/job-only API: the guard preserves this owner across nested/rejected entries.
  // The worker must use probe_snapshot for last-fire diagnostics, not this execution owner.
  return __atomic_load_n(&g_callback_script_handler, __ATOMIC_RELAXED);
}

uint32_t gtav_frame_hook_jobs_run(void) {
  return g_jobs_run;
}

uint32_t gtav_frame_hook_jobs_dropped(void) {
  return g_queue.dropped;
}

void gtav_frame_hook_probe_snapshot(GtavFrameHookProbeSnapshot* out) {
  if (!out) return;
  out->calls = g_probe_calls;
  out->calls_with_context = g_probe_calls_with_context;
  out->last_fsbase = g_probe_last_fsbase;
  out->last_real_fsbase = g_probe_last_real_fsbase;
  out->last_script_context = g_probe_last_script_context;
  out->last_native_thread = g_probe_last_native_thread;
  out->last_native_thread_vtable = g_probe_last_native_thread_vtable;
#if GTAV_FRAME_HOOK_TLS_CALIBRATE_CTX
  out->tls_ctx_off = g_calibrated_ctx_offset;
#else
  out->tls_ctx_off = 0;
#endif
}
