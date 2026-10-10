#pragma once

// GTA V game-thread TLS layout constants, shared by the code that inspects the
// rage script/game-thread context off the thread's FS base.
//
// On x86-64 a rage native handler reaches the active script thread through
//
//     mov rA, qword ptr fs:[0]                            ; thread TLS self-pointer
//     mov rB, qword ptr [rA - GTAV_TLS_GAME_CTX_OFFSET]   ; rage script-thread context
//     mov rC, qword ptr [rB + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET]
//
// The context slot is populated on a real game/script thread and NULL on the injected
// scePad worker, so a handler that dereferences it without a null check faults there.
// Both the feature layer (gtav_tls_game_ctx_slot in features.cpp) and the frame-hook
// probe (capture_probe_context in frame_hook.c) read this same slot, so the offsets are
// named once here rather than duplicated as bare literals in each.
//
// Both magnitudes are build-specific and move together (the context struct grew by 0x10
// between the two known builds):
//
// What these two slots ARE (offline RE of the decrypted 01.010.002 eboot, 2026-09-17):
//   * fs:[0] - GTAV_TLS_GAME_CTX_OFFSET is the ACTIVE rage scrThread. Its sole writer in the whole
//     image is scrThread::Run, which saves the previous value, installs itself, and restores.
//   * that + GTAV_TLS_CTX_NATIVE_THREAD_OFFSET is the script's CGameScriptHandler -- the
//     SCRIPT-RESOURCE OWNER, not merely a "native thread". GTA's own model-streaming natives
//     resolve it through exactly this chain to decide which script owns a streaming request:
//     REQUEST_MODEL's shared helper (0x1c07fb0) registers a type-0xE resource via
//     `mov rdi,[rax+0x198]; mov rax,[rdi]; call [rax+0x68]` (vtable slot 13), and
//     SET_MODEL_AS_NO_LONGER_NEEDED (0x1c09760) releases it via slot 15 ([vtable+0x78]).
//     See include/gtavmenu/script_handler_census.h.
// The macro name predates knowing this, and is now also a manifest key
// (loader.tlsCtxNativeThreadOffset) and a test constant, so it is deliberately NOT renamed.
//
//     build       ctx slot   native-thread field
//     01.005.000  -0x130     +0x188
//     01.010.002  -0x140     +0x198
//
// Derived offline from the decrypted eboot rather than on hardware: across the exec
// segment the `mov rax, fs:[0]` sites resolve to exactly 451 `[rax - 0x130]` loads on
// 01.005.000 and exactly 451 `[rax - 0x140]` loads on 01.010.002, and those loads are
// followed by 280 vs 282 `+0x188` / `+0x198` field reads respectively -- a 1:1
// correspondence across the two builds. A developer classifier rebuilds this evidence and a
// host test gates it.
//
// tools/feature_menu_target_cflags.py emits per-target
// -DGTAV_TLS_GAME_CTX_OFFSET=... / -DGTAV_TLS_CTX_NATIVE_THREAD_OFFSET=... when
// data/targets/<target>.json carries the "tlsGameCtxOffset" /
// "tlsCtxNativeThreadOffset" rows, which override these defaults.
#ifndef GTAV_TLS_GAME_CTX_OFFSET
#define GTAV_TLS_GAME_CTX_OFFSET 0x130u
#endif

// Offset of the native/owning-thread pointer inside the rage script-thread context. Read
// only by the diagnostic probe (never on a hot path), to confirm the captured context
// points at a real thread object rather than stale TLS.
#ifndef GTAV_TLS_CTX_NATIVE_THREAD_OFFSET
#define GTAV_TLS_CTX_NATIVE_THREAD_OFFSET 0x188u
#endif
