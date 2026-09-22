#pragma once

// Menu-worker lifecycle + per-tick entry points: the small, type-free C ABI that the
// loader and host drive. The richer init/status types live in gtavmenu/abi.h, which this
// header deliberately does NOT pull in (keep this boundary minimal).
//
// Threading contract:
//   - gtav_menu_worker_tick runs on the scePad worker thread (render + input; NO script
//     context, so it must only touch worker-safe natives).
//   - gtav_menu_frame_tick runs on the game's script thread, driven by the PLAYER_PED_ID
//     frame hook -- the one place queued game-thread jobs drain in valid context.

#ifdef __cplusplus
extern "C" {
#endif

// 1 while the menu panel is shown, 0 while hidden.
int gtav_menu_is_visible(void);
// Show (visible != 0) or hide the menu panel.
void gtav_menu_set_visible(int visible);
// Advance one worker-thread tick: poll input and render. Worker thread only.
void gtav_menu_worker_tick(void);
// Advance one game-thread tick from the frame hook: drain queued jobs in valid script
// context. Game (script) thread only.
void gtav_menu_frame_tick(void);
// Isolated phase-interception primitive. Returns one only when a game-side diagnostic batch ran.
int gtav_menu_render_phase_tick(unsigned long long epoch);
// Monotonic worker-tick count since init (does not reset while the worker runs).
unsigned long long gtav_menu_tick_count(void);

#ifdef __cplusplus
}
#endif
