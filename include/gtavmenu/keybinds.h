#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Customizable hotkeys: a small fixed table of (button-combo -> feature action) bindings.
 * A bound combo fires its action while the menu is CLOSED (see gtav_pad_input_map), letting
 * the user quick-toggle e.g. God Mode without opening the menu. Kept tiny so the profile INI
 * round-trip stays trivial and gtav_pad_input_map stays a pure, host-testable function.
 *
 * combo_mask is an OR of GTAV_PAD_BTN_* bits (pad_input.h); 0 = empty slot. A valid binding
 * uses at least two buttons (a lone face button would fire constantly) and must not equal the
 * menu open chord. action is a GTAV_NATIVE_SHELL_ACTION_* id restricted to the worker-safe
 * quick toggles/one-shots (see kBindableActions in shell_menu_defs.hpp); 0 = none.
 *
 * The three pieces of this feature: this struct + bindable list (shell_menu_defs.hpp), the
 * cycler/apply logic that lets the user pick a combo per slot (features/keybinds.inc), and
 * the closed-menu dispatch that fires a matched combo (gtav_pad_input_map in pad_input.c). */
#define GTAV_KEYBIND_SLOTS 4u

typedef struct GtavKeybind {
  uint32_t combo_mask;
  uint32_t action;
} GtavKeybind;

#ifdef __cplusplus
}
#endif
