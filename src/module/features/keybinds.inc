// Customizable hotkeys.
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.
//
// Each fixed slot binds one worker-safe quick toggle to a button combo chosen from a
// curated list. The chosen combo fires the toggle while the menu is CLOSED (detection +
// game-input suppression live in pad_input.c). Selections persist in the profile and are
// pushed into pad_input via gtav_pad_input_set_binds(). A curated combo list (rather than a
// live "capture" mode) keeps the whole feature pure/host-testable and avoids binding a combo
// that collides with the open chord or a single button.

// Fixed bindable features, one per keybind slot. All are worker-safe quick toggles, so
// firing them off-menu through handle_shell_action_param is safe. Slot index ==
// action - GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0.
struct KeybindSlot {
  char label[16];  // inline (no relocation; see feature_catalog.h)
  uint32_t action;
};
static const KeybindSlot kBindableActions[GTAV_KEYBIND_SLOTS] = {
    {"God Mode", GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE},
    {"Noclip", GTAV_NATIVE_SHELL_ACTION_TOGGLE_NOCLIP},
    {"Super Jump", GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_JUMP},
    {"Invisible", GTAV_NATIVE_SHELL_ACTION_TOGGLE_INVISIBLE},
};

// Curated combos (index 0 = Off/unbound). Every combo is multi-button and avoids DpadLeft
// (reserved by the R1+Left open chord), so a binding never collides with menu open/close or
// fires from a single press.
struct KeybindCombo {
  char label[12];  // inline (no relocation)
  uint32_t mask;   // GTAV_PAD_BTN_* OR-mask
};
static const KeybindCombo kKeybindCombos[] = {
    {"Off", 0u},
    {"L1+R1", GTAV_PAD_BTN_L1 | GTAV_PAD_BTN_R1},
    {"L1+Tri", GTAV_PAD_BTN_L1 | GTAV_PAD_BTN_TRIANGLE},
    {"L1+Sq", GTAV_PAD_BTN_L1 | GTAV_PAD_BTN_SQUARE},
    {"R1+Tri", GTAV_PAD_BTN_R1 | GTAV_PAD_BTN_TRIANGLE},
    {"R1+Sq", GTAV_PAD_BTN_R1 | GTAV_PAD_BTN_SQUARE},
    {"L1+R1+Tri", GTAV_PAD_BTN_L1 | GTAV_PAD_BTN_R1 | GTAV_PAD_BTN_TRIANGLE},
};
static const uint32_t kKeybindComboCount =
    (uint32_t)(sizeof(kKeybindCombos) / sizeof(kKeybindCombos[0]));

// Selected combo index per slot.
static uint32_t g_keybind_combo[GTAV_KEYBIND_SLOTS];

// Build the live bind table from the current selections and install it in pad_input.
static void apply_keybinds() {
  GtavKeybind binds[GTAV_KEYBIND_SLOTS];
  for (uint32_t i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
    const uint32_t ci = g_keybind_combo[i] % kKeybindComboCount;
    binds[i].combo_mask = kKeybindCombos[ci].mask;
    binds[i].action = kKeybindCombos[ci].mask ? kBindableActions[i].action : 0u;
  }
  gtav_pad_input_set_binds(binds, GTAV_KEYBIND_SLOTS);
}

static const char* keybind_value_label(uint32_t slot) {
  if (slot >= GTAV_KEYBIND_SLOTS) return "";
  return kKeybindCombos[g_keybind_combo[slot] % kKeybindComboCount].label;
}

// Cycle a slot's combo. Like the menu-customisation cyclers: param 2 = prev (Left), else next
// (Cross/Right). Reinstalls the bind table so the change takes effect immediately.
static uint32_t cycle_keybind(uint32_t slot, uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0 + slot;
  if (slot >= GTAV_KEYBIND_SLOTS) return unavailable(action, "bad keybind slot");
  if (param == 2u) {
    g_keybind_combo[slot] = (g_keybind_combo[slot] + kKeybindComboCount - 1u) % kKeybindComboCount;
  } else {
    g_keybind_combo[slot] = (g_keybind_combo[slot] + 1u) % kKeybindComboCount;
  }
  apply_keybinds();
  return ok(action, kKeybindCombos[g_keybind_combo[slot]].mask ? "keybind set" : "keybind cleared");
}
