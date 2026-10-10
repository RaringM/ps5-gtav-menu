// Weapons > Attachments "Component" (roadmap F4.3): fit or remove any component the equipped
// weapon's meta lists, and the pack-weapon notes of Weapon Tint, Give Max Ammo and the give.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace AFTER
// custom_rpf.inc (the pack lane's pack_weapon_* helpers, custom_pack_data.inc) and
// weapon_attachments.inc (wattach_current_weapon, wattach_row_for, kWAttach). Not a standalone TU.
//
// The list belongs to the weapon in hand (GET_CURRENT_PED_WEAPON, worker-safe like Weapon Tint):
// a pack weapon offers every component its own weapons.meta lists under <AttachPoints> (the pack's
// own components, e.g. gtavmenu-wcomp-v1's, and the donor's retail ones it kept), named by the
// pack's `spawn component` row or the part's kind; a retail weapon offers its Attachments-table
// components (no retail weapons.meta is staged). A CHOICE row: Left/Right pick one (worker index
// math only, so the bridge's revert replay is exact), Cross queues the fit/remove onto the game
// thread (HAS_PED_GOT_WEAPON_COMPONENT -> GIVE_ / REMOVE_WEAPON_COMPONENT_TO/FROM_PED walk the ped
// weapon manager, like Apply Attachments). DOES_WEAPON_TAKE_WEAPON_COMPONENT is not in the reviewed
// native table, so the meta (or the table) is the list.

#if GTAV_MENU_ENABLE_CUSTOM_PACKS && (defined(__PROSPERO__) || defined(__ORBIS__))
#define GTAV_WCOMP_PACK_LANE 1
#else
// No pack lane: no pack weapons.
static int pack_weapon_spawn(uint32_t) { return -1; }
[[maybe_unused]] static int pack_weapon_wheel_slot(uint32_t) { return -1; }
static int pack_weapon_components(uint32_t, uint32_t*, char*, size_t, int, uint32_t*) {
  return -1;
}
static int pack_weapon_hashes(uint32_t*, int) { return 0; }
static int pack_weapon_tint_none(int) { return 0; }
#endif

// A pack-notes line (only with the pack lane: the weapons it names are pack weapons).
[[gnu::noinline]] static void wcomp_note(const char* format, ...) {
#ifdef GTAV_WCOMP_PACK_LANE
  char line[128];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  gtav_worker_klog(line);
#else
  (void)format;
#endif
}

#define GTAV_WCOMP_MAX 16
#define GTAV_WCOMP_AMMO_MAX 32  // pack weapons Give Max Ammo tops up
#define GTAV_WCOMP_LABEL 18  // 17 characters + " (off)" fits the 24-byte list value (a default
                             // part's " (default)" cuts the name instead)

static const char kWAttachSlotNames[GTAV_WATTACH_SLOT_COUNT][GTAV_WCOMP_LABEL] = {
    "Suppressor", "Scope", "Grip", "Extended Clip", "Flashlight",
};

// The equipped weapon's components (worker thread). `on`/`known`: what the game thread last saw
// (bit per entry), folded in from the g_wcomp_done_* mailbox. `defaults`: the pack weapon's default
// parts (its meta's <Default value="true" />), which the engine keeps fitted.
static struct {
  uint32_t weapon;
  int count;
  uint32_t hashes[GTAV_WCOMP_MAX];
  char labels[GTAV_WCOMP_MAX][GTAV_WCOMP_LABEL];
  uint32_t on, known, defaults;
} g_wcomp;
static uint32_t g_wcomp_pick;
static uint32_t g_wcomp_seen;
// Game thread -> worker: the last fit/remove (weapon, component, has), published by the seq bump.
static uint32_t g_wcomp_done_seq, g_wcomp_done_weapon, g_wcomp_done_component, g_wcomp_done_has;

static void wcomp_refresh() {
  const uint32_t weapon = wattach_current_weapon(nullptr);
  if (weapon != g_wcomp.weapon) {
    memset(&g_wcomp, 0, sizeof(g_wcomp));
    g_wcomp.weapon = weapon;
    g_wcomp_pick = 0;
    const int n = weapon ? pack_weapon_components(weapon, g_wcomp.hashes, &g_wcomp.labels[0][0],
                                                  GTAV_WCOMP_LABEL, GTAV_WCOMP_MAX,
                                                  &g_wcomp.defaults)
                         : 0;
    if (n >= 0) {
      g_wcomp.count = n;
    } else if (const struct WAttachRow* row = wattach_row_for(weapon)) {
      for (int s = 0; s < GTAV_WATTACH_SLOT_COUNT; ++s) {
        if (!row->comp[s]) continue;
        g_wcomp.hashes[g_wcomp.count] = row->comp[s];
        memcpy(g_wcomp.labels[g_wcomp.count++], kWAttachSlotNames[s], GTAV_WCOMP_LABEL);
      }
    }
  }
  const uint32_t seq = __atomic_load_n(&g_wcomp_done_seq, __ATOMIC_ACQUIRE);
  if (seq != g_wcomp_seen) {
    g_wcomp_seen = seq;
    for (int i = 0; i < g_wcomp.count; ++i) {
      if (g_wcomp_done_weapon != g_wcomp.weapon || g_wcomp.hashes[i] != g_wcomp_done_component)
        continue;
      // A fit can replace another part on the same attach point (a clip swaps the clip): the
      // other entries' last seen state no longer holds.
      if (g_wcomp_done_has) g_wcomp.known = 0u;
      g_wcomp.known |= 1u << i;
      if (g_wcomp_done_has) g_wcomp.on |= 1u << i;
      else g_wcomp.on &= ~(1u << i);
    }
  }
}

[[gnu::noinline]] static const char* weapon_component_value_label() {
  static char value[24];
  wcomp_refresh();
  if (!g_wcomp.count) return "None";
  const uint32_t i = g_wcomp_pick % (uint32_t)g_wcomp.count;
  const uint32_t bit = 1u << i;
  const char* state = (g_wcomp.defaults & bit) ? " (default)"
                      : !(g_wcomp.known & bit)  ? ""
                      : (g_wcomp.on & bit)      ? " (on)"
                                                : " (off)";
  // The state suffix always shows; a long name is cut to make room for it.
  snprintf(value, sizeof(value), "%.*s%s", (int)(sizeof(value) - 1u - strlen(state)),
           g_wcomp.labels[i], state);
  return value;
}

// Worker: Left/Right (param 1/2) pick, Cross (0) queues the fit/remove of the picked component.
static uint32_t cycle_weapon_component(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_COMPONENT;
  wcomp_refresh();
  if (param == 1u || param == 2u) {
    if (g_wcomp.count) stage_step(&g_wcomp_pick, (uint32_t)g_wcomp.count, param);
    return ok(action, weapon_component_value_label());
  }
  if (!g_wcomp.weapon) return failed(action, "no weapon equipped");
  if (!g_wcomp.count) return unavailable(action, "no components for this weapon");
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (gtav_frame_hook_is_active()) {
    const uint32_t component = g_wcomp.hashes[g_wcomp_pick % (uint32_t)g_wcomp.count];
    if (gtav_frame_hook_enqueue(action, component)) return ok(action, "queued for main thread");
    return failed(action, "main-thread job queue full");
  }
#endif
  return unavailable(action, "needs main-thread hook");
}

// Game thread (self-enqueued by cycle_weapon_component; param = the component's joaat): fit the
// component to the weapon in hand, or remove it when fitted. The engine fits only a component the
// weapon's AttachPoints list, so has == had after a fit means the weapon refused it; has == had
// after a remove is a default part (the standard clip): the engine keeps it until another part of
// its attach point replaces it, which is not a failure (on hardware: PISTOL_CLIP_01 had=1 has=1).
static uint32_t apply_weapon_component(uint32_t component) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_COMPONENT;
  if (!g_n.give_weapon_component_to_ped || !g_n.remove_weapon_component_from_ped ||
      !g_n.has_ped_got_weapon_component || !g_n.get_current_ped_weapon || !g_n.player_ped_id)
    return unavailable(action, "missing weapon component natives");
  int ped = 0;
  const uint32_t weapon = wattach_current_weapon(&ped);
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  if (!weapon) return failed(action, "no weapon equipped");
  int had = 0;
  const int has = weapon_component_flip(ped, weapon, component, &had);
  g_wcomp_done_weapon = weapon;
  g_wcomp_done_component = component;
  g_wcomp_done_has = (uint32_t)has;
  __atomic_add_fetch(&g_wcomp_done_seq, 1u, __ATOMIC_RELEASE);
  if (pack_weapon_spawn(weapon) >= 0)
    wcomp_note("GTAVMenu pack weapon component 0x%08x weapon=0x%08x had=%d has=%d",
               (unsigned)component, (unsigned)weapon, had, has);
  if (has == had && had) return ok(action, "default part: fit another one instead");
  if (has == had) return failed(action, "weapon refused the component");
  return ok(action, has ? "component on" : "component off");
}

// Weapon Tint on a pack weapon, before the stock setter: a pack-notes line. Returns 1 (skip the
// setter) when the weapon's pack says `tints <weapon> none`: its model reads no tint palette.
static int weapon_tint_note(uint32_t weapon, uint32_t index) {
  const int spawn = pack_weapon_spawn(weapon);
  if (spawn < 0) return 0;
  if (pack_weapon_tint_none(spawn)) {
    wcomp_note("GTAVMenu pack weapon tint 0x%08x index=%u skipped: no tint palette",
               (unsigned)weapon, (unsigned)index);
    return 1;
  }
  wcomp_note("GTAVMenu pack weapon tint 0x%08x index=%u", (unsigned)weapon, (unsigned)index);
  return 0;
}

// Give Max Ammo, game thread: top up the weapon in hand and every pack weapon the ped carries
// (the catalog loop covers the retail ones). GET_MAX_AMMO is 0 for a weapon the ped lacks.
static void give_max_ammo_extra(int ped) {
  uint32_t weapons[1 + GTAV_WCOMP_AMMO_MAX];
  int n = 0;
  uint32_t current = 0;
  if (g_n.get_current_ped_weapon &&
      invoke_return<int>(g_n.get_current_ped_weapon, ped, &current, 1) && current)
    weapons[n++] = current;
  n += pack_weapon_hashes(weapons + n, GTAV_WCOMP_AMMO_MAX);
  for (int i = 0; i < n; ++i) {
    if (i && weapons[i] == current) continue;
    const int max_ammo = top_up_ammo(ped, weapons[i]);
    if (max_ammo > 0 && pack_weapon_spawn(weapons[i]) >= 0)
      wcomp_note("GTAVMenu pack weapon ammo 0x%08x max=%d", (unsigned)weapons[i], max_ammo);
  }
}

#if GTAV_MENU_ENABLE_CUSTOM_PACKS
// A pack weapon was given (game thread): its wheel slot, for pack-notes and the toast.
static const char* weapon_given_note(uint32_t weapon) {
  const int slot = pack_weapon_wheel_slot(weapon);
  wcomp_note("GTAVMenu pack weapon 0x%08x wheel slot=%d", (unsigned)weapon, slot);
#ifdef GTAV_WCOMP_PACK_LANE
  if (slot >= 0) {
    static char toast[48];
    snprintf(toast, sizeof(toast), "weapon given (wheel: %s slot)", kWheelSlotNames[slot]);
    return toast;
  }
#endif
  return "weapon given";
}
#endif
