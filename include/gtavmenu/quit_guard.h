#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Worker-thread teardown/suspend guard. Called once per worker tick. It watches the frame-hook
// heartbeat (gtav_frame_hook_call_count) and -- when GTAV_MENU_QUIT_GUARD_APPSTATE is compiled in
// -- the PS5 system-service background flag, and parks/unparks the live input path
// (gtav_pad_input_park / gtav_pad_input_unpark) so the menu defensively un-hooks itself BEFORE GTA
// is force-closed or rest-mode-suspended. That is the teardown gtav_menu_shutdown() never sees: a
// force-close skips the graceful Stop Runtime path, leaving the scePad GOT swap pointing into the
// about-to-unmap worker. The decision logic lives in the pure, host-tested teardown_watchdog.h.
//
// No-op unless GTAV_MENU_ENABLE_QUIT_GUARD=1. The production profile enables it and the explicit
// smoke profile disables it.
void gtav_quit_guard_tick(void);

// Reset the guard state. Call from gtav_menu_init so a re-inject starts from a clean state.
void gtav_quit_guard_reset(void);

#ifdef __cplusplus
}
#endif
