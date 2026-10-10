// World cyclers (weather, time, wanted, gravity, time-scale, move rate)
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

// Weather types cycled by CYCLE_WEATHER (hashed via GET_HASH_KEY, applied with
// SET_WEATHER_TYPE_NOW_PERSIST so it sticks). Order roughly clear -> stormy.
// Inline char arrays (not const char*) so no R_X86_64_RELATIVE relocation is needed --
// the etaHEN fake-self PRX loader does not reliably apply those. See feature_catalog.h.
static const char kWeatherTypes[][12] = {
    "EXTRASUNNY", "CLEAR", "CLOUDS", "OVERCAST", "RAIN",
    "THUNDER",    "SMOG",  "FOGGY",  "SNOW",     "BLIZZARD",
};

// Time-of-day presets cycled by CYCLE_TIME (applied with NETWORK_OVERRIDE_CLOCK_TIME).
struct GtavTimeOfDay {
  char name[16];  // inline (no relocation)
  int hour;
};
static const GtavTimeOfDay kTimesOfDay[] = {
    {"MORNING", 8},
    {"NOON", 12},
    {"EVENING", 18},
    {"NIGHT", 0},
};

static void apply_selected_time() {
  if (!g_n.network_override_clock_time) return;
  const uint32_t count = (uint32_t)(sizeof(kTimesOfDay) / sizeof(kTimesOfDay[0]));
  const GtavTimeOfDay& t = kTimesOfDay[g_time_index % count];
  invoke_void(g_n.network_override_clock_time, t.hour, 0, 0);
}

// Step a wraparound list index: dir < 0 = previous, dir >= 0 = next.
static uint32_t list_step(uint32_t index, uint32_t count, int dir) {
  if (count == 0u) return 0u;
  index %= count;
  if (dir < 0) return (index + count - 1u) % count;
  return (index + 1u) % count;
}

// Stage half of every list cycler's STAGE/APPLY contract: on a Left/Right press
// (param 1 = next, 2 = prev) advance *idx and return 1; on the apply press (param 0)
// leave *idx untouched and return 0 so the caller runs its apply path. Pairs with
// list_step so the stage step is spelled once instead of inline in every cycler.
static int stage_step(uint32_t* idx, uint32_t count, uint32_t param) {
  if (param != 1u && param != 2u) return 0;
  *idx = list_step(*idx, count, param == 2u ? -1 : 1);
  return 1;
}

// Pick-then-apply for the live mode rows (Fly, Vehicle Fly / Hover, Autopilot Mode, Gun Toolkit;
// CHOICE in list_kinds.def). Their game-thread drivers read a live mode int, so stepping it
// directly engaged every mode on the way (Off -> On -> Fast). Left/Right now move `pick`; Cross
// copies it to the live mode. `seen` is the live mode the pick last agreed with: when something
// else changes the live mode (Stop Autopilot, the autopilot auto-off, Disable All, a teleport
// reset) the pick follows it instead of showing a stale value. Worker-only.
struct ModePick {
  uint32_t pick;
  int seen;
};
static uint32_t mode_pick_sync(ModePick* p, int live) {
  if (live != p->seen) {
    p->seen = live;
    p->pick = (uint32_t)live;
  }
  return p->pick;
}
// Same contract as stage_step: Left/Right (param 1/2) step the pick and return 1; Cross (param 0)
// commits the pick to *live and returns 0 so the caller runs its apply path.
static int mode_pick_step(ModePick* p, volatile int* live, uint32_t count, uint32_t param) {
  mode_pick_sync(p, *live);
  if (stage_step(&p->pick, count, param)) return 1;
  *live = (int)(p->pick % count);
  p->seen = *live;
  return 0;
}

// List cyclers use a STAGE/APPLY model so Left/Right only PICK a value and Select
// (Cross) APPLIES it. param: 1 = stage next, 2 = stage prev, 0 = apply current.
// Staging just advances the index (the menu re-reads gtav_features_value_label to
// show "< VALUE >"); only apply calls the gameplay native.
static uint32_t cycle_weather(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WEATHER;
  const uint32_t count = (uint32_t)(sizeof(kWeatherTypes) / sizeof(kWeatherTypes[0]));
  if (stage_step(&g_weather_index, count, param)) return ok(action, kWeatherTypes[g_weather_index]);
  if (!g_n.set_weather_type_now_persist) {
    return unavailable(action, "missing weather native");
  }
  // Apply: SET_WEATHER_TYPE_NOW_PERSIST takes the weather-name STRING (it hashes it
  // internally) -- pass the pointer, never a pre-hashed int.
  const char* name = kWeatherTypes[g_weather_index % count];
  invoke_void(g_n.set_weather_type_now_persist, name);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "weather set: %s", name);
  return ok(action, msg);
}

static uint32_t cycle_time(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME;
  const uint32_t count = (uint32_t)(sizeof(kTimesOfDay) / sizeof(kTimesOfDay[0]));
  if (stage_step(&g_time_index, count, param)) return ok(action, kTimesOfDay[g_time_index].name);
  if (!g_n.network_override_clock_time) {
    return unavailable(action, "missing clock native");
  }
  // Apply: NETWORK_OVERRIDE_CLOCK_TIME(h, m, s) forces the world clock in SP too.
  const GtavTimeOfDay& t = kTimesOfDay[g_time_index % count];
  apply_selected_time();
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "time set: %s", t.name);
  return ok(action, msg);
}

static uint32_t toggle_freeze_time() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_FREEZE_TIME;
  if (!g_n.network_override_clock_time) {
    return unavailable(action, "missing clock native");
  }
  g_freeze_time_enabled = !g_freeze_time_enabled;
  if (g_freeze_time_enabled) apply_selected_time();
  return ok(action, g_freeze_time_enabled ? "freeze time enabled" : "freeze time disabled");
}

// Wanted-level labels for the SET_WANTED_LEVEL list cycler (index == star count).
static const char kWantedLevels[][12] = {
    "0 STARS", "1 STAR", "2 STARS", "3 STARS", "4 STARS", "5 STARS",
};

static uint32_t cycle_wanted(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WANTED;
  const uint32_t count = (uint32_t)(sizeof(kWantedLevels) / sizeof(kWantedLevels[0]));
  if (stage_step(&g_wanted_index, count, param)) return ok(action, kWantedLevels[g_wanted_index]);
  if (!g_n.player_id || !g_n.set_player_wanted_level || !g_n.set_player_wanted_level_now) {
    return unavailable(action, "missing wanted-level natives");
  }
  // Apply: SET_PLAYER_WANTED_LEVEL(player, level, delayed) then _NOW to commit it.
  int player = native_player_id();
  int level = (int)(g_wanted_index % count);
  invoke_void(g_n.set_player_wanted_level, player, level, 0);
  invoke_void(g_n.set_player_wanted_level_now, player, 0);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "wanted set: %s", kWantedLevels[g_wanted_index % count]);
  return ok(action, msg);
}

static uint32_t cycle_nitro_power(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_NITRO_POWER;
  const uint32_t count = (uint32_t)(sizeof(kNitroProfiles) / sizeof(kNitroProfiles[0]));
  stage_step(&g_nitro_power_index, count, param);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "nitro power: %s", current_nitro_profile().label);
  return ok(action, msg);
}

// Time-scale presets cycled by CYCLE_TIME_SCALE (applied with SET_TIME_SCALE). A
// global slow-motion control finer than the on/off Slow Motion toggle.
struct GtavTimeScale {
  char label[12];  // inline (no relocation)
  float scale;
};
static const GtavTimeScale kTimeScales[] = {
    {"1.0x", 1.0f}, {"0.75x", 0.75f}, {"0.5x", 0.5f}, {"0.25x", 0.25f}, {"0.1x", 0.1f},
};

// Gravity-level presets cycled by CYCLE_GRAVITY (applied with SET_GRAVITY_LEVEL, which
// takes an integer level 0..3 from strongest to weakest). Finer than the Low Gravity
// on/off toggle.
struct GtavGravityLevel {
  char label[12];  // inline (no relocation)
  int level;
};
static const GtavGravityLevel kGravityLevels[] = {
    {"NORMAL", 0},
    {"LOW", 1},
    {"LOWER", 2},
    {"MOON", 3},
};

static uint32_t cycle_clock_hour(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CLOCK_HOUR;
  if (param == 1u || param == 2u) {
    g_clock_hour = (param == 2u) ? (g_clock_hour + 23u) % 24u : (g_clock_hour + 1u) % 24u;
    char buf[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(buf, sizeof(buf), "%02u:00", g_clock_hour % 24u);
    return ok(action, buf);
  }
  if (!g_n.set_clock_time && !g_n.network_override_clock_time) {
    return unavailable(action, "missing clock native");
  }
  // Apply: prefer SET_CLOCK_TIME(h,m,s); fall back to NETWORK_OVERRIDE_CLOCK_TIME.
  if (g_n.set_clock_time) {
    invoke_void(g_n.set_clock_time, (int)(g_clock_hour % 24u), 0, 0);
  } else {
    invoke_void(g_n.network_override_clock_time, (int)(g_clock_hour % 24u), 0, 0);
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "clock set: %02u:00", g_clock_hour % 24u);
  return ok(action, msg);
}

static uint32_t cycle_time_scale(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME_SCALE;
  const uint32_t count = (uint32_t)(sizeof(kTimeScales) / sizeof(kTimeScales[0]));
  if (param == 1u || param == 2u) {
    // Presets are stored normal-to-slowest so index 0 remains the safe/default
    // 1.0x profile value. The displayed slider is numeric, so left lowers the
    // scale and right raises it.
    g_timescale_index = list_step(g_timescale_index, count, param == 2u ? 1 : -1);
    return ok(action, kTimeScales[g_timescale_index % count].label);
  }
  if (!g_n.set_time_scale) {
    return unavailable(action, "missing time-scale native");
  }
  const GtavTimeScale& t = kTimeScales[g_timescale_index % count];
  invoke_void(g_n.set_time_scale, t.scale);
  // Keep the Slow Motion toggle's display in sync: any scale below 1.0 is slow-mo.
  g_slow_motion_enabled = (t.scale < 1.0f) ? 1 : 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "time scale: %s", t.label);
  return ok(action, msg);
}

static uint32_t cycle_gravity(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_GRAVITY;
  const uint32_t count = (uint32_t)(sizeof(kGravityLevels) / sizeof(kGravityLevels[0]));
  if (stage_step(&g_gravity_index, count, param))
    return ok(action, kGravityLevels[g_gravity_index % count].label);
  if (!g_n.set_gravity_level) {
    return unavailable(action, "missing gravity native");
  }
  const GtavGravityLevel& g = kGravityLevels[g_gravity_index % count];
  invoke_void(g_n.set_gravity_level, g.level);
  // Keep the Low Gravity toggle's display in sync.
  g_low_gravity_enabled = (g.level != 0) ? 1 : 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "gravity: %s", g.label);
  return ok(action, msg);
}

// On-foot movement-rate presets cycled by CYCLE_MOVE_RATE. These set the strength of
// the Fast Movement toggle: the worker tick reasserts the selected multiplier through
// SET_PED_MOVE_RATE_OVERRIDE (the same native the toggle already owns), so picking a
// rate here tunes how fast on-foot movement is without a separate native.
struct GtavMoveRate {
  char label[8];  // inline (no relocation)
  float rate;
};
static const GtavMoveRate kMoveRates[] = {
    {"1.2x", 1.2f},
    {"1.4x", 1.4f},
    {"1.8x", 1.8f},
    {"2.5x", 2.5f},
};

static float current_move_rate() {
  const uint32_t count = (uint32_t)(sizeof(kMoveRates) / sizeof(kMoveRates[0]));
  return kMoveRates[g_move_rate_index % count].rate;
}

static uint32_t cycle_move_rate(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_MOVE_RATE;
  const uint32_t count = (uint32_t)(sizeof(kMoveRates) / sizeof(kMoveRates[0]));
  if (stage_step(&g_move_rate_index, count, param))
    return ok(action, kMoveRates[g_move_rate_index % count].label);
  // Apply: turn Fast Movement on so the worker tick starts asserting the selected rate.
  g_fast_move_enabled = 1;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "move speed: %s", kMoveRates[g_move_rate_index % count].label);
  return ok(action, msg);
}

// --- Los Santos Customs (worker-safe vehicle mod + respray) ---
// Curated paint palette using GTA V "classic" colour ids (cross-checked against
// PC-MenyooSP's COLOR_CLASSIC_* enum). Any 0..159 id is a valid paint, so the apply
// path can never fault on an out-of-range colour.
struct PaintColor {
  char label[12];  // inline (no relocation; see feature_catalog.h)
  int id;
};
