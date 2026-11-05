// PS4 touchpad lane: the scePadReadState GOT swap (see gtavmenu/ps4_pad_hook.h).

#include "gtavmenu/ps4_pad_hook.h"

#include <stddef.h>

#include "gtavmenu/pad_input.h"
#include "gtavmenu/ps4_slice.h"
#include GTAV_PS4_TARGET_HEADER

#ifndef GTAV_PS4_PAD_READ_STATE_GOT
#define GTAV_PS4_PAD_READ_STATE_GOT UINT64_C(0)
#endif

// The parts of ScePadData (0x78 bytes) the hook reads, at the offsets the 01.57 pad update uses.
typedef struct {
  uint32_t buttons;  // +0x00
  uint8_t sticks_and_analog[8];
  uint8_t motion[40];
  uint8_t touch_count;  // +0x34
  uint8_t touch_pad[7];
  uint16_t touch0_x;  // +0x3C
  uint16_t touch0_y;  // +0x3E
  uint8_t touch0_id;
  uint8_t touch0_pad[3];
  uint8_t touch1[8];  // +0x44
  uint8_t connected;  // +0x4C
} GtavPs4PadData;
_Static_assert(offsetof(GtavPs4PadData, touch_count) == 0x34, "ScePadData touch count");
_Static_assert(offsetof(GtavPs4PadData, touch0_x) == 0x3C, "ScePadData first touch");
_Static_assert(offsetof(GtavPs4PadData, connected) == 0x4C, "ScePadData connected flag");

typedef int (*GtavPs4ReadStateFn)(int, void*);

static volatile uint32_t g_touch_x;
static volatile uint32_t g_touch_y;
static volatile uint32_t g_touch_fingers;
static volatile uint32_t g_touch_clicked;
static volatile uint32_t g_hide;
// The verified libScePad entry the slot held when the swap went in (0 while not installed).
static uint64_t g_original;
// What the swap puts in the slot: 0 = the hook itself, else the host's trampoline to it.
static uint64_t g_entry;
// The value the swap put in (0 while not installed); the restore expects exactly this.
static uint64_t g_installed;

int gtav_ps4_pad_read_state_hook(int handle, void* data) {
  __atomic_add_fetch(&gtav_ps4_control.pad_hook_inflight, 1u, __ATOMIC_ACQ_REL);
  // The host-verified entry: nonzero whenever the swap can be in place.
  const GtavPs4ReadStateFn real = (GtavPs4ReadStateFn)(uintptr_t)__atomic_load_n(
      &gtav_ps4_control.pad_got_expected, __ATOMIC_ACQUIRE);
  const int result = real ? real(handle, data) : -1;
  GtavPs4PadData* pad = (GtavPs4PadData*)data;
  if (result == 0 && pad && pad->connected) {
    const uint32_t fingers = pad->touch_count;
    g_touch_fingers = fingers;
    if (fingers) {
      g_touch_x = pad->touch0_x;
      g_touch_y = pad->touch0_y;
    }
    g_touch_clicked = (pad->buttons & GTAV_PAD_BTN_TOUCH_PAD) ? 1u : 0u;
    if (__atomic_load_n(&g_hide, __ATOMIC_RELAXED)) {
      // The game tracks touches and swipes (01.57 pad update), so the menu's gestures must not
      // reach it while the menu is open.
      pad->touch_count = 0;
      pad->buttons &= ~GTAV_PAD_BTN_TOUCH_PAD;
    }
  }
  __atomic_add_fetch(&gtav_ps4_control.pad_hook_calls, 1u, __ATOMIC_RELAXED);
  __atomic_sub_fetch(&gtav_ps4_control.pad_hook_inflight, 1u, __ATOMIC_ACQ_REL);
  return result;
}

static void set_state(uint32_t state) {
  __atomic_store_n(&gtav_ps4_control.pad_hook_state, state, __ATOMIC_RELEASE);
}

static uint64_t swap_entry(void) {
  const uint64_t entry = __atomic_load_n(&g_entry, __ATOMIC_ACQUIRE);
  return entry ? entry : (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook;
}

void gtav_ps4_pad_hook_set_entry(uint64_t entry) {
  __atomic_store_n(&g_entry, entry, __ATOMIC_RELEASE);
  gtav_ps4_control.pad_hook_address = swap_entry();
}

uint32_t gtav_ps4_pad_hook_sync(int want) {
  uint64_t* slot = (uint64_t*)(uintptr_t)GTAV_PS4_PAD_READ_STATE_GOT;
  const uint32_t state = gtav_ps4_control.pad_hook_state;
  if (want) {
    if (state == GTAV_PS4_PAD_HOOK_INSTALLED || state == GTAV_PS4_PAD_HOOK_UNAVAILABLE ||
        state == GTAV_PS4_PAD_HOOK_MISMATCH || state == GTAV_PS4_PAD_HOOK_LOST) {
      return state;
    }
    // A stop is under way: the host may already have looked at the slot, so never swap it in now.
    const uint32_t host = __atomic_load_n(&gtav_ps4_control.host_flags, __ATOMIC_ACQUIRE);
    if (host & (GTAV_PS4_HOST_SHUTDOWN_REQUEST | GTAV_PS4_HOST_QUIESCE)) return state;
    const uint64_t expected = __atomic_load_n(&gtav_ps4_control.pad_got_expected, __ATOMIC_ACQUIRE);
    if (!slot || !expected) {
      set_state(GTAV_PS4_PAD_HOOK_UNAVAILABLE);
      return GTAV_PS4_PAD_HOOK_UNAVAILABLE;
    }
    const uint64_t entry = swap_entry();
    uint64_t current = expected;
    if (!__atomic_compare_exchange_n(slot, &current, entry, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      set_state(GTAV_PS4_PAD_HOOK_MISMATCH);
      return GTAV_PS4_PAD_HOOK_MISMATCH;
    }
    g_original = expected;
    g_installed = entry;
    set_state(GTAV_PS4_PAD_HOOK_INSTALLED);
    return GTAV_PS4_PAD_HOOK_INSTALLED;
  }
  __atomic_store_n(&g_hide, 0u, __ATOMIC_RELAXED);
  if (state != GTAV_PS4_PAD_HOOK_INSTALLED) {
    // A refusal is retried the next time the lane is turned on.
    if (state != GTAV_PS4_PAD_HOOK_OFF && state != GTAV_PS4_PAD_HOOK_RESTORED) {
      set_state(GTAV_PS4_PAD_HOOK_OFF);
      return GTAV_PS4_PAD_HOOK_OFF;
    }
    return state;
  }
  uint64_t current = g_installed;
  const int restored = __atomic_compare_exchange_n(slot, &current, g_original, 0, __ATOMIC_ACQ_REL,
                                                   __ATOMIC_ACQUIRE);
  g_original = 0;
  g_installed = 0;
  g_touch_fingers = 0;
  g_touch_clicked = 0;
  set_state(restored ? GTAV_PS4_PAD_HOOK_RESTORED : GTAV_PS4_PAD_HOOK_LOST);
  return gtav_ps4_control.pad_hook_state;
}

int gtav_ps4_pad_hook_live(void) {
  return gtav_ps4_control.pad_hook_state == GTAV_PS4_PAD_HOOK_INSTALLED &&
         gtav_ps4_control.pad_hook_calls != 0;
}

int gtav_ps4_pad_hook_idle(void) {
  return __atomic_load_n(&gtav_ps4_control.pad_hook_inflight, __ATOMIC_ACQUIRE) == 0;
}

void gtav_ps4_pad_hook_set_hide(int hide) {
  const int installed = gtav_ps4_control.pad_hook_state == GTAV_PS4_PAD_HOOK_INSTALLED;
  __atomic_store_n(&g_hide, (hide && installed) ? 1u : 0u, __ATOMIC_RELAXED);
}

void gtav_ps4_pad_hook_sample(GtavPs4TouchSample* out) {
  if (!out) return;
  out->x = g_touch_x;
  out->y = g_touch_y;
  out->fingers = g_touch_fingers;
  out->clicked = g_touch_clicked;
}
