#pragma once

// Menu-definition data for the shell: the ShellItem/ShellMenu types, the menu/row
// enums, and the static const k*Items[] layout tables (one array per menu screen).
// This is "what the menu looks like", split out from native_bridge.cpp's "how it
// behaves" logic. It is a private fragment textually included inside that file's
// anonymous namespace -- it relies on the includes already pulled in there
// (<stdint.h> and gtavmenu/native_bridge.h for the GTAV_NATIVE_SHELL_ACTION_* ids)
// and is not meant to be included anywhere else.

struct ShellItem {
  // label is inline (no relocation; the etaHEN fake-self PRX loader may not apply
  // R_X86_64_RELATIVE -- see feature_catalog.h). unavailable_reason stays a pointer: it
  // is nullptr in every static menu row (no relocation), and the one runtime non-null
  // value is a PC-relative string literal assigned at runtime.
  char label[48];
  uint32_t type;
  uint32_t submenu;
  uint32_t action;
  const char* unavailable_reason;
  uint32_t param;
};

struct ShellMenu {
  const char* label;
  const ShellItem* items;
  uint32_t item_count;
};

enum ShellMenuId {
  SHELL_MENU_MAIN = 0,
  SHELL_MENU_SELF = 1,
  SHELL_MENU_VEHICLES = 2,
  SHELL_MENU_WEAPONS = 3,
  SHELL_MENU_WORLD = 4,
  SHELL_MENU_RUNTIME = 5,
  // Self sub-categories (keeps the Self list short and navigable as it grows).
  SHELL_MENU_SELF_DEFENSE = 6,
  SHELL_MENU_SELF_MOVEMENT = 7,
  SHELL_MENU_SELF_METERS = 8,
  SHELL_MENU_SELF_PRESETS = 9,
  SHELL_MENU_SELF_TELEPORT = 10,
  SHELL_MENU_SELF_TELEPORT_CITY = 11,
  SHELL_MENU_SELF_TELEPORT_COUNTRY = 12,
  SHELL_MENU_SELF_TELEPORT_AIRFIELDS = 13,
  SHELL_MENU_VEHICLE_SPAWNER = 14,
  SHELL_MENU_HUD = 15,
  SHELL_MENU_WEAPON_PICKER = 16,
  SHELL_MENU_VEHICLE_CUSTOMS = 17,
  SHELL_MENU_SKIN_PICKER = 18,
  // Los Santos Customs sub-categories (keeps the LSC list navigable as it grows).
  SHELL_MENU_LSC_PERFORMANCE = 19,
  SHELL_MENU_LSC_PAINT = 20,
  SHELL_MENU_LSC_COSMETICS = 21,
  SHELL_MENU_LSC_BODYWORK = 22,
  SHELL_MENU_LSC_PLATES = 23,
  SHELL_MENU_WEAPON_FX = 24,
  // Menu customisation (theme + draw region) and the keybinds sub-page.
  SHELL_MENU_MENU_SETTINGS = 25,
  SHELL_MENU_KEYBINDS = 26,
  // World content sub-pages: screen effects, ped control, and the ped/object spawners.
  SHELL_MENU_EFFECTS = 27,
  SHELL_MENU_PED_CONTROL = 28,
  SHELL_MENU_PED_SPAWNER = 29,
  SHELL_MENU_OBJECT_SPAWNER = 30,
  // Minigames ("fun modes"), linked under World.
  SHELL_MENU_MINIGAMES = 31,
  SHELL_MENU_SPAWNED_ENTITIES = 32,
  SHELL_MENU_COMPANIONS = 33,
  SHELL_MENU_BODYGUARD_SPAWNER = 34,
  // Vehicle autopilot (self-driving), linked under Vehicles.
  SHELL_MENU_AUTOPILOT = 35,
  // Self > Scenarios: a static parent page (Stop + Browse) plus the dynamic category-filtered
  // scenario picker (one START_SCENARIO row per catalog scenario, like the object browser).
  SHELL_MENU_SELF_SCENARIOS = 36,
  SHELL_MENU_SELF_SCENARIO_PICKER = 37,
  // Vehicles > Controls: doors / windows / headlights / locks / convertible roof.
  SHELL_MENU_VEHICLE_CONTROLS = 38,
  // Self > Wardrobe: clothing/prop component editor + saved outfit.
  SHELL_MENU_WARDROBE = 39,
  // Self > Emotes: the dynamic emote picker (Stop + one PLAY_EMOTE row per catalog anim).
  SHELL_MENU_EMOTES = 40,
  // Quick: a top-level menu of user-pinned rows (global favorites), built at runtime from the
  // (action, param) pins in build_quick_items().
  SHELL_MENU_QUICK = 41,
  // World > Spectacle: world-effect toys (Wind / Camera Shake / Fireworks).
  SHELL_MENU_WORLD_SPECTACLE = 42,
  // Self > Free Camera: enter the detached cinematic camera + tune its fly speed.
  SHELL_MENU_FREE_CAM = 43,
  // Menu Settings > Controls: a read-only reference of the full controller grammar.
  SHELL_MENU_CONTROLS = 44,
  // Self goal sub-pages (keep the Self root short): money one-shots, wanted-level controls, and
  // the appearance editors (skin / wardrobe / scenarios / emotes).
  SHELL_MENU_SELF_MONEY = 45,
  SHELL_MENU_SELF_WANTED = 46,
  SHELL_MENU_SELF_APPEARANCE = 47,
  // World > Environment: the weather / time / gravity cyclers grouped off the World root.
  SHELL_MENU_WORLD_ENVIRONMENT = 48,
  // Menu Settings > Theme Editor: per-channel RGB sliders for the mutable "Custom" theme.
  SHELL_MENU_THEME_EDITOR = 49,
  // Theme Editor > More Colours: the rest of the editable colour groups (footer / scrollbar /
  // selected-label / toggle-on / toggle-off / list / locked), kept off the main editor page so it
  // stays short.
  SHELL_MENU_THEME_EDITOR_MORE = 50,
  // Top-level "Find": a flat, alphabetised index of every actionable row across the whole tree
  // (toggles / actions / cyclers), built at runtime like Quick. Pairs with L3/R3 letter-jump so any
  // feature is a couple of presses away without remembering its path. Excludes the giant dynamic
  // catalogs (vehicle/ped/object/scenario browsers) and the Quick/Find hubs themselves.
  SHELL_MENU_FIND = 51,
  // Weapons > Attachments: per-slot Off/On staging rows + Apply/Remove for the equipped weapon.
  SHELL_MENU_WEAPON_ATTACH = 52,
  SHELL_MENU_COUNT = 53,
  SHELL_MENU_NONE = 0xffffffffu,
};

enum ShellRowType {
  SHELL_ROW_SUBMENU = 0,
  SHELL_ROW_ACTION = 1,
  SHELL_ROW_TOGGLE = 2,
  SHELL_ROW_DISABLED = 3,
  SHELL_ROW_NUMERIC_PLACEHOLDER = 4,
  // Left/Right-adjustable list value (e.g. weather, time). Activating it (Cross)
  // also steps forward; Left/Right cycle prev/next without moving the cursor.
  SHELL_ROW_LIST = 5,
};

static const uint32_t kShellMaxMenuDepth = 4;
static const uint32_t kShellVisibleRows = 10;

static const ShellItem kMainItems[] = {
    {"Quick", SHELL_ROW_SUBMENU, SHELL_MENU_QUICK, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Self", SHELL_ROW_SUBMENU, SHELL_MENU_SELF, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Vehicles", SHELL_ROW_SUBMENU, SHELL_MENU_VEHICLES, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Weapons", SHELL_ROW_SUBMENU, SHELL_MENU_WEAPONS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"World", SHELL_ROW_SUBMENU, SHELL_MENU_WORLD, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"HUD", SHELL_ROW_SUBMENU, SHELL_MENU_HUD, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Runtime", SHELL_ROW_SUBMENU, SHELL_MENU_RUNTIME, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Menu Settings", SHELL_ROW_SUBMENU, SHELL_MENU_MENU_SETTINGS, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Find", SHELL_ROW_SUBMENU, SHELL_MENU_FIND, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
};

// Menu customisation: colour theme + draw side. Submenus first (Theme Editor / Keybinds /
// Controls), then the SHELL_ROW_LIST cyclers handled in menu.c (bridge render state, not native
// features): appearance cyclers (Theme / Draw Side) then the behaviour/timing cyclers.
static const ShellItem kMenuSettingsItems[] = {
    {"Theme Editor", SHELL_ROW_SUBMENU, SHELL_MENU_THEME_EDITOR, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Keybinds", SHELL_ROW_SUBMENU, SHELL_MENU_KEYBINDS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Controls", SHELL_ROW_SUBMENU, SHELL_MENU_CONTROLS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Theme", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_THEME, nullptr, 0},
    {"Draw Side", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_REGION, nullptr,
     0},
    {"Menu Width", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PANEL_WIDTH,
     nullptr, 0},
    {"Worker Hz", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ,
     nullptr, 0},
    {"Toast Time", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME,
     nullptr, 0},
    {"Motion", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION, nullptr, 0},
    {"Nav Delay", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY,
     nullptr, 0},
    {"Nav Speed", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED,
     nullptr, 0},
    {"Touchpad", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD, nullptr,
     0},
};

// Theme Editor: per-channel RGB sliders for the mutable "Custom" theme. The "More Colours"
// sub-page link leads (submenus-first convention), then every slider, then Reset at the bottom.
// Every slider shares one CYCLE_CUSTOM_COLOR action; the row's param selects the channel (0-2
// accent, 3-5 selection, 6-8 panel RGB). Left/Right step the value (auto-repeat to ramp); editing
// selects the Custom theme so the recolour previews live in the running menu. Reset re-seeds the
// default palette.
static const ShellItem kThemeEditorItems[] = {
    {"More Colours", SHELL_ROW_SUBMENU, SHELL_MENU_THEME_EDITOR_MORE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Accent Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 0},
    {"Accent Green", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 1},
    {"Accent Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 2},
    {"Selection Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 3},
    {"Selection Green", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 4},
    {"Selection Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 5},
    {"Panel Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 6},
    {"Panel Green", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 7},
    {"Panel Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 8},
    {"Reset Custom Theme", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_RESET_CUSTOM_THEME, nullptr, 0},
};

// Theme Editor > More Colours: RGB sliders for the remaining seven colour groups, same
// CYCLE_CUSTOM_COLOR action with the channel in param (9-29; see custom_color_channel). Split off
// the main editor page so neither screen is overwhelming. Editing here also selects the Custom
// slot.
static const ShellItem kThemeEditorMoreItems[] = {
    {"Footer Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 9},
    {"Footer Green", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 10},
    {"Footer Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 11},
    {"Scrollbar Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 12},
    {"Scrollbar Green", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 13},
    {"Scrollbar Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 14},
    {"Sel. Label Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 15},
    {"Sel. Label Green", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 16},
    {"Sel. Label Blue", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 17},
    {"Toggle On Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 18},
    {"Toggle On Green", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 19},
    {"Toggle On Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 20},
    {"Toggle Off Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 21},
    {"Toggle Off Green", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 22},
    {"Toggle Off Blue", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR, nullptr, 23},
    {"List Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 24},
    {"List Green", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 25},
    {"List Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 26},
    {"Locked Red", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 27},
    {"Locked Green", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 28},
    {"Locked Blue", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR,
     nullptr, 29},
};

// Keybinds: one cycler per fixed bindable feature; each picks the button combo (incl. "Off")
// that fires that feature while the menu is closed. The labels mirror kBindableActions in
// keybinds.inc; the actions are the contiguous CYCLE_KEYBIND_0..3 block (slot order).
static const ShellItem kKeybindsItems[] = {
    {"God Mode Key", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0,
     nullptr, 0},
    {"Noclip Key", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_1,
     nullptr, 0},
    {"Super Jump Key", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_2,
     nullptr, 0},
    {"Invisible Key", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_3,
     nullptr, 0},
};

// Controls: a read-only reference of the full controller grammar (the per-row hint bar shows the
// focused row's verbs; this page documents the whole vocabulary in one place). Every row is a
// non-activating SHELL_ROW_DISABLED info line; the binding text lives in the label. Keep the
// strings in sync with gtav_pad_input_map (pad_input.c) if a chord ever changes.
static const ShellItem kControlsItems[] = {
    {"R1 + DPad Left: Open / Close", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"DPad Up / Down: Navigate", SHELL_ROW_DISABLED, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Cross: Select / Enter", SHELL_ROW_DISABLED, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Circle: Back  (Hold: To Root)", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"DPad Left / Right: Adjust Value", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"R3: Pin to Quick / Favorite", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"L1 / R1 / L3: free (gameplay)", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Triangle / Square: free (gameplay)", SHELL_ROW_DISABLED, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
};

// Self is split into goal sub-pages so the root stays short and navigable. Layout convention
// (applied across the whole tree): submenu rows are grouped at the TOP, then the leaf
// action/toggle rows, each block kept in a similar-items order. Submenus run capabilities
// (Presets / Invincibility / Movement / Meters) -> resources (Money / Wanted) -> identity
// (Appearance) -> mobility (Teleport / Free Camera); the leaf rows run upkeep (Heal / Auto Heal /
// Clean) -> stealth (Invisibility) -> self-destruct (Kill / Ragdoll).
static const ShellItem kSelfItems[] = {
    {"Presets", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_PRESETS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Invincibility", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_DEFENSE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Movement", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_MOVEMENT, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Meters", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_METERS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Money", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_MONEY, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Wanted", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_WANTED, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Appearance", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_APPEARANCE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Teleport", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_TELEPORT, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Free Camera", SHELL_ROW_SUBMENU, SHELL_MENU_FREE_CAM, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Heal + Armor", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_HEAL_ARMOR,
     nullptr, 0},
    {"Auto Heal", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_HEAL,
     nullptr, 0},
    {"Clean Self", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CLEAN_SELF, nullptr,
     0},
    {"Invisibility", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_INVISIBLE,
     nullptr, 0},
    {"Kill Self", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_KILL_SELF, nullptr,
     0},
    {"Force Ragdoll", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL,
     nullptr, 0},
};

// Self > Appearance: the look/identity editors, grouped off the Self root.
static const ShellItem kSelfAppearanceItems[] = {
    {"Skin Changer", SHELL_ROW_SUBMENU, SHELL_MENU_SKIN_PICKER, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Wardrobe", SHELL_ROW_SUBMENU, SHELL_MENU_WARDROBE, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Scenarios", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_SCENARIOS, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Emotes", SHELL_ROW_SUBMENU, SHELL_MENU_EMOTES, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
};

// Self > Money: the cash one-shots + the Set Cash cycler.
static const ShellItem kSelfMoneyItems[] = {
    {"Give $10,000", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY,
     nullptr, 10000},
    {"Give $100,000", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY,
     nullptr, 100000},
    {"Give $1,000,000", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY,
     nullptr, 1000000},
    {"Set Cash", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CASH, nullptr, 0},
};

// Self > Wanted: wanted-level controls.
static const ShellItem kSelfWantedItems[] = {
    {"Clear Wanted", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CLEAR_WANTED,
     nullptr, 0},
    {"Set Wanted Level", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WANTED,
     nullptr, 0},
    {"Never Wanted", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_NEVER_WANTED, nullptr, 0},
    {"Wanted Lock", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_WANTED_LOCK,
     nullptr, 0},
};

// Self > Scenarios parent page: cancel the current scenario, or open the category-filtered
// browser to pick one. The browser (SHELL_MENU_SELF_SCENARIO_PICKER) is built at runtime from
// the scenario catalog (rebuild_scenario_picker), like the object spawner. Both entering and
// cancelling a scenario mutate the ped task tree, so they are game-thread gated (locked until the
// frame hook is live, then they drain on the game thread).
static const ShellItem kSelfScenariosItems[] = {
    {"Browse Scenarios", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_SCENARIO_PICKER,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Stop Scenario", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO,
     "Needs main-thread hook", 0},
};

static const ShellItem kSelfPresetItems[] = {
    {"Stealth Mode", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PRESET_STEALTH,
     nullptr, 0},
    {"Stunt Mode", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PRESET_STUNT,
     nullptr, 0},
    {"Survival Mode", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PRESET_SURVIVAL,
     nullptr, 0},
    {"Driver Assist", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER,
     nullptr, 0},
    {"Parkour Mode", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PRESET_PARKOUR,
     nullptr, 0},
};

// Teleport submenu: grouped single-player landmark sub-pages first, then the waypoint/saved
// position one-shots. The preset rows carry their kTeleportPresets[] index in the param field.
static const ShellItem kSelfTeleportItems[] = {
    {"City Landmarks", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_TELEPORT_CITY,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"North Landmarks", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_TELEPORT_COUNTRY,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Airfields", SHELL_ROW_SUBMENU, SHELL_MENU_SELF_TELEPORT_AIRFIELDS,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Save Position", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_SAVE_LOCATION,
     nullptr, 0},
    {"Return to Saved", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION, nullptr, 0},
    {"To Waypoint", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT,
     nullptr, 0},
    {"To Objective", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE,
     nullptr, 0},
    {"Into Last Vehicle", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TELEPORT_LAST_VEHICLE, "Needs main-thread hook", 0},
};

// .param indexes into kTeleportPresets[] in features.cpp -- keep them in sync.
static const ShellItem kSelfTeleportCityItems[] = {
    {"Maze Bank Roof", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 3},
    {"Maze Bank Arena", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 5},
    {"Diamond Casino", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 6},
    {"Vinewood Sign", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 7},
    {"Del Perro Pier", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 8},
    {"Legion Square", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 9},
    {"Vespucci Beach", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 10},
    {"Mirror Park", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 11},
};

static const ShellItem kSelfTeleportCountryItems[] = {
    {"Mount Chiliad", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 1},
    {"Fort Zancudo", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 2},
    {"Paleto Bay", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 12},
    {"Sandy Shores", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 13},
    {"Mount Gordo", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 14},
    {"Observatory", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 15},
    {"Alamo Sea Marina", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET, nullptr, 16},
};

static const ShellItem kSelfTeleportAirfieldItems[] = {
    {"LS Airport", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 0},
    {"Sandy Shores Airfield", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET, nullptr, 4},
    {"McKenzie Airfield", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET, nullptr, 17},
    {"LSIA Helipad", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET,
     nullptr, 18},
};

static const ShellItem kSelfDefenseItems[] = {
    {"God Mode", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE,
     nullptr, 0},
    {"Super Proofs", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_PROOFS, nullptr, 0},
    {"No Ragdoll", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RAGDOLL,
     nullptr, 0},
    {"No Critical Hits", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_CRIT,
     nullptr, 0},
    {"Seatbelt", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEATBELT,
     nullptr, 0},
    {"Ignored By Everyone", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_IGNORED_BY_ALL, nullptr, 0},
};

static const ShellItem kSelfMovementItems[] = {
    {"Super Run", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_RUN,
     nullptr, 0},
    {"Fast Movement", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_MOVE,
     nullptr, 0},
    {"Fast Swim", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_SWIM,
     nullptr, 0},
    {"Super Jump", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_JUMP,
     nullptr, 0},
    {"Move Speed", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_MOVE_RATE,
     nullptr, 0},
    {"Noclip", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_NOCLIP, nullptr,
     0},
    {"Fly", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE, nullptr, 0},
    {"Infinite Parachutes", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_PARACHUTES, nullptr, 0},
};

static const ShellItem kSelfMetersItems[] = {
    {"Infinite Stamina", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_STAMINA, nullptr, 0},
    {"Infinite Special", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_SPECIAL, nullptr, 0},
    {"Max All Stats", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MAX_ALL_STATS,
     nullptr, 0},
};

// Weapons root: submenus first (Weapon Browser / Combat Effects), then the leaf rows grouped --
// loadout one-shots (give/remove), ammo toggles, then the customisation cyclers (tint / toolkit).
static const ShellItem kWeaponItems[] = {
    {"Weapon Browser", SHELL_ROW_SUBMENU, SHELL_MENU_WEAPON_PICKER, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Combat Effects", SHELL_ROW_SUBMENU, SHELL_MENU_WEAPON_FX, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Attachments", SHELL_ROW_SUBMENU, SHELL_MENU_WEAPON_ATTACH, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Give Weapon Pack", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS,
     nullptr, 0},
    {"Give Max Ammo", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO,
     "Needs main-thread hook", 0},
    {"Remove All Weapons", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS, nullptr, 0},
    {"Infinite Ammo", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_AMMO, nullptr, 0},
    {"No Reload", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RELOAD,
     nullptr, 0},
    {"Weapon Tint", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_TINT,
     nullptr, 0},
    {"Gun Toolkit", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN,
     nullptr, 0},
};

// Weapons > Attachments: stage each slot Off/On for the equipped weapon (worker-side cyclers), then
// Apply (game-thread, gives/removes the weapon's canonical component per slot). Remove strips them.
static const ShellItem kWeaponAttachItems[] = {
    {"Suppressor", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SUPP,
     nullptr, 0},
    {"Scope", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SCOPE, nullptr,
     0},
    {"Grip", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_GRIP, nullptr,
     0},
    {"Extended Clip", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_CLIP,
     nullptr, 0},
    {"Flashlight", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_FLASH,
     nullptr, 0},
    {"Apply Attachments", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS, "Needs main-thread hook", 0},
    {"Remove All Attachments", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS, "Needs main-thread hook", 0},
};

// Weapon effects: explosive / fire ammo. Continuous toggles whose entity-creating natives
// run on the game thread via the frame-hook tick (so they require the live hook).
static const ShellItem kWeaponFxItems[] = {
    {"Explosive Ammo", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_AMMO, nullptr, 0},
    {"Fire Ammo", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_FIRE_AMMO,
     nullptr, 0},
    {"Explosive Melee", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_MELEE, nullptr, 0},
    {"Weapon Damage", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_DAMAGE,
     nullptr, 0},
    {"Explosion at Player", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER, nullptr, 0},
    {"Explosion at Waypoint", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT, nullptr, 0},
};

// Los Santos Customs: worker-safe mod/respray on the current vehicle, grouped into
// Performance / Paint & Color / Cosmetics sub-categories. Paint, tint, neon and tyre
// smoke are list cyclers (Left/Right stage a value, Cross applies); the rest are
// one-shots. "Reset to Stock" lives at the root and reverts every category.
static const ShellItem kVehicleCustomsItems[] = {
    {"Performance", SHELL_ROW_SUBMENU, SHELL_MENU_LSC_PERFORMANCE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Paint & Color", SHELL_ROW_SUBMENU, SHELL_MENU_LSC_PAINT, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Bodywork", SHELL_ROW_SUBMENU, SHELL_MENU_LSC_BODYWORK, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Cosmetics", SHELL_ROW_SUBMENU, SHELL_MENU_LSC_COSMETICS, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Plates & Livery", SHELL_ROW_SUBMENU, SHELL_MENU_LSC_PLATES, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Reset to Stock", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_LS_STOCK_VEHICLE,
     nullptr, 0},
};

// Performance: the one-shot "max everything" plus per-slot tier pickers. Each picker is a
// list cycler (Left/Right stage a tier bounded live by GET_NUM_VEHICLE_MODS, Cross applies).
static const ShellItem kLscPerformanceItems[] = {
    {"Max Performance", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_LS_MAX_PERFORMANCE, nullptr, 0},
    {"Engine", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_ENGINE, nullptr, 0},
    {"Brakes", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_BRAKES, nullptr, 0},
    {"Transmission", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_TRANSMISSION,
     nullptr, 0},
    {"Suspension", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_SUSPENSION,
     nullptr, 0},
    {"Armor", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_ARMOR, nullptr, 0},
};

// Bodywork: per-slot visual mod tier pickers (same list-cycler mechanism as Performance).
static const ShellItem kLscBodyworkItems[] = {
    {"Spoiler", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_SPOILER, nullptr, 0},
    {"Front Bumper", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_FRONT_BUMPER,
     nullptr, 0},
    {"Rear Bumper", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_REAR_BUMPER,
     nullptr, 0},
    {"Side Skirt", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_SIDE_SKIRT,
     nullptr, 0},
    {"Exhaust", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_EXHAUST, nullptr, 0},
    {"Frame", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_FRAME, nullptr, 0},
    {"Grille", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_GRILLE, nullptr, 0},
    {"Hood", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_HOOD, nullptr, 0},
    {"Fenders", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_FENDERS, nullptr, 0},
    {"Roof", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_ROOF, nullptr, 0},
};

static const ShellItem kLscPaintItems[] = {
    {"Primary Paint", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_PRIMARY,
     nullptr, 0},
    {"Secondary Paint", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_SECONDARY, nullptr, 0},
    {"Window Tint", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WINDOW_TINT,
     nullptr, 0},
    {"Neon Underglow", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_NEON,
     nullptr, 0},
    {"Tyre Smoke", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TYRE_SMOKE,
     nullptr, 0},
    {"Pearlescent", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PEARLESCENT,
     nullptr, 0},
    {"Wheel Color", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_COLOR,
     nullptr, 0},
    {"Custom Primary", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_PRIMARY, nullptr, 0},
    {"Custom Secondary", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_SECONDARY, nullptr, 0},
    {"Rainbow Neon", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_NEON, nullptr, 0},
    {"Rainbow Paint", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_PAINT, nullptr, 0},
};

static const ShellItem kLscCosmeticsItems[] = {
    {"Install Cosmetics", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_LS_INSTALL_COSMETICS, nullptr, 0},
    {"Wheel Type", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_TYPE,
     nullptr, 0},
    {"Wheels", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_WHEELS, nullptr, 0},
    {"Horn", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_MOD_HORN, nullptr, 0},
    {"Xenon Headlights", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_XENON,
     nullptr, 0},
    {"Bulletproof Tyres", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_LS_BULLETPROOF_TYRES, nullptr, 0},
    {"Burst Tyres", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_BURST_TYRES,
     nullptr, 0},
    {"Fix Tyres", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_FIX_TYRES, nullptr,
     0},
};

// Plates & Livery: number-plate style/text and vehicle liveries (live-count cycler).
static const ShellItem kLscPlatesItems[] = {
    {"Livery", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_LIVERY, nullptr, 0},
    {"Plate Style", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_STYLE,
     nullptr, 0},
    {"Plate Text", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_TEXT,
     nullptr, 0},
};

// World > Environment: the weather / time-of-day / gravity / time-scale cyclers, grouped off the
// World root so the root holds the spawn/ped/minigame links plus the population toggle.
static const ShellItem kWorldEnvironmentItems[] = {
    {"Weather", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WEATHER, nullptr,
     0},
    {"Time of Day", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME, nullptr,
     0},
    {"Set Hour", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CLOCK_HOUR,
     nullptr, 0},
    {"Freeze Time", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_FREEZE_TIME,
     nullptr, 0},
    {"Low Gravity", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_LOW_GRAVITY,
     nullptr, 0},
    {"Gravity Level", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_GRAVITY,
     nullptr, 0},
    {"Slow Motion", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLOW_MOTION,
     nullptr, 0},
    {"Time Scale", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME_SCALE,
     nullptr, 0},
};

// World root: every sub-page first (ambiance: Environment / Effects / Spectacle; spawning:
// Spawn Ped / Spawn Object / Spawned Entities; peds: Companions / Ped Control; fun: Minigames),
// then the leaf toggle (Thin Population) and the experimental one-shot last.
static const ShellItem kWorldItems[] = {
    {"Environment", SHELL_ROW_SUBMENU, SHELL_MENU_WORLD_ENVIRONMENT, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Effects", SHELL_ROW_SUBMENU, SHELL_MENU_EFFECTS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Spectacle", SHELL_ROW_SUBMENU, SHELL_MENU_WORLD_SPECTACLE, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Spawn Ped", SHELL_ROW_SUBMENU, SHELL_MENU_PED_SPAWNER, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Spawn Object", SHELL_ROW_SUBMENU, SHELL_MENU_OBJECT_SPAWNER, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Spawned Entities", SHELL_ROW_SUBMENU, SHELL_MENU_SPAWNED_ENTITIES,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Companions", SHELL_ROW_SUBMENU, SHELL_MENU_COMPANIONS, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Ped Control", SHELL_ROW_SUBMENU, SHELL_MENU_PED_CONTROL, GTAV_NATIVE_SHELL_ACTION_NONE,
     nullptr, 0},
    {"Minigames", SHELL_ROW_SUBMENU, SHELL_MENU_MINIGAMES, GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,
     0},
    {"Thin Population", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_THIN_POPULATION, nullptr, 0},
#if GTAV_MENU_ENABLE_PROLOGUE_SKIP
    // Experimental: write the prologue1 mission-complete script global, terminate the running
    // prologue1, and warp to Los Santos. Game-thread gated, so the row stays locked ("needs
    // main-thread hook") until the frame hook is live. Only compiled in the
    // GTAV_MENU_ENABLE_PROLOGUE_SKIP build; the prologue global index is candidate-only.
    {"Skip Prologue (Experimental)", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_SKIP_PROLOGUE, "needs main-thread hook", 0},
#endif
};

// World > Spectacle: world-effect toys. Wind is a worker-safe global setter (like Weather); Camera
// Shake is a game-thread re-asserted mode; Fireworks is a game-thread PTFX streaming job (locked
// until the frame hook is live, like the spawn rows).
static const ShellItem kWorldSpectacleItems[] = {
    {"Wind", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND, nullptr, 0},
    {"Camera Shake", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE,
     nullptr, 0},
    {"Fireworks", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS,
     "Needs main-thread hook", 0},
};

// Self > Free Camera: enter the detached cinematic camera (closes the menu; Circle exits) and tune
// its fly speed. "Enter Free Camera" self-refuses until the frame hook is live (the scripted-cam
// natives run on that tick), so it carries no static lock.
static const ShellItem kFreeCamItems[] = {
    {"Enter Free Camera", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_FREE_CAM,
     nullptr, 0},
    {"Speed", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED,
     nullptr, 0},
};

static const ShellItem kSpawnedEntitiesItems[] = {
    {"Clear Spawned Vehicles", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES, "Needs main-thread hook", 0},
    {"Clear Spawned Peds", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS, "Needs main-thread hook", 0},
    {"Clear Spawned Objects", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS, "Needs main-thread hook", 0},
    {"Clear All Spawned", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL, "Needs main-thread hook", 0},
    // Spooner-lite: Save/Load parse and publish worker-side; streaming + CREATE_* run through the
    // game-thread chain. Cancel atomically stops that chain before its next entry.
    {"Save Map", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_SAVE_MAP, nullptr, 0},
    {"Load Map", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_LOAD_MAP, nullptr, 0},
    {"Cancel Map Load", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CANCEL_MAP_LOAD,
     nullptr, 0},
    {"Check Custom Assets", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_PROBE_CUSTOM_MOUNT, nullptr, 0},
    // Last-object tweaks. Duplicate/Attach/Detach touch entity managers (gated until the hook is
    // live); Object Alpha is a worker-safe transparency cycler.
    {"Duplicate Last Object", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_DUPLICATE_LAST_ENTITY, "Needs main-thread hook", 0},
    {"Attach Last to Player", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_ATTACH_LAST_ENTITY, "Needs main-thread hook", 0},
    {"Detach Last Object", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_DETACH_LAST_ENTITY, "Needs main-thread hook", 0},
    {"Object Alpha", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA,
     nullptr, 0},
};

// Companions: the Spawn Bodyguard browser first, then the per-guard settings grouped --
// durability (Health / Armor / Invincible), combat (Weapon / Accuracy), behaviour (Formation /
// Aggression), display (Map Blips) -- and the Bring/Dismiss command one-shots last.
static const ShellItem kCompanionItems[] = {
    {"Spawn Bodyguard", SHELL_ROW_SUBMENU, SHELL_MENU_BODYGUARD_SPAWNER,
     GTAV_NATIVE_SHELL_ACTION_NONE, nullptr, 0},
    {"Bodyguard Health", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_HEALTH, nullptr, 0},
    {"Bodyguard Armor", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ARMOR, nullptr, 0},
    {"Invincible Guards", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_INVINCIBLE, nullptr, 0},
    {"Weapon", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_WEAPON,
     nullptr, 0},
    {"Accuracy", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ACCURACY,
     nullptr, 0},
    {"Formation", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_FORMATION, nullptr, 0},
    {"Aggression", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_AGGRESSION, nullptr, 0},
    {"Map Blips", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_BLIPS, nullptr, 0},
    {"Bring Bodyguards", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS, "Needs main-thread hook", 0},
    {"Dismiss Bodyguards", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS, "Needs main-thread hook", 0},
};

// Vehicle autopilot (self-driving). Cyclers + Stop only set worker-side state; the drive
// natives run on the game-thread frame-hook tick, so the Mode cycler self-refuses with a
// toast until the hook is live (no static "Needs main-thread hook" lock needed here).
static const ShellItem kAutopilotItems[] = {
    {"Mode", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE,
     nullptr, 0},
    {"Aggression", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION, nullptr, 0},
    {"Drive Speed", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED,
     nullptr, 0},
    // Cruise Control holds the player's own driving at a fixed speed (distinct from full
    // autopilot's wander/waypoint Mode above); its speed picker rides alongside it here.
    {"Cruise Speed", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_CRUISE_SPEED,
     nullptr, 0},
    {"Cruise Control", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_CRUISE_CONTROL, nullptr, 0},
    {"Stop Autopilot", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_STOP_AUTOPILOT,
     nullptr, 0},
};

// Vehicles > Controls: doors / windows / headlights / locks / convertible roof. Every row is a
// worker-direct one-shot or cycler on the player's current vehicle (no game-thread gate). "Door"
// is a selection cycler (picks which door Open/Close acts on); Headlights/Door Locks are
// stage/apply cyclers (Left/Right pick, Cross applies).
static const ShellItem kVehicleControlsItems[] = {
    {"Door", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_DOOR, nullptr,
     0},
    {"Open Door", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_OPEN_VEHICLE_DOOR,
     nullptr, 0},
    {"Close Door", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CLOSE_VEHICLE_DOOR,
     nullptr, 0},
    {"Roll Windows Down", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_ROLL_DOWN_WINDOWS, nullptr, 0},
    {"Roll Windows Up", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_ROLL_UP_WINDOWS,
     nullptr, 0},
    {"Headlights", SHELL_ROW_LIST, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_HEADLIGHTS, nullptr, 0},
    {"Door Locks", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_LOCK,
     nullptr, 0},
    {"Raise Roof", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_RAISE_VEHICLE_ROOF,
     nullptr, 0},
    {"Lower Roof", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_LOWER_VEHICLE_ROOF,
     nullptr, 0},
};

// Self > Wardrobe: a compact clothing editor. "Slot" picks the component/prop; "Style" and
// "Texture" step+apply the selected slot live; Reset/Clear Prop/Save Outfit/Apply Outfit are
// actions. All worker-direct on the player ped (no game-thread gate). One saved outfit (profile).
static const ShellItem kWardrobeItems[] = {
    {"Slot", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_SLOT, nullptr,
     0},
    {"Style", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_STYLE,
     nullptr, 0},
    {"Texture", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_TEXTURE,
     nullptr, 0},
    {"Clear Prop", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_WARDROBE_CLEAR_PROP,
     nullptr, 0},
    {"Reset to Default", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_WARDROBE_RESET,
     nullptr, 0},
    {"Outfit Slot", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_OUTFIT_SLOT,
     nullptr, 0},
    {"Save Outfit", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT,
     nullptr, 0},
    {"Apply Outfit", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_APPLY_OUTFIT,
     nullptr, 0},
};

// Minigames ("fun modes"), reached from World > Minigames. Each is a mutually-exclusive
// TOGGLE: enabling one turns off any other (single active mode). They run their per-frame work
// on the game thread via the frame hook, so each refuses with a toast until the hook is live
// (same as Noclip). Blackout, Zombie Outbreak and Flood were the former "Phase B" set; their
// natives are now resolved and pinned, so they are live toggles like the rest (see
// docs/MINIGAMES.md).
static const ShellItem kMinigamesItems[] = {
    {"Riot Mode", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RIOT,
     nullptr, 0},
    {"Meteor Shower", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_METEOR, nullptr, 0},
    {"Storm", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_STORM,
     nullptr, 0},
    {"Inferno", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_INFERNO,
     nullptr, 0},
    {"Ragdoll Party", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RAGDOLL, nullptr, 0},
    {"City Blackout", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_BLACKOUT, nullptr, 0},
    {"Zombie Outbreak", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_ZOMBIES, nullptr, 0},
    {"Flood (experimental)", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_FLOOD, nullptr, 0},
};

// Screen effects submenu (World > Effects). Night vision / thermal are toggles; the two
// cyclers stage worker-side and apply on the game thread.
static const ShellItem kEffectsItems[] = {
    {"Night Vision", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_NIGHT_VISION, nullptr, 0},
    {"Thermal Vision", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEETHROUGH, nullptr, 0},
    {"Screen Effect", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE,
     nullptr, 0},
    {"Trippy FX", SHELL_ROW_LIST, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX,
     nullptr, 0},
};

// Ped control submenu (World > Ped Control). All game-thread one-shot actions (LOCK until the
// frame hook is live).
static const ShellItem kPedControlItems[] = {
    {"Everyone Attacks Me", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_PEDS_ATTACK_PLAYER, "Needs main-thread hook", 0},
    {"Everyone Flees", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PEDS_FLEE_PLAYER,
     "Needs main-thread hook", 0},
    {"Calm Peds", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_PEDS_STOP,
     "Needs main-thread hook", 0},
    {"Ragdoll Nearby", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_RAGDOLL_NEARBY,
     "Needs main-thread hook", 0},
};

// HUD overlay submenu: master switch + per-element toggles. The overlay is drawn by
// the worker render path (render_hud_overlay) regardless of whether the panel is open.
static const ShellItem kHudItems[] = {
    {"HUD Overlay", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD, nullptr,
     0},
    {"Speedometer", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_SPEEDO,
     nullptr, 0},
    {"Coordinates", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_COORDS,
     nullptr, 0},
    {"FPS Readout", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_FPS,
     nullptr, 0},
    {"Waypoint Dist", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_DISTANCE, nullptr, 0},
};

static const ShellItem kRuntimeItems[] = {
    {"Telemetry", SHELL_ROW_TOGGLE, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY,
     nullptr, 0},
    {"Debug Overlay", SHELL_ROW_TOGGLE, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_TOGGLE_DEBUG_OVERLAY, nullptr, 0},
    {"Disable All Features", SHELL_ROW_ACTION, SHELL_MENU_NONE,
     GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL, nullptr, 0},
    {"Hide Shell", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_HIDE, nullptr, 0},
    {"Stop Runtime", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_STOP, nullptr, 0},
    {"Save Profile", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_SAVE_PROFILE,
     nullptr, 0},
    {"Reload Profile", SHELL_ROW_ACTION, SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_LOAD_PROFILE,
     nullptr, 0},
};
