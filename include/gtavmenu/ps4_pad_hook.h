#pragma once

// PS4 touchpad input: an exact-slot swap of the game's scePadReadState import.
//
// The menu's buttons come from the frontend controls (IS_DISABLED_CONTROL_PRESSED); touches are not
// exposed as controls, so the optional Touchpad lane reads them from the game's own per-frame
// scePadReadState call. On CUSA00411 01.57 the import's GOT slot is plain RW data (no RELRO), so
// the swap is a single compare-and-exchange: the host verifies the slot points into libScePad's
// code and passes that exact value as pad_got_expected; the worker installs over that value only,
// only while the Touchpad setting is on, and puts it back on the game thread before a stop
// completes.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GTAV_PS4_PAD_HOOK_OFF = 0,
  GTAV_PS4_PAD_HOOK_INSTALLED = 1,
  GTAV_PS4_PAD_HOOK_RESTORED = 2,
  GTAV_PS4_PAD_HOOK_UNAVAILABLE = 3,  // no slot for this target, or the host did not verify it
  GTAV_PS4_PAD_HOOK_MISMATCH = 4,     // the slot no longer held the verified libScePad entry
  GTAV_PS4_PAD_HOOK_LOST = 5,         // something else replaced our entry; left alone
};

// The replacement import. Calls the verified original, latches the first touch and the touchpad
// click, and hides both from the game while the menu is open with the Touchpad lane on.
int gtav_ps4_pad_read_state_hook(int handle, void* data);

// PS4 plugin only (the development worker never calls it, so its slot keeps holding the hook):
// the address the import slot is swapped to, a trampoline in the gateway page that jumps to
// gtav_ps4_pad_read_state_hook, so a slot that cannot be put back never points into plugin code.
// 0 keeps the hook itself. Call before the host publishes the control block.
void gtav_ps4_pad_hook_set_entry(uint64_t entry);

// Game thread only. want=1 installs the swap (once; a refusal latches until want drops to 0),
// want=0 restores it. Returns the resulting GTAV_PS4_PAD_HOOK_* state.
uint32_t gtav_ps4_pad_hook_sync(int want);

// 1 while the swap is installed and has delivered at least one read.
int gtav_ps4_pad_hook_live(void);

// 1 while no thread is inside gtav_ps4_pad_read_state_hook.
int gtav_ps4_pad_hook_idle(void);

// Hide touches and the touchpad click from the game (menu open with the Touchpad lane on).
void gtav_ps4_pad_hook_set_hide(int hide);

// The latest touch sample from the hook.
typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t fingers;
  uint32_t clicked;
} GtavPs4TouchSample;
void gtav_ps4_pad_hook_sample(GtavPs4TouchSample* out);

#ifdef __cplusplus
}
#endif
