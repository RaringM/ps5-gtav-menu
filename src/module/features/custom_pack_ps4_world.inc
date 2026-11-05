// PS4 world boundary: archive vehicle packs do not enable typ/map/bounds actions yet.
static void pack_world_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void pack_world_note(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, text);
  char line[sizeof(text) + 16];
  snprintf(line, sizeof(line), "GTAVMenu %s", text);
  gtav_worker_klog(line);
}

__attribute__((unused)) static void pack_interior_diag_tick() {}
__attribute__((unused)) static int pack_interior_collision_ready() { return 1; }

static uint64_t g_pack_typs_loaded;
static uint64_t g_pack_typs_failed;
static int g_pack_map_waiting_typs;
static uint32_t g_pack_maps_requested;
static uint32_t g_pack_maps_failed;
static uint32_t g_pack_maps_finished;
static uint32_t g_pack_bounds_failed;
static uint32_t g_pack_bounds_finished;

static uint32_t load_pack_typ(uint32_t) {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP,
                     "pack typ/map loading is not supported on PS4");
}
static uint32_t load_pack_map(uint32_t) {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP,
                     "pack typ/map loading is not supported on PS4");
}
static uint32_t finish_pack_map(uint32_t) {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP,
                     "pack typ/map loading is not supported on PS4");
}
static uint32_t load_pack_bounds(uint32_t) {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS,
                     "pack bounds loading is not supported on PS4");
}
