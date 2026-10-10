// Wardrobe / outfit editor (Self > Wardrobe).
//
// Source fragment of features.cpp, textually included inside its anonymous namespace.
//
// Edits the player ped's clothing: SET_PED_COMPONENT_VARIATION / SET_PED_PROP_INDEX are
// modify-existing-ped field stores (same worker-safe class as the LSC setters, NOT the
// allocation-class SET_PLAYER_MODEL). Component changes can trigger texture streaming internally,
// so this is flagged for hardware verification like the rest of the wave. One compact UI: a Slot
// selector + Style/Texture cyclers that step+apply the selected slot live. The 17-slot editor
// state (drawable+texture per slot) is what Save/Apply Outfit persist via the profile (v8).

struct PedSlot {
  char name[14];   // inline (no relocation)
  uint8_t id;      // GTA V component id (0..11) or prop id (0,1,2,6,7)
  uint8_t is_prop; // 1 = SET_PED_PROP_INDEX slot, 0 = SET_PED_COMPONENT_VARIATION slot
};

// 12 components (ids 0..11) + 5 props. The order is the editor's slot index; it must total
// GTAV_WARDROBE_SLOTS so the editor arrays and the saved-outfit profile arrays line up.
static const PedSlot kPedSlots[] = {
    {"Face", 0, 0},     {"Mask", 1, 0},      {"Hair", 2, 0},   {"Torso", 3, 0},
    {"Legs", 4, 0},     {"Hands/Bags", 5, 0}, {"Shoes", 6, 0}, {"Neck", 7, 0},
    {"Undershirt", 8, 0}, {"Armor", 9, 0},   {"Decals", 10, 0}, {"Tops", 11, 0},
    {"Hat", 0, 1},      {"Glasses", 1, 1},   {"Earrings", 2, 1}, {"Watch", 6, 1},
    {"Bracelet", 7, 1},
};
static_assert(sizeof(kPedSlots) / sizeof(kPedSlots[0]) == GTAV_WARDROBE_SLOTS,
              "kPedSlots must have GTAV_WARDROBE_SLOTS entries");

static uint32_t g_wardrobe_slot_index = 0;
// Live editor state: the drawable + texture index the user has set for each component slot.
// features.cpp's export/import bridge memcpy these to/from the profile (the working set).
static uint32_t g_wardrobe_drawable[GTAV_WARDROBE_SLOTS];
static uint32_t g_wardrobe_texture[GTAV_WARDROBE_SLOTS];
// Saved outfit slots (profile v16): GTAV_SAVED_OUTFIT_SLOTS full loadouts, distinct from the live
// editor above. Save Outfit copies the editor into the selected slot; Apply Outfit copies the slot
// into the editor and onto the ped. g_outfit_slot selects the slot (worker-side, not persisted).
static uint32_t g_saved_outfit_drawable[GTAV_SAVED_OUTFIT_SLOTS][GTAV_WARDROBE_SLOTS];
static uint32_t g_saved_outfit_texture[GTAV_SAVED_OUTFIT_SLOTS][GTAV_WARDROBE_SLOTS];
static uint32_t g_outfit_slot = 0;
// value_label render buffers (copied immediately by push_list_values, so reuse is safe).
static char g_wardrobe_style_label[12];
static char g_wardrobe_texture_label[12];

static int wardrobe_drawable_count(int ped, uint32_t slot) {
  const PedSlot* s = &kPedSlots[slot];
  if (s->is_prop) {
    return g_n.get_number_of_ped_prop_drawable_variations
               ? invoke_return<int>(g_n.get_number_of_ped_prop_drawable_variations, ped, (int)s->id)
               : 0;
  }
  return g_n.get_number_of_ped_drawable_variations
             ? invoke_return<int>(g_n.get_number_of_ped_drawable_variations, ped, (int)s->id)
             : 0;
}

static int wardrobe_texture_count(int ped, uint32_t slot, int drawable) {
  const PedSlot* s = &kPedSlots[slot];
  if (s->is_prop) {
    return g_n.get_number_of_ped_prop_texture_variations
               ? invoke_return<int>(g_n.get_number_of_ped_prop_texture_variations, ped, (int)s->id,
                                    drawable)
               : 0;
  }
  return g_n.get_number_of_ped_texture_variations
             ? invoke_return<int>(g_n.get_number_of_ped_texture_variations, ped, (int)s->id,
                                  drawable)
             : 0;
}

// Apply one slot's stored drawable+texture to the ped.
static void wardrobe_apply_slot(int ped, uint32_t slot) {
  const PedSlot* s = &kPedSlots[slot];
  int d = (int)g_wardrobe_drawable[slot];
  int t = (int)g_wardrobe_texture[slot];
  if (s->is_prop) {
    // SET_PED_PROP_INDEX(ped, propId, drawableId, textureId, attach=1, p5=1).
    if (g_n.set_ped_prop_index) invoke_void(g_n.set_ped_prop_index, ped, (int)s->id, d, t, 1, 1);
  } else {
    // SET_PED_COMPONENT_VARIATION(ped, componentId, drawableId, textureId, paletteId=0).
    if (g_n.set_ped_component_variation)
      invoke_void(g_n.set_ped_component_variation, ped, (int)s->id, d, t, 0);
  }
}

// Slot selector: pure selection cycler (Style/Texture read g_wardrobe_slot_index). No game effect.
static uint32_t cycle_wardrobe_slot(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_SLOT;
  stage_step(&g_wardrobe_slot_index, GTAV_WARDROBE_SLOTS, param);
  return ok(action, kPedSlots[g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS].name);
}

// Step + apply the selected slot's drawable (live on every Left/Right; list_kinds.def
// SETTING_APPLY). Changing the drawable resets the texture to 0 (a texture index is only valid
// within its drawable). Cross (param 0) applies the shown style and texture as they are: the
// editor's value is not read back from the ped (no GET_PED_PROP_INDEX / GET_PED_DRAWABLE_VARIATION
// in the reviewed native table), so it can differ from what the ped wears -- a prop slot shows
// style 0 while no prop is on (fresh editor, Clear Prop, another ped). Writing the value the ped
// already wears changes nothing, so Cross applies unconditionally.
static uint32_t cycle_wardrobe_style(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_STYLE;
  if (!g_n.set_ped_component_variation && !g_n.set_ped_prop_index) {
    return unavailable(action, "missing wardrobe natives");
  }
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return failed(action, "player ped unavailable");
  uint32_t slot = g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS;
  int count = wardrobe_drawable_count(ped, slot);
  if (count <= 0) return failed(action, "no styles for this slot");
  int d = (int)g_wardrobe_drawable[slot];
  if (param == 2u)
    d = (d - 1 + count) % count;
  else if (param == 1u)
    d = (d + 1) % count;
  else
    d %= count;
  if (param == 1u || param == 2u || d != (int)g_wardrobe_drawable[slot] ||
      (int)g_wardrobe_texture[slot] >= wardrobe_texture_count(ped, slot, d)) {
    g_wardrobe_texture[slot] = 0;
  }
  g_wardrobe_drawable[slot] = (uint32_t)d;
  wardrobe_apply_slot(ped, slot);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s style %d/%d", kPedSlots[slot].name, d + 1, count);
  return ok(action, msg);
}

// Step + apply the selected slot's texture (live; Cross applies the shown one, see the Style row).
static uint32_t cycle_wardrobe_texture(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_TEXTURE;
  if (!g_n.set_ped_component_variation && !g_n.set_ped_prop_index) {
    return unavailable(action, "missing wardrobe natives");
  }
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return failed(action, "player ped unavailable");
  uint32_t slot = g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS;
  int count = wardrobe_texture_count(ped, slot, (int)g_wardrobe_drawable[slot]);
  if (count <= 0) return failed(action, "no colours for this style");
  int t = (int)g_wardrobe_texture[slot];
  if (param == 2u)
    t = (t - 1 + count) % count;
  else if (param == 1u)
    t = (t + 1) % count;
  else
    t %= count;
  g_wardrobe_texture[slot] = (uint32_t)t;
  wardrobe_apply_slot(ped, slot);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s colour %d/%d", kPedSlots[slot].name, t + 1, count);
  return ok(action, msg);
}

static uint32_t wardrobe_reset() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_WARDROBE_RESET;
  if (!g_n.set_ped_default_component_variation) {
    return unavailable(action, "missing wardrobe natives");
  }
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return failed(action, "player ped unavailable");
  invoke_void(g_n.set_ped_default_component_variation, ped);
  for (uint32_t i = 0; i < GTAV_WARDROBE_SLOTS; ++i) {
    g_wardrobe_drawable[i] = 0;
    g_wardrobe_texture[i] = 0;
  }
  return ok(action, "clothing reset to default");
}

static uint32_t wardrobe_clear_prop() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_WARDROBE_CLEAR_PROP;
  uint32_t slot = g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS;
  if (!kPedSlots[slot].is_prop) return unavailable(action, "selected slot is not a prop");
  if (!g_n.clear_ped_prop) return unavailable(action, "missing clear-prop native");
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return failed(action, "player ped unavailable");
  invoke_void(g_n.clear_ped_prop, ped, (int)kPedSlots[slot].id, 0);
  g_wardrobe_drawable[slot] = 0;
  g_wardrobe_texture[slot] = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s removed", kPedSlots[slot].name);
  return ok(action, msg);
}

// True when the selected outfit slot holds a saved outfit (any non-default component). Drives the
// overwrite confirm. An all-zero slot reads as empty (a fully-default outfit is indistinguishable,
// which is acceptable for the confirm gate).
static int saved_outfit_slot_occupied(void) {
  const uint32_t s = g_outfit_slot % GTAV_SAVED_OUTFIT_SLOTS;
  for (uint32_t i = 0; i < GTAV_WARDROBE_SLOTS; ++i) {
    if (g_saved_outfit_drawable[s][i] || g_saved_outfit_texture[s][i]) return 1;
  }
  return 0;
}

// Outfit-slot selector: pure worker-side selection cycler (Save/Apply read g_outfit_slot).
static uint32_t cycle_outfit_slot(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_OUTFIT_SLOT;
  stage_step(&g_outfit_slot, GTAV_SAVED_OUTFIT_SLOTS, param);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "outfit slot %u%s",
           (unsigned)((g_outfit_slot % GTAV_SAVED_OUTFIT_SLOTS) + 1u),
           saved_outfit_slot_occupied() ? " [set]" : " [empty]");
  return ok(action, msg);
}

// Persistent "< value >" render for the Outfit Slot row (copied immediately by push_list_values).
static char g_outfit_slot_label[24];
static const char* outfit_slot_value_label(void) {
  const int occupied = saved_outfit_slot_occupied();
  const char* format = locale_text_or(occupied ? "value.slot_set" : "value.slot_empty",
                                      occupied ? "Slot %u [set]" : "Slot %u [empty]");
  snprintf(g_outfit_slot_label, sizeof(g_outfit_slot_label), format,
           (unsigned)((g_outfit_slot % GTAV_SAVED_OUTFIT_SLOTS) + 1u));
  return g_outfit_slot_label;
}

static uint32_t save_outfit() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT;
  // Copy the live editor into the selected saved-outfit slot, then persist the whole profile.
  const uint32_t s = g_outfit_slot % GTAV_SAVED_OUTFIT_SLOTS;
  for (uint32_t i = 0; i < GTAV_WARDROBE_SLOTS; ++i) {
    g_saved_outfit_drawable[s][i] = g_wardrobe_drawable[i];
    g_saved_outfit_texture[s][i] = g_wardrobe_texture[i];
  }
  if (gtav_features_profile_save_default() != 0) return failed(action, "could not save outfit");
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "saved to outfit slot %u", (unsigned)(s + 1u));
  return ok(action, msg);
}

static uint32_t apply_outfit() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_APPLY_OUTFIT;
  if (!g_n.set_ped_component_variation && !g_n.set_ped_prop_index) {
    return unavailable(action, "missing wardrobe natives");
  }
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return failed(action, "player ped unavailable");
  const uint32_t s = g_outfit_slot % GTAV_SAVED_OUTFIT_SLOTS;
  if (!saved_outfit_slot_occupied()) return unavailable(action, "slot is empty");
  // Copy the saved slot into the live editor, then apply every slot to the ped.
  for (uint32_t i = 0; i < GTAV_WARDROBE_SLOTS; ++i) {
    g_wardrobe_drawable[i] = g_saved_outfit_drawable[s][i];
    g_wardrobe_texture[i] = g_saved_outfit_texture[s][i];
    wardrobe_apply_slot(ped, i);
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "applied outfit slot %u", (unsigned)(s + 1u));
  return ok(action, msg);
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Full-player "store" camera while the Wardrobe submenu is open: a scripted cam parked in front of
// the player ped, looking back at it, so outfit changes are visible (the normal follow camera often
// shows the player's back). g_wardrobe_cam_active is set by the worker each tick from the open-
// submenu state (native_bridge worker tick); this driver runs on the GAME thread from
// gtav_features_game_thread_tick() -- the cam create/activate/render natives fault on the worker.
// The menu stays VISIBLE (no vis_request, no input suppression): the panel renders over the framed
// ped, and the cam re-points each frame from the ped's live coords/heading so it always frames the
// front even if the player turns/walks. No POINT_CAM_AT_* / GET_ENTITY_FORWARD_VECTOR natives are
// crossmapped, so the look basis is computed from the heading (yaw = heading + 180) with the
// always-available libm-free trig.
static int g_wardrobe_cam_handle = 0;
static int g_wardrobe_cam_started = 0;
static uint32_t g_wardrobe_cam_last_frame = 0;
static const float GTAV_WARDROBE_CAM_DIST = 2.4f;    // metres in front of the ped (tune in-game)
static const float GTAV_WARDROBE_CAM_HEIGHT = 0.25f;  // above the ped origin (~chest framing)

static void wardrobe_cam_teardown(void) {
  if (g_wardrobe_cam_handle) {
    if (g_n.set_cam_active) invoke_void(g_n.set_cam_active, g_wardrobe_cam_handle, 0);
    if (g_n.destroy_cam) invoke_void(g_n.destroy_cam, g_wardrobe_cam_handle, 0);
    g_wardrobe_cam_handle = 0;
  }
  // Ease back to the gameplay camera.
  if (g_n.render_script_cams) invoke_void(g_n.render_script_cams, 0, 1, 500, 1, 0);
  g_wardrobe_cam_started = 0;
}

static void wardrobe_cam_game_thread_tick(void) {
  // Stand down when not in the Wardrobe submenu, or when a mutually-exclusive scripted-cam mode owns
  // the render cam (free cam / object move / noclip), so two cams never fight for ownership.
  if (!g_wardrobe_cam_active || g_free_cam_active || g_object_move_mode_enabled || g_noclip_enabled) {
    if (g_wardrobe_cam_started) wardrobe_cam_teardown();
    return;
  }
  if (!g_n.create_cam_with_params || !g_n.set_cam_active || !g_n.render_script_cams ||
      !g_n.set_cam_coord || !g_n.set_cam_rot || !g_n.get_entity_coords || !g_n.get_entity_heading ||
      !g_n.player_ped_id) {
    if (g_wardrobe_cam_started) wardrobe_cam_teardown();
    return;
  }
  // Collapse the many hook fires within one rendered frame to a single update.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_wardrobe_cam_last_frame) return;
    g_wardrobe_cam_last_frame = frame;
  }
  int ped = native_player_ped_id();
  if (!ped) return;
  const Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);  // GET_ENTITY_COORDS(ped, alive)
  const float h = invoke_return<float>(g_n.get_entity_heading, ped);  // degrees, 0 = +Y
  const float hr = h * GTAV_FEAT_DEG2RAD;
  const float fwd_x = -feat_sinf(hr);  // ped forward (no GET_ENTITY_FORWARD_VECTOR native)
  const float fwd_y = feat_cosf(hr);
  const float cam_x = c.x + fwd_x * GTAV_WARDROBE_CAM_DIST;
  const float cam_y = c.y + fwd_y * GTAV_WARDROBE_CAM_DIST;
  const float cam_z = c.z + GTAV_WARDROBE_CAM_HEIGHT;
  const float cam_yaw = h + 180.0f;  // look back toward the ped (replaces POINT_CAM_AT_*)
  if (!g_wardrobe_cam_started) {
    // CREATE_CAM_WITH_PARAMS(name, x,y,z, rx,ry,rz, fov, active, rotOrder); FOV set here (no
    // SET_CAM_FOV native crossmapped, so it can't be changed live).
    g_wardrobe_cam_handle =
        invoke_return<int>(g_n.create_cam_with_params, "DEFAULT_SCRIPTED_CAMERA", cam_x, cam_y,
                           cam_z, 0.0f, 0.0f, cam_yaw, 50.0f, 1, 2);
    invoke_void(g_n.set_cam_active, g_wardrobe_cam_handle, 1);
    invoke_void(g_n.render_script_cams, 1, 0, 0, 1, 0);
    g_wardrobe_cam_started = 1;
  }
  invoke_void(g_n.set_cam_coord, g_wardrobe_cam_handle, cam_x, cam_y, cam_z);
  invoke_void(g_n.set_cam_rot, g_wardrobe_cam_handle, 0.0f, 0.0f, cam_yaw, 2);
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK
