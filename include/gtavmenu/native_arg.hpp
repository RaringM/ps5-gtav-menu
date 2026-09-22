#pragma once

// C++-only native-call plumbing (templated invoke helpers + the NativeArg ABI
// struct). This is shared module-internal code, not a stable C ABI boundary
// like the other gtavmenu/ headers -- it lives here only so both translation
// units that call natives include it the same way.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gtavmenu/render_diag.h"

namespace gtavmenu {

struct NativeArg {
  uint64_t* return_value;
  uint32_t arg_count;
  uint8_t padding1[4];
  uint64_t* arg_values;
  uint32_t vector_count;
  uint8_t padding2[4];
  void* arg_vectors[4];
  uint8_t temp_vectors[4][16];
};

static_assert(offsetof(NativeArg, return_value) == 0x0, "NativeArg return pointer offset changed");
static_assert(offsetof(NativeArg, arg_count) == 0x8, "NativeArg arg count offset changed");
static_assert(offsetof(NativeArg, arg_values) == 0x10, "NativeArg arg values offset changed");
static_assert(offsetof(NativeArg, vector_count) == 0x18, "NativeArg vector count offset changed");
static_assert(offsetof(NativeArg, arg_vectors) == 0x20, "NativeArg vector pointer offset changed");
static_assert(offsetof(NativeArg, temp_vectors) == 0x40, "NativeArg temp vector offset changed");

// --- crash breadcrumb -------------------------------------------------------
// g_last_native_addr holds the address of the GTA native we are about to call,
// stamped immediately before the indirect call. If a native faults (typically on a
// stale/freed entity handle resolved mid state-transition), the culprit's address
// survives in this fixed, named global -- recoverable from a PS5 coredump and
// surfaced on the periodic telemetry event -- instead of forcing a from-scratch
// backtrace decode (which an earlier such crash did require). g_last_native_seq
// advances every call so a reader can tell the breadcrumb is live. inline => one
// shared definition across both native-calling translation units (features.cpp +
// native_bridge.cpp) without a separate .cpp.
inline volatile uint64_t g_last_native_addr = 0;
inline volatile uint32_t g_last_native_seq = 0;

static inline void note_native_call(uint64_t address) {
  g_last_native_addr = address;
  g_last_native_seq = g_last_native_seq + 1;
}

struct NativeCallContext {
  uint64_t args[32];
  uint64_t returns[4];
  NativeArg native_arg;

  void reset() {
    memset(args, 0, sizeof(args));
    memset(returns, 0, sizeof(returns));
    memset(&native_arg, 0, sizeof(native_arg));
    native_arg.return_value = returns;
    native_arg.arg_values = args;
  }

  template <typename T>
  void push(T value) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "NativeArg scalar argument is too large");
    // Guard the fixed args[] buffer. arg_values points at args[], so an over-push would write
    // past it into returns[]/native_arg and corrupt the call context (and potentially the
    // native's view of its own arg array). No current binding pushes more than ~9 scalars, so
    // this never fires today; it exists so a future mis-binding drops the overflow arg instead
    // of smashing the context. This is the single chokepoint every push path (invoke_void /
    // invoke_return / the _ctx borrowed-TLS variants) flows through.
    if (native_arg.arg_count >= sizeof(args) / sizeof(args[0])) return;
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(value));
    args[native_arg.arg_count++] = raw;
  }

  // Drain any Vector3 out-params the native staged into the call-context scratch slots
  // back to the caller's buffers. A GTA V native with a Vector3* out-param (e.g.
  // GET_PED_LAST_WEAPON_IMPACT_COORD) does NOT write the result straight to the pointer we
  // passed: it records that pointer in arg_vectors[], writes the packed x/y/z into
  // temp_vectors[], and bumps vector_count. Without this drain the caller's buffer keeps
  // whatever it held before the call (typically 0/0/0). Mirrors rage
  // scrNativeCallContext::SetVectorResults; no-op when the native staged no vector
  // (vector_count stays 0), so it is safe to run after every call.
  void apply_vector_results() {
    while (native_arg.vector_count) {
      native_arg.vector_count--;
      // Destination is a rage scrVector: x/y/z each occupy their own 8-byte script slot
      // (padded), the layout every caller reads back. The scratch source is packed floats.
      uint8_t* dst = reinterpret_cast<uint8_t*>(native_arg.arg_vectors[native_arg.vector_count]);
      if (!dst) continue;
      const float* src =
          reinterpret_cast<const float*>(native_arg.temp_vectors[native_arg.vector_count]);
      memcpy(dst + 0, &src[0], sizeof(float));   // x -> slot 0
      memcpy(dst + 8, &src[1], sizeof(float));   // y -> slot 1 (offset 8)
      memcpy(dst + 16, &src[2], sizeof(float));  // z -> slot 2 (offset 16)
    }
  }

  template <typename... Args>
  void invoke_void(uint64_t address, Args... invoke_args) {
    // A null address is an unresolved native slot; jumping to 0 would fault the whole GTA
    // process, not just the worker. Call sites guard on g_n.<native>, but treat a null here
    // as a safe no-op so one missing guard can never crash the game.
    if (!address) return;
    reset();
    int unused[] = {0, ((void)push(invoke_args), 0)...};
    (void)unused;
    note_native_call(address);
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_native(address, 0);
#endif
    ((void (*)(NativeArg*))(uintptr_t)address)(&native_arg);
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_native(address, 1);
#endif
    apply_vector_results();
  }

  template <typename R, typename... Args>
  R invoke_return(uint64_t address, Args... invoke_args) {
    R value;
    memset(&value, 0, sizeof(value));
    // Null (unresolved) address: return a zeroed result instead of a null jump (see invoke_void).
    if (!address) return value;
    reset();
    int unused[] = {0, ((void)push(invoke_args), 0)...};
    (void)unused;
    note_native_call(address);
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_native(address, 0);
#endif
    ((void (*)(NativeArg*))(uintptr_t)address)(&native_arg);
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_native(address, 1);
#endif
    apply_vector_results();

#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_native_return(address, returns[0]);
#endif
    memcpy(&value, returns, sizeof(value));
    return value;
  }
};

template <typename... Args>
static inline void invoke_native_void(uint64_t address, Args... args) {
  NativeCallContext context;
  context.invoke_void(address, args...);
}

template <typename R, typename... Args>
static inline R invoke_native_return(uint64_t address, Args... args) {
  NativeCallContext context;
  return context.invoke_return<R>(address, args...);
}

}  // namespace gtavmenu
