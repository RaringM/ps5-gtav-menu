// Single-player money.
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

// Give single-player cash. Mirrors the reference GiveSPCash(): write all three
// SP{0,1,2}_TOTAL_CASH stats so the increment lands on whichever protagonist is active
// without having to detect the current character. The stat hashes are precomputed
// joaat(name) (same joaat() as the catalogs) so no GET_HASH_KEY call is needed.
// STAT_GET_INT/STAT_SET_INT touch the global stats store (no entity/script context), so
// this runs worker-direct like the other Self one-shots. STAT_SET_INT's third arg `save`
// is 1 so the new balance persists to the save file. The dollar amount rides in `param`.
// SP{0,1,2}_TOTAL_CASH stat joaat hashes -- one per protagonist. Writing all three lets the
// money one-shots land on whichever character is active without detecting them. Verified:
// joaat("sp0_total_cash") == 0x0324C31D (host-tested in test_stat_hashes.py).
static const uint32_t kSpCashStats[] = {
    0x0324C31Du,  // SP0_TOTAL_CASH (Michael)
    0x44BD6982u,  // SP1_TOTAL_CASH (Franklin)
    0x8D75047Du,  // SP2_TOTAL_CASH (Trevor)
};

static uint32_t give_money(uint32_t amount) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY;
  if (!g_n.stat_get_int || !g_n.stat_set_int) {
    return unavailable(action, "missing stat natives");
  }
  const int delta = (int)amount;
  for (uint32_t i = 0; i < sizeof(kSpCashStats) / sizeof(kSpCashStats[0]); ++i) {
    int cur = 0;
    invoke_void(g_n.stat_get_int, kSpCashStats[i], &cur, -1);
    invoke_void(g_n.stat_set_int, kSpCashStats[i], cur + delta, 1);
  }
  const char* msg = "cash added";
  if (amount >= 1000000u) {
    msg = "added $1,000,000";
  } else if (amount >= 100000u) {
    msg = "added $100,000";
  } else if (amount >= 10000u) {
    msg = "added $10,000";
  }
  return ok(action, msg);
}

// Set Cash: set the active character's on-hand cash to an EXACT amount, where GIVE_MONEY only
// ADDS. A SHELL_ROW_LIST cycler -- Left/Right (param 2/1) stage a preset, Cross (param 0) writes
// it. STAT_SET_INT save=1 commits to the save file (same worker-safe store as give_money), so a
// write is persistent. The top preset is capped under INT_MAX (the stat is a signed 32-bit int).
typedef struct {
  uint32_t amount;
  char label[16];
} CashPreset;

static const CashPreset kCashPresets[] = {
    {0u, "$0"},
    {10000u, "$10,000"},
    {100000u, "$100,000"},
    {1000000u, "$1,000,000"},
    {10000000u, "$10,000,000"},
    {100000000u, "$100,000,000"},
    {2000000000u, "$2,000,000,000"},  // < INT_MAX (2,147,483,647)
};
// Default to the $1,000,000 preset so the first apply is a sensible amount.
static uint32_t g_cash_index = 3u;

static const char* cash_value_label(void) {
  return kCashPresets[g_cash_index % (sizeof(kCashPresets) / sizeof(kCashPresets[0]))].label;
}

static uint32_t set_cash(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CASH;
  const uint32_t count = sizeof(kCashPresets) / sizeof(kCashPresets[0]);
  // Stage prev/next without writing (Left = 2 steps back, Right = 1 steps forward).
  if (stage_step(&g_cash_index, count, param)) return ok(action, cash_value_label());
  if (!g_n.stat_set_int) return unavailable(action, "missing stat natives");
  const int amount = (int)kCashPresets[g_cash_index % count].amount;
  for (uint32_t i = 0; i < sizeof(kSpCashStats) / sizeof(kSpCashStats[0]); ++i) {
    invoke_void(g_n.stat_set_int, kSpCashStats[i], amount, 1);
  }
  char msg[32];
  snprintf(msg, sizeof(msg), "cash set: %s", cash_value_label());
  return ok(action, msg);
}

// Max All Stats: write every SP skill stat (+ special-ability capacity) to 100, the documented
// cap, for all three protagonists so it applies to whoever is active. Pure global stat writes
// (STAT_SET_INT, save=1), worker-safe like give_money. Hashes are precomputed joaat(name),
// host-verified in test_stat_hashes.py; STAT_SET_INT on an unknown hash is a silent no-op (no
// crash). Both SPECIAL_ABILITY and SPECIAL_ABILITY_UNLOCKED are written since the persisted
// "special" skill stat name varies by export.
static uint32_t max_all_stats(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_MAX_ALL_STATS;
  if (!g_n.stat_set_int) return unavailable(action, "missing stat natives");
  static const uint32_t kMaxStats[] = {
      // SP0 (Michael): stamina, strength, lung, shooting, stealth, flying, driving, wheelie,
      // special, special-unlocked.
      0x22C8AAA2u, 0x906B2799u, 0x73968EBDu, 0xB4892709u, 0x2268B791u, 0x78ABE4E6u, 0x8D3D751Cu,
      0x11B47270u, 0xDCDD4CA8u, 0x4ECD9F81u,
      // SP1 (Franklin)
      0x255EFFB5u, 0xB82874E3u, 0x6C3BBB1Au, 0xCB261497u, 0xE76D0C23u, 0xE98BEE3Du, 0xE43E1F8Cu,
      0x7DD80AC8u, 0x15151676u, 0x51CCFBA3u,
      // SP2 (Trevor)
      0x7D8246AEu, 0x4F19E159u, 0x7E9487B3u, 0x2A3A74EAu, 0xD03B7EEBu, 0x77CF9710u, 0x6BB49903u,
      0x6BEF592Fu, 0x078FE7A3u, 0x05B06442u,
  };
  for (uint32_t i = 0; i < sizeof(kMaxStats) / sizeof(kMaxStats[0]); ++i) {
    invoke_void(g_n.stat_set_int, kMaxStats[i], 100, 1);
  }
  return ok(action, "all stats maxed");
}
