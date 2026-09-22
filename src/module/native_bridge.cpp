// Tag this translation unit's GTAV_LOG*/NB_TRACE lines as the native subsystem. Must precede
// the first include, since features.h pulls in log.h (which else defaults the category).
#define GTAV_LOG_DEFAULT_CATEGORY GTAV_LOG_CAT_NATIVE

#include "gtavmenu/native_bridge.h"

#include "gtavmenu/abi.h"
#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/feature_catalog.h"
#include "gtavmenu/feature_profile.h"
#include "gtavmenu/features.h"
#include "gtavmenu/frame_hook.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu_draw_list.h"
#include "gtavmenu/status.h"

#include "gtavmenu/native_invoke.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Breadcrumb trace into the worker log ring at DEBUG, so it surfaces only when Telemetry
// raises the threshold. Cheap when below threshold: gtav_vlogf drops it before formatting.
#define NB_TRACE(...) GTAV_LOGD(__VA_ARGS__)

#ifndef GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY
#define GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY 0
#endif

#ifndef GTAV_MENU_WORKER_TEXT_OVERLAY_INTERVAL
#define GTAV_MENU_WORKER_TEXT_OVERLAY_INTERVAL 1
#endif

// Vehicle-spawner image preview (default off; GTAV_MENU_ENABLE_VEHICLE_PREVIEW=1). The texture dict
// is streamed on the game thread by the feature layer. Legacy builds draw the highlighted image on
// the worker; phase-list builds copy the sprite command for render-callback submission.
#ifndef GTAV_MENU_ENABLE_VEHICLE_PREVIEW
#define GTAV_MENU_ENABLE_VEHICLE_PREVIEW 0
#endif

// Real PlayStation button-glyph sprites on the bottom instructional-button bar (default off;
// GTAV_MENU_ENABLE_BUTTON_GLYPHS=1). The bar always renders coloured button CHIPS (DRAW_RECT +
// token text, fully verified + worker-safe); with this on it additionally tries DRAW_SPRITE of a
// real controller-button texture for each glyph, falling back to the chip whenever the (txd,
// texture) is empty or its dict is not yet resident -- so it can never draw a white "missing
// texture" quad. The glyph (txd, texture) names in kHintGlyphInfo are intentionally empty until
// verified on hardware (we have no GET_TEXTURE_RESOLUTION on this lane to auto-reject a wrong
// name); fill them in there, then build the live menu with this flag to light them up.
#ifndef GTAV_MENU_ENABLE_BUTTON_GLYPHS
#define GTAV_MENU_ENABLE_BUTTON_GLYPHS 0
#endif

#if GTAV_MENU_PHASE_DRAW_LIST && GTAV_MENU_ENABLE_BUTTON_GLYPHS
#error "Phase draw-list gate does not support worker-streamed button-glyph sprites"
#endif

// Gate worker-thread native calls (render + getters) on the frame hook having fired
// in valid script context, i.e. single-player gameplay is live. Worker natives are
// unsafe before that: injected before SP finishes loading, the per-tick render/getter
// natives run on the scePad worker with no script context and crash the load
// (hardware-confirmed when the watch lane injected at the landing page). With this on,
// the worker stays render-inert until the hook reports gameplay, so injection timing
// no longer matters. 0 = legacy (render unconditionally once the native table resolves).
#ifndef GTAV_MENU_WORKER_REQUIRE_CONTEXT
#define GTAV_MENU_WORKER_REQUIRE_CONTEXT 0
#endif

// True once worker-thread natives are safe to call. When the context gate is on, that
// means the frame hook has fired in valid script context at least once (calls_with_context
// > 0) -- a monotonic "gameplay has started" latch. Without the frame hook (bare build)
// there is no such signal, so fall back to the legacy "always ready".
static int worker_natives_ready(void) {
#if GTAV_MENU_WORKER_REQUIRE_CONTEXT && defined(GTAV_MENU_ENABLE_FRAME_HOOK) && \
    GTAV_MENU_ENABLE_FRAME_HOOK
  GtavFrameHookProbeSnapshot snap;
  gtav_frame_hook_probe_snapshot(&snap);
  return snap.calls_with_context > 0 ? 1 : 0;
#else
  return 1;
#endif
}

// Render park flag. When set, gtav_native_bridge_worker_tick refreshes its status snapshot but
// issues NO drawing/getter natives -- see gtav_native_bridge_park() below. Driven by the
// teardown/suspend guard (quit_guard.c) so a force-close / rest-mode suspend does not find the
// worker still feeding GTA's draw lists (which keeps the GPU non-idle and trips the
// SYSTEM_SUSPEND_BLOCK_TIMEOUT crash). The phase consumer also reads this flag, so accesses must
// remain lock-free and atomic across the worker and callback threads.
static int g_render_parked = 0;
static_assert(__atomic_always_lock_free(sizeof(int), nullptr),
              "render park flag must be lock-free");

static int render_is_parked(void) {
  return __atomic_load_n(&g_render_parked, __ATOMIC_ACQUIRE);
}

extern "C" void gtav_native_bridge_park(void) {
  if (__atomic_exchange_n(&g_render_parked, 1, __ATOMIC_ACQ_REL)) return;
  gtav_status_eventf(GTAV_MENU_EVENT_SHUTDOWN, "render PARKED: worker draw stood down (suspend)");
}
extern "C" void gtav_native_bridge_unpark(void) {
  if (!__atomic_exchange_n(&g_render_parked, 0, __ATOMIC_ACQ_REL)) return;
  gtav_status_eventf(GTAV_MENU_EVENT_SHUTDOWN, "render UNPARKED: worker draw re-armed");
}
extern "C" int gtav_native_bridge_is_parked(void) {
  return render_is_parked();
}

// Runtime-tunable render cadence, seeded from the compile-time default. Adjustable
// live via the SET_RENDER_INTERVAL mailbox command so the smooth-vs-flicker sweet
// spot can be found without rebuild/re-inject. 1 = render every worker tick.
static uint32_t g_render_interval = (uint32_t)GTAV_MENU_WORKER_TEXT_OVERLAY_INTERVAL;

extern "C" void gtav_native_bridge_set_render_interval(uint32_t interval) {
  if (interval >= 1u && interval <= 240u) g_render_interval = interval;
}
extern "C" uint32_t gtav_native_bridge_render_interval(void) {
  return g_render_interval;
}

#ifndef GTAV_MENU_ENABLE_TOASTS
#define GTAV_MENU_ENABLE_TOASTS 0
#endif

// On-screen toast / status-confirmation queue. Each feature action already
// produces a result code + human-readable message; these toasts surface that on
// screen (bottom-centre) so the user sees that a setting took effect or why it
// failed. Drawn with the proven DRAW_RECT + text natives, independent of menu
// visibility (modelled on render_hud_overlay). Toasts are pushed from menu.c on
// the same worker thread that renders them (push happens in
// consume_controller_input/mailbox before gtav_native_bridge_worker_tick in the
// same tick), so no lock or atomics are needed.
// Default 270 ticks = ~4.5s at the 60 Hz worker rate. Tunable live via the Toast Time
// menu row / SET_TOAST_TICKS mailbox command, mirroring g_render_interval. Kept outside
// the build gate so the setter/getter ABI is stable in every build. (Lifetime is counted
// in worker ticks, so it scales inversely with worker_hz.)
static uint32_t g_toast_ticks = 270u;

extern "C" void gtav_native_bridge_set_toast_ticks(uint32_t ticks) {
  if (ticks >= 30u && ticks <= 1800u) g_toast_ticks = ticks;
}
extern "C" uint32_t gtav_native_bridge_toast_ticks(void) {
  return g_toast_ticks;
}

// Persisted selection index for the Worker Hz and Toast Time menu cyclers. These used to be
// function-static in menu.c, so a tuned value silently reverted on every re-inject. The bridge
// holds them as plain data (the choice tables + apply live in menu.c, which owns the worker
// period); features.cpp round-trips them through the profile like theme_index/region_index, and
// menu.c reads/writes them from the cyclers and applies them at init. Defaults match the
// historical function-static seeds (60 Hz / 4.5 s).
static uint32_t g_worker_hz_index = 0u;
static uint32_t g_toast_time_index = 2u;
// v13 accessibility/feel tuners. g_reduce_motion (0 = full motion, 1 = reduced) is read directly by
// the draw path's animation helpers; the Nav Delay / Nav Speed indices are plain persisted data
// (the choice tables + apply, which actuate the pad mapper, live in menu.c) -- defaults match the
// historical pad repeat cadence (Normal/Normal).
static uint32_t g_reduce_motion = 0u;
static uint32_t g_nav_delay_index = 1u;
static uint32_t g_nav_speed_index = 1u;
// v14: optional touchpad gesture input (swipe to scroll/page, tap/click to select), layered on top
// of the d-pad. Default OFF -- the live touch coordinate range + finger encoding are unverified on
// hardware, so per the safety policy it stays opt-in until calibrated. Read by the live pad poll
// path (pad_input.c) through the accessor below.
static uint32_t g_touchpad_enabled = 0u;

extern "C" void gtav_native_bridge_set_worker_hz_index(uint32_t index) {
  g_worker_hz_index = index;
}
extern "C" uint32_t gtav_native_bridge_worker_hz_index(void) {
  return g_worker_hz_index;
}
extern "C" void gtav_native_bridge_set_toast_time_index(uint32_t index) {
  g_toast_time_index = index;
}
extern "C" uint32_t gtav_native_bridge_toast_time_index(void) {
  return g_toast_time_index;
}
extern "C" void gtav_native_bridge_set_reduce_motion(int on) {
  g_reduce_motion = on ? 1u : 0u;
}
extern "C" int gtav_native_bridge_reduce_motion(void) {
  return (int)g_reduce_motion;
}
extern "C" void gtav_native_bridge_set_nav_delay_index(uint32_t index) {
  g_nav_delay_index = index;
}
extern "C" uint32_t gtav_native_bridge_nav_delay_index(void) {
  return g_nav_delay_index;
}
extern "C" void gtav_native_bridge_set_nav_speed_index(uint32_t index) {
  g_nav_speed_index = index;
}
extern "C" uint32_t gtav_native_bridge_nav_speed_index(void) {
  return g_nav_speed_index;
}
extern "C" void gtav_native_bridge_set_touchpad_enabled(int on) {
  g_touchpad_enabled = on ? 1u : 0u;
}
extern "C" int gtav_native_bridge_touchpad_enabled(void) {
  return (int)g_touchpad_enabled;
}

// Last worker tick seen by the bridge; toast expiry + the motion-feedback flash decay are
// measured against it so a push/event from menu.c lands the same tick the renderer reads. Kept
// outside the toast gate so the (toast-independent) motion feedback can use it in every build.
static uint64_t g_last_worker_tick;

#if GTAV_MENU_ENABLE_TOASTS
#define GTAV_TOAST_TEXT_LEN 80u  // matches GTAV_FEATURE_MESSAGE_LEN
#define GTAV_TOAST_SLOTS 4u

struct Toast {
  char text[GTAV_TOAST_TEXT_LEN];  // inline buffer, relocation-free
  uint8_t r, g, b;
  uint64_t birth_tick;  // worker tick the toast was pushed (drives fade ramps)
  uint64_t expire_tick;
  uint16_t repeat;  // identical consecutive pushes coalesced into one pill ("(xN)")
  uint8_t active;
};
static Toast g_toasts[GTAV_TOAST_SLOTS];
static uint32_t g_toast_head;  // ring write cursor (next slot to overwrite)

static void toast_color(uint32_t result_code, uint8_t* r, uint8_t* g, uint8_t* b) {
  switch (result_code) {
    case GTAV_FEATURE_RESULT_OK:
      *r = 90;
      *g = 210;
      *b = 110;
      break;  // green
    case GTAV_FEATURE_RESULT_FAILED:
      *r = 225;
      *g = 70;
      *b = 70;
      break;  // red
    case GTAV_FEATURE_RESULT_UNAVAILABLE:
      *r = 235;
      *g = 180;
      *b = 70;
      break;  // amber
    default:
      *r = 235;
      *g = 242;
      *b = 255;
      break;  // off-white
  }
}
#endif  // GTAV_MENU_ENABLE_TOASTS

// Per-glyph advance widths for the default GTA font (Chalet London Comprime, a
// condensed sans), in 1/100 em. The renderer sums these so the toast panel hugs
// the proportional text instead of assuming a fixed per-char width. Calibrated so
// an average glyph ~= 100; combined with kEmWidth (the old flat advance) an
// average-width string reproduces the previous box width (no regression). Indices
// are ASCII 0..127; >127 and unmapped controls fall back to 90 in the sum loop.
// Compiled only when the text-menu-list renderer or the toast renderer is active,
// both of which call menu_text_width / render_toasts.
#if (GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY && GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST) || \
    (GTAV_MENU_ENABLE_TOASTS && GTAV_MENU_ENABLE_NATIVE_FEATURES)
// clang-format off
static const uint8_t kGlyphAdvance[128] = {
    // 0x00-0x1F control chars
     90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,
     90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,
    // 0x20-0x2F  sp ! " # $ % & ' ( ) * + , - . /
     45,  40,  58,  90,  90, 120, 120,  40,  48,  48,  70,  90,  40,  58,  40,  58,
    // 0x30-0x3F  0 1 2 3 4 5 6 7 8 9 : ; < = > ?
     90,  58,  90,  90,  90,  90,  90,  90,  90,  90,  40,  40,  90,  90,  90,  85,
    // 0x40-0x4F  @ A B C D E F G H I J K L M N O
    130, 120, 120, 120, 120, 110, 105, 125, 120,  45,  70, 120, 100, 140, 120, 125,
    // 0x50-0x5F  P Q R S T U V W X Y Z [ \ ] ^ _
    115, 125, 120, 115, 110, 120, 118, 140, 118, 115, 110,  48,  58,  48,  90,  90,
    // 0x60-0x6F  ` a b c d e f g h i j k l m n o
     90,  90,  92,  88,  92,  90,  58,  92,  92,  40,  45,  88,  40, 130,  92,  92,
    // 0x70-0x7F  p q r s t u v w x y z { | } ~ DEL
     92,  92,  58,  85,  58,  92,  88, 120,  88,  88,  85,  70,  40,  70,  90,  90,
};
// clang-format on
#endif  // text-menu-list or toasts active

#if GTAV_MENU_ENABLE_TOASTS
// Non-colour status token prefixed to every toast so the result reads without relying on the
// accent-bar colour (colourblind-safe + glanceable): success/fail/blocked/info.
static const char* toast_status_glyph(uint32_t result_code) {
  switch (result_code) {
    case GTAV_FEATURE_RESULT_OK:
      return "[OK] ";
    case GTAV_FEATURE_RESULT_FAILED:
      return "[X] ";
    case GTAV_FEATURE_RESULT_UNAVAILABLE:
      return "[!] ";
    default:
      return "[i] ";
  }
}
#endif

extern "C" void gtav_native_bridge_push_toast(const char* text, uint32_t result_code) {
#if GTAV_MENU_ENABLE_TOASTS
  if (!text || !text[0]) return;
  char full[GTAV_TOAST_TEXT_LEN];
  snprintf(full, sizeof(full), "%s%s", toast_status_glyph(result_code), text);  // glyph + message
  // De-dup: if the most recently written toast is still live and carries the same text, coalesce
  // this push into it -- bump the repeat counter and refresh its lifetime -- instead of spamming a
  // new pill (e.g. mashing a toggle, or a per-tick status repeat). Otherwise take the next slot.
  Toast* last = &g_toasts[(g_toast_head + GTAV_TOAST_SLOTS - 1u) % GTAV_TOAST_SLOTS];
  if (last->active && g_last_worker_tick < last->expire_tick && strcmp(last->text, full) == 0) {
    if (last->repeat < 0xffffu) last->repeat++;
    toast_color(result_code, &last->r, &last->g, &last->b);
    last->birth_tick = g_last_worker_tick;
    last->expire_tick = g_last_worker_tick + g_toast_ticks;
    return;
  }
  Toast* t = &g_toasts[g_toast_head];
  g_toast_head = (g_toast_head + 1u) % GTAV_TOAST_SLOTS;  // overwrite oldest
  snprintf(t->text, sizeof(t->text), "%s", full);         // truncates safely
  toast_color(result_code, &t->r, &t->g, &t->b);
  t->birth_tick = g_last_worker_tick;
  t->expire_tick = g_last_worker_tick + g_toast_ticks;
  t->repeat = 1u;
  t->active = 1;
#else
  (void)text;
  (void)result_code;
#endif
}

#ifndef GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
#define GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST 0
#endif

static_assert(sizeof(GtavNativeShellSnapshot) == 112, "GtavNativeShellSnapshot size changed");
static_assert(sizeof(GtavNativeShellState) == GTAV_NATIVE_SHELL_STATE_SIZE,
              "GtavNativeShellState size changed");

extern "C" {
GtavNativeShellState gtav_native_shell_state __attribute__((used, visibility("default"))) = {
    GTAV_NATIVE_SHELL_STATE_MAGIC,
    GTAV_NATIVE_BRIDGE_ABI_VERSION,
    sizeof(GtavNativeShellState),
    {},
};

const char gtav_native_shell_state_marker[] __attribute__((used)) = "GTAV_NATIVE_SHELL_STATE_V1";
}

// Menu colour theme + draw region. Defined at file scope (not in the anon namespace below)
// so the extern "C" customisation accessors after that namespace can reach this state too.
// The chrome/accent colours were inline literals in the draw path; they are now pulled from
// the active theme so the user can recolour the menu. Theme 0 reproduces the original
// "Default Blue" values byte-for-byte, so the out-of-box look is unchanged. Only the chrome +
// accent + selected-label colour vary by theme; the semantic value colours (toggle ON=green /
// OFF=red, locked=red, list=cyan) and the title/default-label text stay constant for
// readability.
namespace {
struct MenuTheme {
  char name[12];            // inline (no relocation; see feature_catalog.h)
  uint8_t panel[4];         // main panel fill
  uint8_t accent[4];        // header bar
  uint8_t selection[4];     // selected-row highlight
  uint8_t footer[4];        // footer strip
  uint8_t scroll_thumb[4];  // scrollbar thumb
  uint8_t sel_label[3];     // selected-row label text
  // Semantic value colours. These were inline literals in draw_menu_row; promoting them into the
  // theme lets a palette tune the most readable elements (status text), not just the chrome. The
  // four shipping themes fill these with the original literals so their look is byte-identical;
  // only the High-Contrast theme overrides them. The submenu "OPEN" colour still derives from
  // accent, and the neutral ACT/STOP default + off-white label stay constant for consistency.
  uint8_t toggle_on[3];     // toggle value text when ON
  uint8_t toggle_off[3];    // toggle value text when OFF
  uint8_t value_list[3];    // list "< value >" text
  uint8_t value_locked[3];  // locked / unavailable value text
};

// Semantic value colours shared by the four original themes (the exact literals draw_menu_row
// used inline before they were promoted into the theme): toggle ON green, OFF red, list cyan,
// locked red. Kept in named macros so the four themes stay byte-identical and obviously so.
#define GTAV_THEME_VALUE_COLORS {120, 205, 95}, {205, 95, 95}, {120, 210, 240}, {210, 80, 80}
const MenuTheme kThemes[] = {
    {"Blue",
     {12, 14, 20, 220},
     {28, 86, 150, 235},
     {60, 120, 200, 150},
     {20, 40, 66, 225},
     {210, 225, 255, 240},
     {255, 238, 150},
     GTAV_THEME_VALUE_COLORS},
    {"Crimson",
     {20, 12, 14, 220},
     {150, 40, 52, 235},
     {200, 70, 80, 150},
     {66, 22, 28, 225},
     {255, 210, 215, 240},
     {255, 200, 150},
     GTAV_THEME_VALUE_COLORS},
    {"Emerald",
     {12, 20, 14, 220},
     {36, 130, 72, 235},
     {70, 190, 110, 150},
     {22, 60, 38, 225},
     {215, 255, 225, 240},
     {220, 255, 180},
     GTAV_THEME_VALUE_COLORS},
    {"Mono",
     {16, 16, 18, 225},
     {90, 94, 104, 235},
     {130, 134, 144, 150},
     {44, 46, 52, 225},
     {225, 228, 235, 240},
     {240, 240, 245},
     GTAV_THEME_VALUE_COLORS},
    // High-Contrast: a near-opaque black panel, bright amber chrome and white labels for
    // legibility over bright scenes / for low-vision use. Brightened semantic colours too.
    // Demonstrates the now-themeable status colours; the Theme cycler picks it up by count.
    {"Contrast",
     {0, 0, 0, 240},
     {255, 200, 0, 255},
     {255, 200, 0, 170},
     {18, 18, 18, 245},
     {255, 255, 255, 245},
     {255, 255, 255},
     {90, 255, 120},
     {255, 95, 95},
     {120, 235, 255},
     {255, 110, 110}},
    // Colourblind-safe: the stock themes encode toggle ON/OFF as green/red, which deuteran/protan
    // vision cannot separate. This palette swaps the semantic colours to a blue/orange pair (safe
    // across the common types) so ON vs OFF differs by hue AND brightness; the bracketed "[ ON ]"/
    // "[ OFF ]" word and the locked [L] token (draw_menu_row) add shape redundancy on top. Neutral
    // blue chrome.
    {"CB-Safe",
     {12, 14, 20, 224},
     {40, 110, 190, 235},
     {70, 130, 210, 150},
     {20, 38, 62, 225},
     {220, 232, 255, 240},
     {255, 235, 150},
     {90, 165, 240},
     {240, 150, 40},
     {120, 210, 240},
     {240, 150, 40}},
};
#undef GTAV_THEME_VALUE_COLORS
const uint32_t kThemeCount = (uint32_t)(sizeof(kThemes) / sizeof(kThemes[0]));

// Draw region: the whole panel anchors off one x. RIGHT (default) keeps the original 0.75;
// LEFT mirrors it to the screen's left edge. Everything downstream derives from text_x.
enum { GTAV_MENU_REGION_RIGHT = 0, GTAV_MENU_REGION_LEFT = 1 };
const char* const kRegionNames[] = {"Right", "Left"};
const uint32_t kRegionCount = 2u;
const float GTAV_MENU_REGION_LEFT_X = 0.045f;  // left-side panel anchor
const float GTAV_MENU_REGION_RIGHT_X =
    0.64f;  // right-side panel anchor (nudged left so the wider panel keeps its right-edge margin)

uint32_t g_theme_index = 0u;
uint32_t g_region_index = GTAV_MENU_REGION_RIGHT;

// Menu panel width (Narrow/Normal/Wide). Bounded so the right-anchored panel keeps its on-screen
// right margin (widest right edge ~= REGION_RIGHT_X 0.64 + 0.345 < 1.0). Normal (index 1) = 0.305
// reproduces today's look exactly; the default GTAV_MENU_PANEL_W stays as documentation of it.
const float kPanelWidths[] = {0.265f, 0.305f, 0.345f};
const char* const kPanelWidthNames[] = {"Narrow", "Normal", "Wide"};
const uint32_t kPanelWidthCount = 3u;
uint32_t g_panel_width_index = 1u;  // default Normal

// Mutable "Custom" theme slot, appended after the const kThemes[] as the synthetic index
// kThemeCount. Seeded from the default (Blue) palette on first use so it starts looking like the
// stock menu; the in-menu Theme Editor (CYCLE_CUSTOM_COLOR rows) edits its accent/selection/panel
// RGB. Only those three chrome colours are user-editable + persisted; the rest stay at the seed.
MenuTheme g_custom_theme;
int g_custom_theme_seeded = 0;
const uint32_t kThemeSelectableCount = kThemeCount + 1u;  // const presets + the one Custom slot

void ensure_custom_theme_seeded() {
  if (!g_custom_theme_seeded) {
    g_custom_theme = kThemes[0];
    g_custom_theme_seeded = 1;
  }
}

[[maybe_unused]] const MenuTheme& active_theme() {
  if (g_theme_index >= kThemeCount) {
    ensure_custom_theme_seeded();
    return g_custom_theme;
  }
  return kThemes[g_theme_index % kThemeCount];
}

// Map a Theme-Editor channel to a writable RGB byte in g_custom_theme. Each group is one MenuTheme
// colour and contributes 3 channels (R,G,B); the editable groups, in channel order, are:
//   0-2 accent, 3-5 selection, 6-8 panel  (the original chrome groups, on the Theme Editor page)
//   9-11 footer, 12-14 scrollbar, 15-17 selected-label, 18-20 toggle-ON, 21-23 toggle-OFF,
//   24-26 list-value, 27-29 locked-value  (the rest, on the "More Colours" sub-page)
// Alphas are never editable so the panel can't be made invisible. comp 0-2 indexes R/G/B (the [4]
// groups carry alpha at [3], left untouched).
uint8_t* custom_color_channel(uint32_t channel) {
  ensure_custom_theme_seeded();
  const uint32_t group = channel / 3u;
  const uint32_t comp = channel % 3u;
  uint8_t* base = nullptr;
  switch (group) {
    case 0u:
      base = g_custom_theme.accent;
      break;
    case 1u:
      base = g_custom_theme.selection;
      break;
    case 2u:
      base = g_custom_theme.panel;
      break;
    case 3u:
      base = g_custom_theme.footer;
      break;
    case 4u:
      base = g_custom_theme.scroll_thumb;
      break;
    case 5u:
      base = g_custom_theme.sel_label;
      break;
    case 6u:
      base = g_custom_theme.toggle_on;
      break;
    case 7u:
      base = g_custom_theme.toggle_off;
      break;
    case 8u:
      base = g_custom_theme.value_list;
      break;
    case 9u:
      base = g_custom_theme.value_locked;
      break;
    default:
      return nullptr;
  }
  return base + comp;
}
[[maybe_unused]] float menu_panel_text_x() {
  return g_region_index == GTAV_MENU_REGION_LEFT ? GTAV_MENU_REGION_LEFT_X
                                                 : GTAV_MENU_REGION_RIGHT_X;
}
[[maybe_unused]] float menu_panel_width() {
  return kPanelWidths[g_panel_width_index % kPanelWidthCount];
}
}  // namespace

namespace {

#include "shell_menu_defs.hpp"

// The Vehicles root holds the maintenance toggles plus a "Vehicle Spawner" submenu;
// the (large) spawn list lives in that submenu so it doesn't clutter the root. The
// spawner's first row is a "Vehicle Class" cycler and the rest are one spawn row per
// catalog entry MATCHING the selected class -- rebuilt whenever the class changes.
#ifndef GTAV_MENU_VEHICLE_ROWS_MAX
#define GTAV_MENU_VEHICLE_ROWS_MAX 1024u
#endif
static const uint32_t kVehicleRootRows = 27;  // utilities + handling modes + spawner defaults +
                                              // delete + Customs + Browser + Autopilot + Controls +
                                              // Save/Spawn Saved Vehicle + Fly + Slippery + Rocket
                                              // (Cruise Control/Speed moved under Autopilot)
static const uint32_t kSpawnerItemsCap = 1u + GTAV_MENU_VEHICLE_ROWS_MAX;
// Plain statics. In the PRX build -fno-zero-initialized-in-bss places these in
// file-backed .data (the PRX loader only maps ~4 KB of BSS, so large NOBITS arrays would
// be unmapped and fault -- hardware 2026-06-15); the ps5debug build maps the whole payload.
static ShellItem g_vehicle_items[kVehicleRootRows];
static ShellItem g_spawner_items[1 + GTAV_MENU_VEHICLE_ROWS_MAX];
static uint32_t g_spawner_item_count;
// Selected vehicle-class filter: 0 = All; 1..22 = GTA V vehicle class 0..21.
static uint32_t g_spawner_class_index;

// Class names indexed so that [0] = "All" and [n] = GTA V class (n-1). Order matches
// the game's GET_VEHICLE_CLASS ids 0..21.
// Inline char arrays (not const char*) so no relocation is needed (the etaHEN fake-self
// PRX loader does not reliably apply R_X86_64_RELATIVE). See feature_catalog.h.
// The last two entries are synthetic filters (Favorites/Recents), not GET_VEHICLE_CLASS ids;
// spawner_class_filter() and rebuild_spawner_items() special-case them. Keep them last so the
// real class ids stay 1..22 (index = class + 1).
static const char kVehicleClassNames[][20] = {
    "All",      "Compacts",        "Sedans",  "SUVs",      "Coupes",
    "Muscle",   "Sports Classics", "Sports",  "Super",     "Motorcycles",
    "Off-Road", "Industrial",      "Utility", "Vans",      "Cycles",
    "Boats",    "Helicopters",     "Planes",  "Service",   "Emergency",
    "Military", "Commercial",      "Trains",  "Favorites", "Recents",
};

// Selected ped-browser category filter: 0 = All; 1..N = ped category (n-1). Shared by the
// skin changer, spawn-ped and spawn-bodyguard menus (they all browse the one ped catalog).
static uint32_t g_ped_category_index;

// Ped category names indexed so [0] = "All" and [n] = ped category (n-1). The entries after
// "All" come from ped_categories_generated.h in catalog category-index order, so a catalog
// row with category index N maps to kPedCategoryNames[N + 1]. Inline char arrays (not const
// char*) so no relocation is needed (the etaHEN fake-self PRX loader does not reliably apply
// R_X86_64_RELATIVE). See feature_catalog.h / devtools/generators/generate_ped_registry.py.
static const char kPedCategoryNames[][24] = {
    "All",
#include "gtavmenu/ped_categories_generated.h"
    // Synthetic filters appended after the generated categories (kept last, like the vehicle
    // class names): rebuild_ped_picker() special-cases these two.
    "Favorites",
    "Recents",
};

// Runtime menu table; the vehicles + spawner slots point at the arrays above.
static ShellMenu g_menus[SHELL_MENU_COUNT];

static int spawner_class_filter() {
  return (g_spawner_class_index == 0) ? -1 : (int)(g_spawner_class_index - 1);
}

static int ped_category_filter() {
  return (g_ped_category_index == 0) ? -1 : (int)(g_ped_category_index - 1);
}

// ---- Browser favorites / recents (Wave 2) ------------------------------------------------
// The vehicle and ped browsers each expose two synthetic filter categories (the last two
// cycler positions): "Favorites" (user-pinned via R3 on a spawn row) and "Recents"
// (auto, most-recent-first, filled when a spawn actually runs). Both are model-hash lists
// persisted in the feature profile (v7) and bridged in/out by features.cpp like theme/region.
// Pure worker-side menu bookkeeping -- no game-thread natives involved.
static uint32_t g_fav_vehicles[GTAV_FAVORITE_VEHICLES_MAX];
static uint32_t g_fav_peds[GTAV_FAVORITE_PEDS_MAX];
static uint32_t g_recent_vehicles[GTAV_RECENT_VEHICLES_MAX];
static uint32_t g_recent_peds[GTAV_RECENT_PEDS_MAX];

static const uint32_t kVehFilterCount =
    (uint32_t)(sizeof(kVehicleClassNames) / sizeof(kVehicleClassNames[0]));
static const uint32_t kVehFilterFavorites = kVehFilterCount - 2u;
static const uint32_t kVehFilterRecents = kVehFilterCount - 1u;
static const uint32_t kPedFilterCount =
    (uint32_t)(sizeof(kPedCategoryNames) / sizeof(kPedCategoryNames[0]));
static const uint32_t kPedFilterFavorites = kPedFilterCount - 2u;
static const uint32_t kPedFilterRecents = kPedFilterCount - 1u;

static int hashlist_contains(const uint32_t* a, uint32_t n, uint32_t h) {
  if (!h) return 0;
  for (uint32_t i = 0; i < n; ++i)
    if (a[i] == h) return 1;
  return 0;
}

// Toggle membership: drop if present (compacting the tail), else add into the first empty slot.
// Returns 1 if the hash is now pinned, 0 if it was removed or the list is full (40 is generous).
static int hashlist_toggle(uint32_t* a, uint32_t n, uint32_t h) {
  if (!h) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (a[i] == h) {
      for (uint32_t j = i; j + 1u < n; ++j) a[j] = a[j + 1u];
      a[n - 1u] = 0;
      return 0;
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (a[i] == 0) {
      a[i] = h;
      return 1;
    }
  }
  return 0;
}

// Most-recently-used insert: move h to the front, shifting the rest down, stopping at the old
// copy or the first empty slot -- so there are never duplicates and the oldest falls off the end.
static void recents_push(uint32_t* a, uint32_t n, uint32_t h) {
  if (!h) return;
  uint32_t prev = h;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t cur = a[i];
    a[i] = prev;
    if (cur == h || cur == 0) return;
    prev = cur;
  }
}

static int vehicle_catalog_index_for_hash(uint32_t h) {
  for (uint32_t i = 0; i < gtav_vehicle_catalog_count; ++i)
    if (gtav_vehicle_catalog[i].model_hash == h) return (int)i;
  return -1;
}

static int ped_catalog_index_for_hash(uint32_t h) {
  for (uint32_t i = 0; i < gtav_ped_model_catalog_count; ++i)
    if (gtav_ped_model_catalog[i].hash == h) return (int)i;
  return -1;
}

// ---- Global "Quick" favorites (v10) ------------------------------------------------------
// A single top-level menu that gathers user-pinned action/toggle rows from anywhere in the menu
// tree, so the rows you reach for most are one hop away. Unlike the browser favorites (which key
// on a model hash), a Quick pin identifies ANY row generically by its dispatch (action, param):
// the same key the activation path already routes on. Pins are stored as parallel arrays
// (action 0 == empty slot) persisted in the feature profile and bridged in/out by features.cpp
// like the browser favorites. Pure worker-side menu bookkeeping -- no game-thread natives.
static uint32_t g_quick_action[GTAV_QUICK_PINS_MAX];
static uint32_t g_quick_param[GTAV_QUICK_PINS_MAX];

// Toggle a (action, param) pin: drop if present (compacting the tail), else add into the first
// empty slot. Returns 1 if the row is now pinned, 0 if it was removed or the list is full.
static int quick_toggle(uint32_t action, uint32_t param) {
  if (action == GTAV_NATIVE_SHELL_ACTION_NONE) return 0;
  for (uint32_t i = 0; i < GTAV_QUICK_PINS_MAX; ++i) {
    if (g_quick_action[i] == action && g_quick_param[i] == param) {
      for (uint32_t j = i; j + 1u < GTAV_QUICK_PINS_MAX; ++j) {
        g_quick_action[j] = g_quick_action[j + 1u];
        g_quick_param[j] = g_quick_param[j + 1u];
      }
      g_quick_action[GTAV_QUICK_PINS_MAX - 1u] = 0;
      g_quick_param[GTAV_QUICK_PINS_MAX - 1u] = 0;
      return 0;
    }
  }
  for (uint32_t i = 0; i < GTAV_QUICK_PINS_MAX; ++i) {
    if (g_quick_action[i] == GTAV_NATIVE_SHELL_ACTION_NONE) {
      g_quick_action[i] = action;
      g_quick_param[i] = param;
      return 1;
    }
  }
  return 0;  // full (16 pins is generous for a console UI)
}

static int vehicle_label_is_duplicate(uint32_t index) {
  if (index >= gtav_vehicle_catalog_count) return 0;
  const char* label = gtav_vehicle_catalog[index].label;
  for (uint32_t i = 0; i < gtav_vehicle_catalog_count; ++i) {
    if (i != index && !strcmp(gtav_vehicle_catalog[i].label, label)) return 1;
  }
  return 0;
}

// Rebuild the spawner rows for the current class filter: row 0 is the class cycler,
// then the spawn rows whose vehicle class matches (all of them when filter == All).
// Each spawn row carries the model hash in .param; the hook-side spawn queue treats
// SPAWN_VEHICLE params as hashes, not catalog indices.
// Fill one vehicle spawn row from catalog entry `i`. A leading "* " marks a favorite (ASCII so
// it renders in the bitmap font). The precision caps keep "* label [model]" within label[48]
// (2 + 26 + 16 + 3 < 48; observed maxima label 25, model 14).
static void fill_vehicle_spawn_row(ShellItem* item, uint32_t i, int favorited) {
  const char* mark = favorited ? "* " : "";
  if (vehicle_label_is_duplicate(i)) {
    snprintf(item->label, sizeof(item->label), "%s%.26s [%.16s]", mark,
             gtav_vehicle_catalog[i].label, gtav_vehicle_catalog[i].model);
  } else {
    snprintf(item->label, sizeof(item->label), "%s%s", mark, gtav_vehicle_catalog[i].label);
  }
  item->type = SHELL_ROW_ACTION;
  item->submenu = SHELL_MENU_NONE;
  item->action = GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE;
  item->unavailable_reason = "Needs main-thread hook";
  item->param = gtav_vehicle_catalog[i].model_hash;
}

static void rebuild_spawner_items() {
  NB_TRACE("rs: A entered");
  NB_TRACE("rs: B cat_count=%u", gtav_vehicle_catalog_count);
  g_spawner_items[0] = {"Vehicle Class", SHELL_ROW_LIST,
                        SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_CLASS,
                        nullptr,         0};
  NB_TRACE("rs: C item0 written");
  uint32_t count = 1;
  uint32_t max_rows = kSpawnerItemsCap;
  if (g_spawner_class_index == kVehFilterFavorites || g_spawner_class_index == kVehFilterRecents) {
    // Synthetic Favorites/Recents views: iterate the saved hash list (favorites in pin order,
    // recents most-recent-first) and resolve each hash back to its catalog row.
    const int fav_view = (g_spawner_class_index == kVehFilterFavorites);
    const uint32_t* list = fav_view ? g_fav_vehicles : g_recent_vehicles;
    const uint32_t list_n = fav_view ? GTAV_FAVORITE_VEHICLES_MAX : GTAV_RECENT_VEHICLES_MAX;
    for (uint32_t k = 0; k < list_n && count < max_rows; ++k) {
      if (!list[k]) continue;
      int idx = vehicle_catalog_index_for_hash(list[k]);
      if (idx < 0) continue;
      fill_vehicle_spawn_row(&g_spawner_items[count++], (uint32_t)idx, fav_view);
    }
  } else {
    int filter = spawner_class_filter();
    NB_TRACE("rs: D filter=%d; loop...", filter);
    for (uint32_t i = 0; i < gtav_vehicle_catalog_count && count < max_rows; ++i) {
      if (filter >= 0) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
        if (gtav_features_vehicle_class(i) != filter) continue;
#else
        continue;  // no class data without the feature layer: specific classes are empty
#endif
      }
      int fav = hashlist_contains(g_fav_vehicles, GTAV_FAVORITE_VEHICLES_MAX,
                                  gtav_vehicle_catalog[i].model_hash);
      fill_vehicle_spawn_row(&g_spawner_items[count++], i, fav);
    }
  }
  g_spawner_item_count = count;
  g_menus[SHELL_MENU_VEHICLE_SPAWNER] = {"Vehicle Browser", g_spawner_items, g_spawner_item_count};
  NB_TRACE("rs: E loop done count=%u", count);
}

// Weapon browser: one give-and-equip row per weapon catalog entry. Mirrors the vehicle
// spawner but with no class filter; .param carries the precomputed weapon joaat hash,
// which the game-thread give path consumes directly (same command lane as SPAWN_VEHICLE's
// model hash). Built once in build_menus(); the catalog never changes at runtime.
// Sized above the full weapon catalog (~100 rows) so the browser never silently truncates the
// roster; the backing array is the only cost (a handful of KB of ShellItem).
#ifndef GTAV_MENU_WEAPON_ROWS_MAX
#define GTAV_MENU_WEAPON_ROWS_MAX 112u
#endif
static ShellItem g_weapon_picker_items[GTAV_MENU_WEAPON_ROWS_MAX];
static uint32_t g_weapon_picker_item_count;

static void build_weapon_picker_items() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < gtav_weapon_catalog_count && count < GTAV_MENU_WEAPON_ROWS_MAX; ++i) {
    if (!gtav_weapon_catalog[i].hash) continue;
    ShellItem* item = &g_weapon_picker_items[count++];
    // Strip the "WEAPON_" prefix for a cleaner label; inline char array (no relocation).
    const char* name = gtav_weapon_catalog[i].name;
    const char* shortn = (strncmp(name, "WEAPON_", 7) == 0) ? name + 7 : name;
    snprintf(item->label, sizeof(item->label), "%.40s", shortn);
    item->type = SHELL_ROW_ACTION;
    item->submenu = SHELL_MENU_NONE;
    item->action = GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON;
    item->unavailable_reason = "Needs main-thread hook";
    item->param = gtav_weapon_catalog[i].hash;
  }
  g_weapon_picker_item_count = count;
  g_menus[SHELL_MENU_WEAPON_PICKER] = {"Weapon Browser", g_weapon_picker_items,
                                       g_weapon_picker_item_count};
}

// Emote picker: row 0 is Stop Emote, row 1 is the Loop Emote toggle (loop vs play-once), then one
// PLAY_EMOTE row per catalog anim (.param = the gtav_anim_catalog index, read by the game-thread
// emote tick). Flat (no category), like the weapon picker; built once. Rows are NOT statically
// locked -- PLAY_EMOTE self-refuses until the frame hook is live (like the minigame toggles), so it
// dispatches and reports a toast; Loop Emote is a pure flag flip that works in any state.
#ifndef GTAV_MENU_EMOTE_ROWS_MAX
#define GTAV_MENU_EMOTE_ROWS_MAX 200u
#endif
static ShellItem g_emote_picker_items[2 + GTAV_MENU_EMOTE_ROWS_MAX];
static uint32_t g_emote_picker_item_count;

static void build_emote_picker_items() {
  g_emote_picker_items[0] = {"Stop Emote",    SHELL_ROW_ACTION,
                             SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_STOP_EMOTE,
                             nullptr,         0};
  g_emote_picker_items[1] = {"Loop Emote",    SHELL_ROW_TOGGLE,
                             SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_EMOTE_LOOP,
                             nullptr,         0};
  uint32_t count = 2;
  for (uint32_t i = 0; i < gtav_anim_catalog_count && count < 2u + GTAV_MENU_EMOTE_ROWS_MAX; ++i) {
    ShellItem* item = &g_emote_picker_items[count++];
    snprintf(item->label, sizeof(item->label), "%.40s", gtav_anim_catalog[i].label);
    item->type = SHELL_ROW_ACTION;
    item->submenu = SHELL_MENU_NONE;
    item->action = GTAV_NATIVE_SHELL_ACTION_PLAY_EMOTE;
    item->unavailable_reason = nullptr;
    item->param = i;  // catalog index, read back on the game-thread emote tick
  }
  g_emote_picker_item_count = count;
  g_menus[SHELL_MENU_EMOTES] = {"Emotes", g_emote_picker_items, g_emote_picker_item_count};
}

// Ped browsers (skin changer / spawn ped / spawn bodyguard) all browse the one ped-model
// catalog (~1.1k entries from the Menyoo dump). Row 0 is the shared category cycler; the
// remaining rows are the catalog peds whose category matches the filter (all of them when
// filter == All). .param carries the precomputed model joaat hash, routed game-thread like
// SPAWN_VEHICLE. The cap holds the cycler row + every ped, so "All" lists the whole catalog.
#ifndef GTAV_MENU_PED_ROWS_MAX
#define GTAV_MENU_PED_ROWS_MAX 1200u
#endif
static const uint32_t kPedPickerItemsCap = 1u + GTAV_MENU_PED_ROWS_MAX;
static ShellItem g_skin_picker_items[1 + GTAV_MENU_PED_ROWS_MAX];
static uint32_t g_skin_picker_item_count;
static ShellItem g_ped_spawner_items[1 + GTAV_MENU_PED_ROWS_MAX];
static uint32_t g_ped_spawner_item_count;
static ShellItem g_bodyguard_spawner_items[1 + GTAV_MENU_PED_ROWS_MAX];
static uint32_t g_bodyguard_spawner_item_count;

// Rebuild a ped picker for the current category filter: row 0 is the category cycler, then
// one action row per matching catalog ped. row_action selects what activating a row does
// (SET_PLAYER_MODEL / SPAWN_PED / SPAWN_BODYGUARD). Game-thread gated (LOCK until the hook is
// live), like the vehicle browser.
// Fill one ped spawn row from catalog entry `i`. A leading "* " marks a favorite. label[48]
// holds "* " + the caption (label[40]) comfortably (2 + 40 < 48).
static void fill_ped_spawn_row(ShellItem* item, uint32_t i, uint32_t row_action, int favorited) {
  snprintf(item->label, sizeof(item->label), "%s%.40s", favorited ? "* " : "",
           gtav_ped_model_catalog[i].label);
  item->type = SHELL_ROW_ACTION;
  item->submenu = SHELL_MENU_NONE;
  item->action = row_action;
  item->unavailable_reason = "Needs main-thread hook";
  item->param = gtav_ped_model_catalog[i].hash;
}

static void rebuild_ped_picker(uint32_t menu_id, ShellItem* items, uint32_t* out_count,
                               uint32_t row_action, const char* title) {
  items[0] = {"Ped Category",  SHELL_ROW_LIST,
              SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_PED_CATEGORY,
              nullptr,         0};
  uint32_t count = 1;
  if (g_ped_category_index == kPedFilterFavorites || g_ped_category_index == kPedFilterRecents) {
    // Favorites/Recents are shared across the skin/spawn-ped/bodyguard browsers (one ped-category
    // index). Iterate the saved hash list and resolve each hash back to its catalog row.
    const int fav_view = (g_ped_category_index == kPedFilterFavorites);
    const uint32_t* list = fav_view ? g_fav_peds : g_recent_peds;
    const uint32_t list_n = fav_view ? GTAV_FAVORITE_PEDS_MAX : GTAV_RECENT_PEDS_MAX;
    for (uint32_t k = 0; k < list_n && count < kPedPickerItemsCap; ++k) {
      if (!list[k]) continue;
      int idx = ped_catalog_index_for_hash(list[k]);
      if (idx < 0) continue;
      fill_ped_spawn_row(&items[count++], (uint32_t)idx, row_action, fav_view);
    }
  } else {
    int filter = ped_category_filter();
    for (uint32_t i = 0; i < gtav_ped_model_catalog_count && count < kPedPickerItemsCap; ++i) {
      if (!gtav_ped_model_catalog[i].hash) continue;
      if (filter >= 0 && (int)gtav_ped_model_catalog[i].category != filter) continue;
      int fav =
          hashlist_contains(g_fav_peds, GTAV_FAVORITE_PEDS_MAX, gtav_ped_model_catalog[i].hash);
      fill_ped_spawn_row(&items[count++], i, row_action, fav);
    }
  }
  *out_count = count;
  g_menus[menu_id] = {title, items, count};
}

static void build_skin_picker_items() {
  rebuild_ped_picker(SHELL_MENU_SKIN_PICKER, g_skin_picker_items, &g_skin_picker_item_count,
                     GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL, "Skin Changer");
}

static void build_ped_spawner_items() {
  rebuild_ped_picker(SHELL_MENU_PED_SPAWNER, g_ped_spawner_items, &g_ped_spawner_item_count,
                     GTAV_NATIVE_SHELL_ACTION_SPAWN_PED, "Spawn Ped");
}

static void build_bodyguard_spawner_items() {
  rebuild_ped_picker(SHELL_MENU_BODYGUARD_SPAWNER, g_bodyguard_spawner_items,
                     &g_bodyguard_spawner_item_count, GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD,
                     "Spawn Bodyguard");
}

// Re-filter all three ped browsers to the current category and reset their cursors. Called
// when the shared ped-category cycler steps.
static void rebuild_ped_pickers() {
  build_skin_picker_items();
  build_ped_spawner_items();
  build_bodyguard_spawner_items();
}

// Selected object-browser category filter: 0 = All; 1..N = object category (n-1).
static uint32_t g_object_category_index;

// Object category names indexed so [0] = "All" and [n] = object category (n-1), from
// object_categories_generated.h (catalog category-index order: a row with category index N maps
// to kObjectCategoryNames[N + 1]). Inline char arrays (no relocation), as for kPedCategoryNames.
static const char kObjectCategoryNames[][24] = {
    "All",
#include "gtavmenu/object_categories_generated.h"
};

static int object_category_filter() {
  return (g_object_category_index == 0) ? -1 : (int)(g_object_category_index - 1);
}

#ifndef GTAV_MENU_OBJECT_SPAWN_ROWS_MAX
// Holds the whole catalog under the "All" filter (90 today), so no category page truncates.
#define GTAV_MENU_OBJECT_SPAWN_ROWS_MAX 128u
#endif
// Fixed header rows above the object list: the category filter, the four spawn-placement
// cyclers, the auto-edit-on-spawn toggle, and the "move last object" interactive-placement row.
static const uint32_t kObjectHeaderRows = 7u;
static const uint32_t kObjectSpawnerItemsCap = kObjectHeaderRows + GTAV_MENU_OBJECT_SPAWN_ROWS_MAX;
static ShellItem g_object_spawner_items[kObjectSpawnerItemsCap];
static uint32_t g_object_spawner_item_count;

// Rebuild the object spawner for the current category filter: the category cycler + the four
// placement cyclers (Spawn At / Distance / Heading / Place on Ground), then one SPAWN_OBJECT row
// per matching catalog prop. Game-thread gated (LOCK until the hook is live), like the vehicle /
// ped browsers. Called at build time and whenever the category cycler steps.
static void build_object_spawner_items() {
  g_object_spawner_items[0] = {"Object Category", SHELL_ROW_LIST,
                               SHELL_MENU_NONE,   GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_CATEGORY,
                               nullptr,           0};
  g_object_spawner_items[1] = {"Spawn At",      SHELL_ROW_LIST,
                               SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_SPAWN_AT,
                               nullptr,         0};
  g_object_spawner_items[2] = {"Distance",      SHELL_ROW_LIST,
                               SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_DISTANCE,
                               nullptr,         0};
  g_object_spawner_items[3] = {"Heading",       SHELL_ROW_LIST,
                               SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_HEADING,
                               nullptr,         0};
  g_object_spawner_items[4] = {"Place on Ground", SHELL_ROW_LIST,
                               SHELL_MENU_NONE,   GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_ON_GROUND,
                               nullptr,           0};
  // Interactive placement: a saved toggle to auto-enter move mode after a spawn, plus a row to
  // re-enter it for the most-recent object. The move row is worker-direct (it only flips
  // transient flags); it refuses with a toast until the frame hook is live, so it is not
  // LOCK-gated.
  g_object_spawner_items[5] = {"Auto-edit on Spawn",
                               SHELL_ROW_TOGGLE,
                               SHELL_MENU_NONE,
                               GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_EDIT_OBJECT,
                               nullptr,
                               0};
  g_object_spawner_items[6] = {"Move Last Object",
                               SHELL_ROW_ACTION,
                               SHELL_MENU_NONE,
                               GTAV_NATIVE_SHELL_ACTION_MOVE_LAST_OBJECT,
                               nullptr,
                               0};
  uint32_t count = kObjectHeaderRows;
  int filter = object_category_filter();
  for (uint32_t i = 0; i < gtav_object_catalog_count && count < kObjectSpawnerItemsCap; ++i) {
    if (!gtav_object_catalog[i].hash) continue;
    if (filter >= 0 && (int)gtav_object_catalog[i].category != filter) continue;
    ShellItem* item = &g_object_spawner_items[count++];
    snprintf(item->label, sizeof(item->label), "%.40s", gtav_object_catalog[i].label);
    item->type = SHELL_ROW_ACTION;
    item->submenu = SHELL_MENU_NONE;
    item->action = GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT;
    item->unavailable_reason = "Needs main-thread hook";
    item->param = gtav_object_catalog[i].hash;
  }
  g_object_spawner_item_count = count;
  g_menus[SHELL_MENU_OBJECT_SPAWNER] = {"Spawn Object", g_object_spawner_items,
                                        g_object_spawner_item_count};
}

// Selected scenario-browser category filter: 0 = All; 1..N = scenario category (n-1).
static uint32_t g_scenario_category_index;

// Scenario category names indexed so [0] = "All" and [n] = scenario category (n-1), from
// scenario_categories_generated.h (catalog category-index order: a row with category index N maps
// to kScenarioCategoryNames[N + 1]). Inline char arrays (no relocation), as for kPedCategoryNames.
static const char kScenarioCategoryNames[][24] = {
    "All",
#include "gtavmenu/scenario_categories_generated.h"
};

static int scenario_category_filter() {
  return (g_scenario_category_index == 0) ? -1 : (int)(g_scenario_category_index - 1);
}

#ifndef GTAV_MENU_SCENARIO_ROWS_MAX
// Holds the whole catalog under the "All" filter (89 today), so no category page truncates.
#define GTAV_MENU_SCENARIO_ROWS_MAX 256u
#endif
// Fixed header row above the scenario list: the category filter cycler.
static const uint32_t kScenarioHeaderRows = 1u;
static const uint32_t kScenarioPickerItemsCap = kScenarioHeaderRows + GTAV_MENU_SCENARIO_ROWS_MAX;
static ShellItem g_scenario_picker_items[kScenarioPickerItemsCap];
static uint32_t g_scenario_picker_item_count;

// Rebuild the scenario picker for the current category filter: row 0 is the category cycler, then
// one START_SCENARIO row per matching catalog scenario. Each row carries its *catalog index* in
// .param (not a hash, unlike the spawn browsers): the game-thread handler reads
// gtav_scenario_catalog[param].name and passes it to TASK_START_SCENARIO_IN_PLACE. Game-thread
// gated (LOCK until the hook is live), like the object / ped browsers. Called at build time and
// whenever the category cycler steps.
static void rebuild_scenario_picker() {
  g_scenario_picker_items[0] = {"Scenario Category",
                                SHELL_ROW_LIST,
                                SHELL_MENU_NONE,
                                GTAV_NATIVE_SHELL_ACTION_CYCLE_SCENARIO_CATEGORY,
                                nullptr,
                                0};
  uint32_t count = kScenarioHeaderRows;
  int filter = scenario_category_filter();
  for (uint32_t i = 0; i < gtav_scenario_catalog_count && count < kScenarioPickerItemsCap; ++i) {
    if (!gtav_scenario_catalog[i].name[0]) continue;
    if (filter >= 0 && (int)gtav_scenario_catalog[i].category != filter) continue;
    ShellItem* item = &g_scenario_picker_items[count++];
    snprintf(item->label, sizeof(item->label), "%.40s", gtav_scenario_catalog[i].label);
    item->type = SHELL_ROW_ACTION;
    item->submenu = SHELL_MENU_NONE;
    item->action = GTAV_NATIVE_SHELL_ACTION_START_SCENARIO;
    item->unavailable_reason = "Needs main-thread hook";
    item->param = i;  // catalog index, read back on the game thread
  }
  g_scenario_picker_item_count = count;
  g_menus[SHELL_MENU_SELF_SCENARIO_PICKER] = {"Scenarios", g_scenario_picker_items,
                                              g_scenario_picker_item_count};
}

static void build_vehicle_items() {
  // Layout convention (same as the static menus): submenu rows first, then the leaf rows grouped by
  // purpose. Submenus: Browser (spawn) -> Customs -> Controls -> Autopilot. Leaf groups:
  // spawn/save, maintenance one-shots, upkeep toggles, performance/driving, with the destructive
  // Delete last.
  g_vehicle_items[0] = {"Vehicle Browser",
                        SHELL_ROW_SUBMENU,
                        SHELL_MENU_VEHICLE_SPAWNER,
                        GTAV_NATIVE_SHELL_ACTION_NONE,
                        nullptr,
                        0};
  g_vehicle_items[1] = {
      "Los Santos Customs",          SHELL_ROW_SUBMENU, SHELL_MENU_VEHICLE_CUSTOMS,
      GTAV_NATIVE_SHELL_ACTION_NONE, nullptr,           0};
  g_vehicle_items[2] = {"Controls",
                        SHELL_ROW_SUBMENU,
                        SHELL_MENU_VEHICLE_CONTROLS,
                        GTAV_NATIVE_SHELL_ACTION_NONE,
                        nullptr,
                        0};
  g_vehicle_items[3] = {"Autopilot",
                        SHELL_ROW_SUBMENU,
                        SHELL_MENU_AUTOPILOT,
                        GTAV_NATIVE_SHELL_ACTION_NONE,
                        nullptr,
                        0};
  // Garage slot selector: which of the 10 saved-vehicle slots Save/Spawn act on (pure worker-side
  // selection). Sits with the Save/Spawn rows so the active slot reads next to them.
  g_vehicle_items[4] = {"Garage Slot",   SHELL_ROW_LIST,
                        SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_SAVED_VEHICLE_SLOT,
                        nullptr,         0};
  // Spawn / save group. Save Current Vehicle is worker-direct (getters); Spawn Saved Vehicle
  // allocates a vehicle so it locks until the game-thread hook is live, like the spawn rows. Spawn
  // Maxed / Spawn Invincible are plain toggles (the work happens at create-time in the spawn job).
  g_vehicle_items[5] = {"Save Current Vehicle",
                        SHELL_ROW_ACTION,
                        SHELL_MENU_NONE,
                        GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE,
                        nullptr,
                        0};
  g_vehicle_items[6] = {"Spawn Saved Vehicle",    SHELL_ROW_ACTION,
                        SHELL_MENU_NONE,          GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE,
                        "Needs main-thread hook", 0};
  g_vehicle_items[7] = {"Spawn Maxed",   SHELL_ROW_TOGGLE,
                        SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_MAXED,
                        nullptr,         0};
  g_vehicle_items[8] = {"Spawn Invincible",
                        SHELL_ROW_TOGGLE,
                        SHELL_MENU_NONE,
                        GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_INVINCIBLE,
                        nullptr,
                        0};
  // Maintenance one-shots.
  g_vehicle_items[9] = {"Fix Vehicle",   SHELL_ROW_ACTION,
                        SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE,
                        nullptr,         0};
  g_vehicle_items[10] = {"Clean Vehicle", SHELL_ROW_ACTION,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CLEAN_VEHICLE,
                         nullptr,         0};
  g_vehicle_items[11] = {"Repair + Clean", SHELL_ROW_ACTION,
                         SHELL_MENU_NONE,  GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE,
                         nullptr,          0};
  g_vehicle_items[12] = {"Place on Wheels", SHELL_ROW_ACTION,
                         SHELL_MENU_NONE,   GTAV_NATIVE_SHELL_ACTION_PLACE_ON_WHEELS,
                         nullptr,           0};
  // Upkeep toggles (keep the vehicle in a desired state).
  g_vehicle_items[13] = {"Vehicle God Mode",
                         SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE,
                         GTAV_NATIVE_SHELL_ACTION_TOGGLE_VEHICLE_GOD,
                         nullptr,
                         0};
  g_vehicle_items[14] = {"Keep Clean",    SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_CLEAN,
                         nullptr,         0};
  g_vehicle_items[15] = {"Keep Repaired", SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_REPAIRED,
                         nullptr,         0};
  g_vehicle_items[16] = {"Engine Always On",
                         SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE,
                         GTAV_NATIVE_SHELL_ACTION_TOGGLE_ENGINE_ON,
                         nullptr,
                         0};
  // Performance / driving. Rocket Boost is a game-thread one-shot (locks until the hook is live);
  // Fly / Hover is a LIST cycler backed by volatile mode state (fly.inc) that self-refuses until
  // the frame hook is live.
  g_vehicle_items[17] = {"Nitro (hold Horn)",
                         SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE,
                         GTAV_NATIVE_SHELL_ACTION_TOGGLE_NITRO,
                         nullptr,
                         0};
  g_vehicle_items[18] = {"Nitro Power",   SHELL_ROW_LIST,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_NITRO_POWER,
                         nullptr,         0};
  g_vehicle_items[19] = {"Launch Boost",  SHELL_ROW_ACTION,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_LAUNCH_BOOST,
                         nullptr,         0};
  g_vehicle_items[20] = {"Rocket Boost",           SHELL_ROW_ACTION,
                         SHELL_MENU_NONE,          GTAV_NATIVE_SHELL_ACTION_VEHICLE_ROCKET_BOOST,
                         "Needs main-thread hook", 0};
  g_vehicle_items[21] = {"Super Brake",   SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_BRAKE,
                         nullptr,         0};
  g_vehicle_items[22] = {"Drift Mode",    SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_TOGGLE_DRIFT_MODE,
                         nullptr,         0};
  g_vehicle_items[23] = {"Stick to Ground", SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE,   GTAV_NATIVE_SHELL_ACTION_TOGGLE_STICK_TO_GROUND,
                         nullptr,           0};
  g_vehicle_items[24] = {"Slippery Roads", SHELL_ROW_TOGGLE,
                         SHELL_MENU_NONE,  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLIPPERY,
                         nullptr,          0};
  // Cruise Control + Cruise Speed live under the Autopilot submenu (kAutopilotItems), grouped with
  // the other self-driving rows, rather than in this root list.
  g_vehicle_items[25] = {"Fly / Hover",   SHELL_ROW_LIST,
                         SHELL_MENU_NONE, GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY,
                         nullptr,         0};
  // Delete Current Vehicle: game-thread DELETE_ENTITY one-shot, so it locks until the frame hook
  // is live (gtav_features_action_is_gated), like the spawn rows. Kept last (destructive).
  g_vehicle_items[26] = {"Delete Vehicle",         SHELL_ROW_ACTION,
                         SHELL_MENU_NONE,          GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE,
                         "Needs main-thread hook", 0};
}

// The Quick menu's rows are copies of the originals they pin. Find the live source row for a
// (action, param) pin by scanning every built menu except Quick itself; returns nullptr when the
// source menu has not been built yet or a dynamic row is currently filtered out (build_quick_items
// then renders a name-only fallback so a pin never silently vanishes).
static const ShellItem* find_row_for_action_param(uint32_t action, uint32_t param) {
  if (action == GTAV_NATIVE_SHELL_ACTION_NONE) return nullptr;
  for (uint32_t m = 0; m < SHELL_MENU_COUNT; ++m) {
    if (m == SHELL_MENU_QUICK || m == SHELL_MENU_FIND) continue;  // derived menus, not pin sources
    const ShellMenu* menu = &g_menus[m];
    for (uint32_t i = 0; i < menu->item_count; ++i) {
      const ShellItem* it = &menu->items[i];
      if (it->action == action && it->param == param && it->type != SHELL_ROW_SUBMENU) return it;
    }
  }
  return nullptr;
}

static ShellItem g_quick_items[GTAV_QUICK_PINS_MAX];
static uint32_t g_quick_item_count;

// Rebuild the Quick menu from the current pins. Each pin copies its source row verbatim (label,
// type, action, param, unavailable_reason) so a pinned toggle renders its live checkmark for free
// (shell_toggle_is_on keys off the global mask, not the menu) and a gated row stays locked the same
// way. Must run after the source menus are built; called last in build_menus() and on every
// pin/unpin. With no pins, a single disabled hint row keeps the menu from being blank.
static void build_quick_items() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < GTAV_QUICK_PINS_MAX; ++i) {
    if (g_quick_action[i] == GTAV_NATIVE_SHELL_ACTION_NONE) continue;
    ShellItem* dst = &g_quick_items[count++];
    const ShellItem* src = find_row_for_action_param(g_quick_action[i], g_quick_param[i]);
    if (src) {
      *dst = *src;
    } else {
      snprintf(dst->label, sizeof(dst->label), "%.40s",
               gtav_native_bridge_action_name(g_quick_action[i]));
      dst->type = SHELL_ROW_ACTION;
      dst->submenu = SHELL_MENU_NONE;
      dst->action = g_quick_action[i];
      dst->unavailable_reason = nullptr;
      dst->param = g_quick_param[i];
    }
  }
  if (count == 0) {
    ShellItem* dst = &g_quick_items[0];
    snprintf(dst->label, sizeof(dst->label), "(empty)");
    dst->type = SHELL_ROW_DISABLED;
    dst->submenu = SHELL_MENU_NONE;
    dst->action = GTAV_NATIVE_SHELL_ACTION_NONE;
    dst->unavailable_reason = "Pin a row with R3";
    dst->param = 0;
    count = 1;
  }
  g_quick_item_count = count;
  g_menus[SHELL_MENU_QUICK] = {"Quick", g_quick_items, g_quick_item_count};
}

// Top-level "Find": a flat, alphabetised index of every actionable row in the tree, so any feature
// is reachable via Find + L3/R3 letter-jump without walking the menus. Built once after the source
// menus (like Quick). The giant dynamic catalogs are excluded -- their thousands of spawn rows
// would swamp the index -- as are the Quick/Find hubs themselves.
#ifndef GTAV_FIND_ITEMS_CAP
#define GTAV_FIND_ITEMS_CAP 384u
#endif
static ShellItem g_find_items[GTAV_FIND_ITEMS_CAP];
static uint32_t g_find_item_count;

static int find_menu_is_excluded(uint32_t m) {
  switch (m) {
    case SHELL_MENU_QUICK:
    case SHELL_MENU_FIND:
    case SHELL_MENU_VEHICLE_SPAWNER:
    case SHELL_MENU_SKIN_PICKER:
    case SHELL_MENU_WEAPON_PICKER:
    case SHELL_MENU_PED_SPAWNER:
    case SHELL_MENU_OBJECT_SPAWNER:
    case SHELL_MENU_SELF_SCENARIO_PICKER:
    case SHELL_MENU_EMOTES:
    case SHELL_MENU_BODYGUARD_SPAWNER:
      return 1;
    default:
      return 0;
  }
}

// Case-insensitive label order so the Find index reads A->Z and L3/R3 letter-jump lands cleanly.
static int find_label_less(const char* a, const char* b) {
  for (;; ++a, ++b) {
    int ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : (int)(unsigned char)*a;
    int cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : (int)(unsigned char)*b;
    if (ca != cb) return ca < cb;
    if (ca == 0) return 0;  // equal
  }
}

static void build_find_items() {
  uint32_t count = 0;
  int truncated = 0;
  for (uint32_t m = 0; m < SHELL_MENU_COUNT && !truncated; ++m) {
    if (find_menu_is_excluded(m)) continue;
    const ShellMenu* menu = &g_menus[m];
    for (uint32_t i = 0; i < menu->item_count; ++i) {
      const ShellItem* it = &menu->items[i];
      // Index only actionable rows: skip submenus (navigation, not a feature) + pure-info rows.
      if (it->type != SHELL_ROW_ACTION && it->type != SHELL_ROW_TOGGLE &&
          it->type != SHELL_ROW_LIST) {
        continue;
      }
      if (it->action == GTAV_NATIVE_SHELL_ACTION_NONE) continue;
      // De-dup by (action, param) so a feature listed in two menus appears once.
      int dup = 0;
      for (uint32_t j = 0; j < count; ++j) {
        if (g_find_items[j].action == it->action && g_find_items[j].param == it->param) {
          dup = 1;
          break;
        }
      }
      if (dup) continue;
      if (count >= GTAV_FIND_ITEMS_CAP) {
        truncated = 1;
        break;
      }
      g_find_items[count++] = *it;
    }
  }
  // Alphabetise (insertion sort by label). Small N, runs once at build time.
  for (uint32_t a = 1; a < count; ++a) {
    ShellItem key = g_find_items[a];
    uint32_t b = a;
    while (b > 0 && find_label_less(key.label, g_find_items[b - 1].label)) {
      g_find_items[b] = g_find_items[b - 1];
      --b;
    }
    g_find_items[b] = key;
  }
  if (count == 0) {
    ShellItem* dst = &g_find_items[0];
    snprintf(dst->label, sizeof(dst->label), "(no features)");
    dst->type = SHELL_ROW_DISABLED;
    dst->submenu = SHELL_MENU_NONE;
    dst->action = GTAV_NATIVE_SHELL_ACTION_NONE;
    dst->unavailable_reason = nullptr;
    dst->param = 0;
    count = 1;
  }
  g_find_item_count = count;
  g_menus[SHELL_MENU_FIND] = {"Find", g_find_items, g_find_item_count};
  if (truncated) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "find index truncated at cap=%u",
                       (unsigned)GTAV_FIND_ITEMS_CAP);
  }
}

static void build_menus() {
  NB_TRACE("bm: enter; vehicle_items...");
  build_vehicle_items();
  NB_TRACE("bm: vehicle_items done; rebuild_spawner...");
  rebuild_spawner_items();
  build_weapon_picker_items();
  build_emote_picker_items();
  build_skin_picker_items();
  build_ped_spawner_items();
  build_bodyguard_spawner_items();
  build_object_spawner_items();
  rebuild_scenario_picker();
  NB_TRACE("bm: rebuild_spawner done; menus...");

  // Collapse the {title, items, sizeof(items)/sizeof(items[0])} boilerplate. The
  // runtime-built browsers register inside their build_*()/rebuild_*() helpers;
  // SHELL_MENU_VEHICLES holds g_vehicle_items with an explicit used-row count.
#define MENU_DEF(id, title, items) \
  g_menus[id] = {title, items, (uint32_t)(sizeof(items) / sizeof((items)[0]))}
  MENU_DEF(SHELL_MENU_MAIN, "Main", kMainItems);
  MENU_DEF(SHELL_MENU_SELF, "Self", kSelfItems);
  MENU_DEF(SHELL_MENU_SELF_DEFENSE, "Invincibility", kSelfDefenseItems);
  MENU_DEF(SHELL_MENU_SELF_MOVEMENT, "Movement", kSelfMovementItems);
  MENU_DEF(SHELL_MENU_SELF_METERS, "Meters", kSelfMetersItems);
  MENU_DEF(SHELL_MENU_SELF_PRESETS, "Presets", kSelfPresetItems);
  MENU_DEF(SHELL_MENU_SELF_APPEARANCE, "Appearance", kSelfAppearanceItems);
  MENU_DEF(SHELL_MENU_SELF_MONEY, "Money", kSelfMoneyItems);
  MENU_DEF(SHELL_MENU_SELF_WANTED, "Wanted", kSelfWantedItems);
  MENU_DEF(SHELL_MENU_SELF_TELEPORT, "Teleport", kSelfTeleportItems);
  MENU_DEF(SHELL_MENU_SELF_SCENARIOS, "Scenarios", kSelfScenariosItems);
  MENU_DEF(SHELL_MENU_SELF_TELEPORT_CITY, "City Landmarks", kSelfTeleportCityItems);
  MENU_DEF(SHELL_MENU_SELF_TELEPORT_COUNTRY, "North Landmarks", kSelfTeleportCountryItems);
  MENU_DEF(SHELL_MENU_SELF_TELEPORT_AIRFIELDS, "Airfields", kSelfTeleportAirfieldItems);
  g_menus[SHELL_MENU_VEHICLES] = {"Vehicles", g_vehicle_items, kVehicleRootRows};
  MENU_DEF(SHELL_MENU_WEAPONS, "Weapons", kWeaponItems);
  MENU_DEF(SHELL_MENU_VEHICLE_CUSTOMS, "Los Santos Customs", kVehicleCustomsItems);
  MENU_DEF(SHELL_MENU_LSC_PERFORMANCE, "Performance", kLscPerformanceItems);
  MENU_DEF(SHELL_MENU_LSC_PAINT, "Paint & Color", kLscPaintItems);
  MENU_DEF(SHELL_MENU_LSC_COSMETICS, "Cosmetics", kLscCosmeticsItems);
  MENU_DEF(SHELL_MENU_LSC_BODYWORK, "Bodywork", kLscBodyworkItems);
  MENU_DEF(SHELL_MENU_LSC_PLATES, "Plates & Livery", kLscPlatesItems);
  MENU_DEF(SHELL_MENU_WEAPON_FX, "Combat Effects", kWeaponFxItems);
  MENU_DEF(SHELL_MENU_WEAPON_ATTACH, "Attachments", kWeaponAttachItems);
  MENU_DEF(SHELL_MENU_WORLD, "World", kWorldItems);
  MENU_DEF(SHELL_MENU_WORLD_ENVIRONMENT, "Environment", kWorldEnvironmentItems);
  MENU_DEF(SHELL_MENU_HUD, "HUD", kHudItems);
  MENU_DEF(SHELL_MENU_RUNTIME, "Runtime", kRuntimeItems);
  MENU_DEF(SHELL_MENU_MENU_SETTINGS, "Menu Settings", kMenuSettingsItems);
  MENU_DEF(SHELL_MENU_KEYBINDS, "Keybinds", kKeybindsItems);
  MENU_DEF(SHELL_MENU_CONTROLS, "Controls", kControlsItems);
  MENU_DEF(SHELL_MENU_THEME_EDITOR, "Theme Editor", kThemeEditorItems);
  MENU_DEF(SHELL_MENU_THEME_EDITOR_MORE, "More Colours", kThemeEditorMoreItems);
  MENU_DEF(SHELL_MENU_EFFECTS, "Effects", kEffectsItems);
  MENU_DEF(SHELL_MENU_PED_CONTROL, "Ped Control", kPedControlItems);
  MENU_DEF(SHELL_MENU_SPAWNED_ENTITIES, "Spawned Entities", kSpawnedEntitiesItems);
  MENU_DEF(SHELL_MENU_COMPANIONS, "Companions", kCompanionItems);
  MENU_DEF(SHELL_MENU_MINIGAMES, "Minigames", kMinigamesItems);
  MENU_DEF(SHELL_MENU_AUTOPILOT, "Autopilot", kAutopilotItems);
  MENU_DEF(SHELL_MENU_VEHICLE_CONTROLS, "Vehicle Controls", kVehicleControlsItems);
  MENU_DEF(SHELL_MENU_WARDROBE, "Wardrobe", kWardrobeItems);
  MENU_DEF(SHELL_MENU_WORLD_SPECTACLE, "Spectacle", kWorldSpectacleItems);
  MENU_DEF(SHELL_MENU_FREE_CAM, "Free Camera", kFreeCamItems);
#undef MENU_DEF
  // Built last: Find + Quick copy rows from the source menus above, so those must exist first.
  build_find_items();
  build_quick_items();
}

static GtavNativeAddressTable g_table;
static GtavNativeShellSnapshot g_snapshot;
static uint64_t g_feature_toggle_mask;
// Display values for SHELL_ROW_LIST cyclers (weather, time, ...), pushed from the
// features module via gtav_native_bridge_set_list_value() -- the same push pattern
// as g_feature_toggle_mask. Kept here so the renderer stays self-contained.
struct ShellListValue {
  uint32_t action;
  char value[24];
};
// Capacity must be >= the number of distinct list cyclers ever pushed via
// set_list_value(): everything in push_list_values() (menu.c) -- weather/time/wanted/
// nitro/move/clock/timescale/gravity/paint x2/tint/neon/tyre-smoke (14), the 17 per-slot
// LS Customs mod pickers, the 8 LSC-expansion cyclers, weapon damage, cash, 4 keybinds,
// timecycle, animpostfx, 6 bodyguard cyclers, 4 object-placement cyclers, 3 autopilot
// cyclers -- plus the init-only theme/region/worker-hz/toast-time rows (~65 total). Past
// capacity, set_list_value() silently drops writes (rows read "..."), so keep headroom.
static ShellListValue g_list_values[112];
// Latches the first time gtav_native_bridge_set_list_value() runs out of slots and drops a
// write. A health signal, not an error: a correct build keeps this 0 (the array is sized with
// headroom over the distinct cycler count), so a 1 means a cycler was added past capacity and is
// silently rendering "..." instead of its value. Surfaced via
// gtav_native_bridge_list_values_saturated(); tests/test_native_bridge_list_capacity.py asserts
// the real menu never trips it.
static int g_list_values_saturated;
static uint32_t g_last_action_param;
static uint64_t g_last_frame_tick;
static uint32_t g_current_menu;
static uint32_t g_selected_by_menu[SHELL_MENU_COUNT];
static uint32_t g_scroll_by_menu[SHELL_MENU_COUNT];
static uint32_t g_menu_stack[kShellMaxMenuDepth];
static uint32_t g_menu_depth;
// Motion polish: the selection highlight glides toward the target row instead of
// jumping, and the panel fades/slides in when first shown. The glide is TICK-ANCHORED
// (eased from g_sel_anim_from to g_sel_anim_target over a fixed number of WORKER ticks
// starting at g_sel_anim_start_tick) so its speed depends only on worker_hz, not on
// render_interval -- the old per-render relaxation sped up whenever the render cadence
// did. g_sel_anim_y caches the last displayed (fractional) row; g_sel_anim_menu tracks
// which menu it belongs to so entering a submenu snaps instead of sweeping.
// g_menu_visible_since is the worker tick the panel last became visible (0->1), keyed
// for the open fade.
[[maybe_unused]] static float g_sel_anim_y;
[[maybe_unused]] static float g_sel_anim_from;
[[maybe_unused]] static float g_sel_anim_target;
[[maybe_unused]] static uint64_t g_sel_anim_start_tick;
[[maybe_unused]] static uint32_t g_sel_anim_menu = 0xffffffffu;
// (Reduce Motion state g_reduce_motion lives with the other persisted view indices near the top of
// this file, so its extern "C" accessors can reach it; the draw-path animation helpers read it.)
// Motion feedback: the worker tick at which the last activation / value-change happened. The draw
// path decays a brief flash from these so input is visibly acknowledged (a press/flip feels
// responsive instead of waiting on the toast). 0 = no pending flash. Pure tick math, over-render
// safe; if never set they have zero visual effect.
[[maybe_unused]] static uint64_t g_activate_flash_tick;
[[maybe_unused]] static uint64_t g_value_flash_tick;
static uint64_t g_menu_visible_since;
static int g_has_table;
static int g_reported_mode;
static int g_seen_frame_tick;
static uint32_t g_canary_flags;
static uint32_t g_canary_interval;
static uint32_t g_canary_max_frames;
static uint32_t g_canary_drawn_frames;
static uint32_t g_worker_text_overlay_draws;
static uint32_t g_canary_timer_reads;
static uint32_t g_last_game_timer;
static uint64_t g_draw_rect_call_override;
static int g_reported_canary;
static int g_reported_worker_text_overlay;
static int g_reported_timer_canary;
#if GTAV_MENU_PHASE_DRAW_LIST
// Worker-owned only while one immutable generation is being assembled. Draw helpers emit into this
// slot instead of invoking rendering natives; the phase callback sees it only after publication.
static GtavMenuDrawList* g_phase_draw_builder;
static int g_phase_draw_build_failed;
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
static char g_phase_draw_preview_dict[GTAV_MENU_DRAW_SPRITE_DICT_CAPACITY];
#endif
#endif
#if GTAV_RENDER_DIAG_ISOLATED && GTAV_MENU_PHASE_DRAW_LIST
static uint64_t g_static_menu_config;
#endif
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
// Model name of the vehicle under the cursor (for the status breadcrumb + placeholder caption),
// "" when not hovering a spawn row. Plus the vehicle's verified preview-registry entry, or NULL
// when it has no mapping -- in which case the render path shows a placeholder and NEVER draws a
// guessed model/model sprite (the white-card bug). Both resolved from the catalogs on hover-change
// and read on the worker thread only.
static char g_preview_model[24];
static const GtavVehiclePreviewEntry* g_preview_entry;
#endif

static const ShellMenu* shell_menu(uint32_t menu_id) {
  if (menu_id >= SHELL_MENU_COUNT) {
    menu_id = SHELL_MENU_MAIN;
  }
  return &g_menus[menu_id];
}

static uint32_t shell_item_count() {
  return shell_menu(g_current_menu)->item_count;
}

static uint32_t shell_window_start() {
  uint32_t count;
  uint32_t start;

  if (g_current_menu >= SHELL_MENU_COUNT) return 0;
  count = shell_item_count();
  if (count <= kShellVisibleRows) return 0;
  start = g_scroll_by_menu[g_current_menu];
  if (start > count - kShellVisibleRows) {
    start = count - kShellVisibleRows;
  }
  return start;
}

[[maybe_unused]] static uint32_t shell_visible_count() {
  const uint32_t count = shell_item_count();
  const uint32_t start = shell_window_start();
  const uint32_t remaining = start < count ? count - start : 0;
  return remaining < kShellVisibleRows ? remaining : kShellVisibleRows;
}

static void ensure_selection_visible() {
  uint32_t count;
  uint32_t selected;
  uint32_t start;

  if (g_current_menu >= SHELL_MENU_COUNT) return;
  count = shell_item_count();
  if (count <= kShellVisibleRows) {
    g_scroll_by_menu[g_current_menu] = 0;
    return;
  }
  selected = g_selected_by_menu[g_current_menu];
  start = g_scroll_by_menu[g_current_menu];
  if (start > count - kShellVisibleRows) {
    start = count - kShellVisibleRows;
  }
  if (selected < start) {
    start = selected;
  } else if (selected >= start + kShellVisibleRows) {
    start = selected - kShellVisibleRows + 1u;
  }
  g_scroll_by_menu[g_current_menu] = start;
}

static void clamp_current_selection() {
  uint32_t count = shell_item_count();
  if (!count) {
    g_snapshot.selected_index = 0;
    return;
  }
  if (g_current_menu >= SHELL_MENU_COUNT) {
    g_current_menu = SHELL_MENU_MAIN;
  }
  if (g_selected_by_menu[g_current_menu] >= count) {
    g_selected_by_menu[g_current_menu] = 0;
  }
  ensure_selection_visible();
  g_snapshot.selected_index = g_selected_by_menu[g_current_menu];
}

static const ShellItem* selected_item() {
  const ShellMenu* menu = shell_menu(g_current_menu);
  clamp_current_selection();
  if (!menu->item_count) return NULL;
  return &menu->items[g_snapshot.selected_index];
}

// Short call-site spellings shared with features.cpp (native_invoke.hpp).
using gtavmenu::invoke_return;
using gtavmenu::invoke_void;

static int text_native_ready() {
  return g_has_table && g_table.begin_text_command_display_text &&
         g_table.add_text_component_substring_player_name &&
         g_table.end_text_command_display_text && g_table.set_text_scale &&
         g_table.set_text_colour && g_table.set_text_font;
}

struct NativeTextStyle {
  int font;
  float scale_x;
  float scale_y;
  int r;
  int g;
  int b;
  int a;
  int centre;
  float wrap_start;
  float wrap_end;
  // Appended so the many {12-field} brace initializers below zero-init these (0 =
  // left-justified, no drop shadow, no outline). justification: 0 left, 2 right (pairs with
  // wrap_start/wrap_end as the alignment bounds). drop_shadow: non-zero enables the
  // engine's default text drop shadow for legibility over the game. outline: non-zero enables
  // the engine's text outline (the strongest legibility lever over bright scenes); both are
  // no-ops unless their natives are baked into the live build (see make/flags.mk).
  int justification;
  int drop_shadow;
  // Default member initializer so the existing {…} brace lists (which stop at drop_shadow) stay
  // valid without a -Wmissing-field-initializers warning; outline is opt-in per element.
  int outline = 0;
};

[[maybe_unused]] static void draw_native_text_line(const char* text, float x, float y,
                                                   const NativeTextStyle& style) {
  if (!text || !text[0] || !text_native_ready()) return;

#if GTAV_MENU_PHASE_DRAW_LIST
  if (g_phase_draw_builder &&
      gtav_menu_draw_list_add_text(g_phase_draw_builder, text, x, y, style.scale_x, style.scale_y,
                                   style.font, style.r, style.g, style.b, style.a, style.centre,
                                   style.wrap_start, style.wrap_end, style.justification,
                                   style.drop_shadow, style.outline) != 0)
    g_phase_draw_build_failed = 1;
  // A phase-list build has exactly one rectangle/text native producer: the verified callback.
  // Outside an active worker publication, fail closed instead of falling back off-phase.
  return;
#endif

  invoke_void(g_table.set_text_font, style.font);
  invoke_void(g_table.set_text_scale, style.scale_x, style.scale_y);
  invoke_void(g_table.set_text_colour, style.r, style.g, style.b, style.a);
  if (style.drop_shadow && g_table.set_text_drop_shadow) {
    invoke_void(g_table.set_text_drop_shadow);
  }
  if (style.outline && g_table.set_text_outline) {
    invoke_void(g_table.set_text_outline);
  }
  if (g_table.set_text_centre) {
    invoke_void(g_table.set_text_centre, style.centre);
  }
  // Always set justification per line so each one resets it (this native shares the
  // engine justify state with SET_TEXT_CENTRE). GTA's enum is 0=CENTRE, 1=LEFT,
  // 2=RIGHT -- our style uses 0 for left and 2 for right, so map 0 -> 1 (engine left).
  if (g_table.set_text_justification) {
    const int engine_just = (style.justification == 2) ? 2 : 1;
    invoke_void(g_table.set_text_justification, engine_just);
  }
  if (g_table.set_text_wrap) {
    invoke_void(g_table.set_text_wrap, style.wrap_start, style.wrap_end);
  }
  invoke_void(g_table.begin_text_command_display_text, "STRING");
  invoke_void(g_table.add_text_component_substring_player_name, text);
  invoke_void(g_table.end_text_command_display_text, x, y);
}

// Legibility shadow. When the engine's SET_TEXT_DROP_SHADOW native is baked into the live build
// (make/flags.mk), use it: one text pass instead of two, which halves this element's native-call
// volume on the over-render budget AND looks crisper than the hand-rolled copy. When it is NOT
// resolved (bare worker build, or an address miss), fall back to the manual two-pass: draw a
// near-black copy a hair down/right, then the real glyphs on top. The caller passes a style whose
// alpha is already faded, so either path inherits the same fade.
[[maybe_unused]] static void draw_native_text_shadowed(const char* text, float x, float y,
                                                       const NativeTextStyle& style) {
  if (g_table.set_text_drop_shadow) {
    NativeTextStyle s = style;
    s.drop_shadow = 1;  // engine shadow; outline (if requested in the style) is applied too
    draw_native_text_line(text, x, y, s);
    return;
  }
  const float off = 0.0015f;
  NativeTextStyle sh = style;
  sh.r = 0;
  sh.g = 0;
  sh.b = 0;
  sh.a = (style.a * 3) / 5;  // ~60% of the glyph alpha
  sh.drop_shadow = 0;
  if (sh.justification == 2) {
    sh.wrap_start += off;
    sh.wrap_end += off;
  }
  draw_native_text_line(text, x + off, y + off, sh);
  draw_native_text_line(text, x, y, style);
}

static uint32_t bridge_flags() {
  uint32_t flags = 0;
  if (!g_has_table) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_NO_NATIVE_TABLE;
    return flags;
  }
  if (g_table.get_frame_count) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_FRAME_READY;
  }
  if ((g_table.is_control_pressed && g_table.is_control_just_pressed) ||
      (g_table.is_disabled_control_pressed && g_table.is_disabled_control_just_pressed)) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_INPUT_READY;
  }
  if (g_table.draw_rect && g_table.begin_text_command_display_text &&
      g_table.add_text_component_substring_player_name && g_table.end_text_command_display_text) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_DRAW_READY;
  }
  if (text_native_ready()) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_TEXT_READY;
  }
#if GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY
  if (text_native_ready()) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_TEXT_OVERLAY_READY;
  }
#endif
  if ((g_canary_flags & GTAV_NATIVE_CANARY_DRAW_RECT) && g_table.draw_rect) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_DRAW_CANARY_READY;
  }
  if (g_table.get_game_timer) {
    flags |= GTAV_NATIVE_BRIDGE_FLAG_TIMER_READY;
  }
  return flags;
}

static const char* selected_label() {
  const ShellItem* item = selected_item();
  return item ? item->label : "";
}

static int shell_item_is_unavailable(const ShellItem* item);

[[maybe_unused]] static int shell_toggle_is_on(const ShellItem* item) {
  if (!item) return 0;
  // Telemetry lives in the snapshot rather than the feature mask; every other toggle reads
  // its bit straight from the pushed mask. The bit cases are generated from the shared
  // toggle list so they cannot fall out of sync with gtav_features_toggle_mask().
  if (item->action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY) {
    return g_snapshot.telemetry_enabled ? 1 : 0;
  }
  switch (item->action) {
#define GTAV_TOGGLE(name, state, reactivate, default_on) \
  case GTAV_NATIVE_SHELL_ACTION_TOGGLE_##name:           \
    return (g_feature_toggle_mask & GTAV_FEATURE_TOGGLE_##name) ? 1 : 0;
#include "gtavmenu/feature_toggles.def"
    default:
      return 0;
  }
}

// Current pushed display value for a SHELL_ROW_LIST row (e.g. "EXTRASUNNY").
static const char* shell_list_value(uint32_t action) {
  if (action == GTAV_NATIVE_SHELL_ACTION_NONE) return "";
  // The vehicle-class filter is owned here (it re-filters the spawner list), not
  // pushed from the feature layer like weather/time.
  if (action == GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_CLASS) {
    uint32_t n = (uint32_t)(sizeof(kVehicleClassNames) / sizeof(kVehicleClassNames[0]));
    return kVehicleClassNames[g_spawner_class_index % n];
  }
  // The ped-category filter (shared by skin/spawn-ped/bodyguard) is owned here too.
  if (action == GTAV_NATIVE_SHELL_ACTION_CYCLE_PED_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kPedCategoryNames) / sizeof(kPedCategoryNames[0]));
    return kPedCategoryNames[g_ped_category_index % n];
  }
  // The object-browser category filter re-filters the spawn list, so it is owned here as well.
  if (action == GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kObjectCategoryNames) / sizeof(kObjectCategoryNames[0]));
    return kObjectCategoryNames[g_object_category_index % n];
  }
  // The scenario-browser category filter re-filters the scenario list; owned here too.
  if (action == GTAV_NATIVE_SHELL_ACTION_CYCLE_SCENARIO_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kScenarioCategoryNames) / sizeof(kScenarioCategoryNames[0]));
    return kScenarioCategoryNames[g_scenario_category_index % n];
  }
  for (size_t i = 0; i < sizeof(g_list_values) / sizeof(g_list_values[0]); ++i) {
    if (g_list_values[i].action == action) return g_list_values[i].value;
  }
  return "";
}

[[maybe_unused]] static const char* selected_value_label(const ShellItem* item) {
  if (!item) return "";
  if (item->type == SHELL_ROW_TOGGLE) {
    return shell_toggle_is_on(item) ? "ON" : "OFF";
  }
  // Pure info / placeholder rows (the Controls reference page, numeric stubs, the empty-Quick
  // hint) carry no value -- a "LOCK" badge on them reads as an error. Genuinely locked rows
  // (hook-gated actions, locked lists) still fall through to "LOCK" below.
  if (item->type == SHELL_ROW_DISABLED || item->type == SHELL_ROW_NUMERIC_PLACEHOLDER) {
    return "";
  }
  if (shell_item_is_unavailable(item)) {
    return "LOCK";
  }
  if (item->type == SHELL_ROW_LIST) {
    // Theme Editor channels show their live 0-255 value (computed, not a g_list_values slot, so
    // one CYCLE_CUSTOM_COLOR action drives every channel row via its param).
    if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR) {
      static char buf[8];
      const uint8_t* c = custom_color_channel(item->param);
      snprintf(buf, sizeof(buf), "%u", c ? (unsigned)*c : 0u);
      return buf;
    }
    const char* v = shell_list_value(item->action);
    return v[0] ? v : "...";
  }
  if (item->type == SHELL_ROW_SUBMENU) {
    // Item-count badge for submenus large enough to SCROLL (e.g. the vehicle browser reads "815"):
    // shows list size before drilling in, where it actually helps. Small menus that fit on one
    // screen keep the clean "OPEN" -- a count there is just noise. draw_menu_row appends " >".
    if (item->submenu < SHELL_MENU_COUNT) {
      const ShellMenu* sub = shell_menu(item->submenu);
      if (sub && sub->item_count > kShellVisibleRows) {
        static char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)sub->item_count);
        return cbuf;
      }
    }
    return "OPEN";
  }
  if (item->action == GTAV_NATIVE_SHELL_ACTION_STOP) {
    return "STOP";
  }
  if (item->action == GTAV_NATIVE_SHELL_ACTION_HIDE) {
    return "HIDE";
  }
  if (item->type == SHELL_ROW_ACTION) {
    return "ACT";
  }
  return "";
}

static const char* shell_row_type_name(uint32_t type) {
  switch (type) {
    case SHELL_ROW_SUBMENU:
      return "submenu";
    case SHELL_ROW_ACTION:
      return "action";
    case SHELL_ROW_TOGGLE:
      return "toggle";
    case SHELL_ROW_DISABLED:
      return "disabled";
    case SHELL_ROW_NUMERIC_PLACEHOLDER:
      return "numeric";
    case SHELL_ROW_LIST:
      return "list";
    default:
      return "unknown";
  }
}

static int shell_item_is_unavailable(const ShellItem* item) {
  if (!item) return 0;
  // A toggle/list row may name a hard prerequisite via unavailable_reason (e.g. a feature
  // staged behind a not-yet-resolved native); render it LOCKed with that reason until the row
  // clears the field. ACTION rows instead use the dynamic gated path below (they unlock when
  // the main-thread hook lands). No row currently sets this, but the renderer honors it.
  if (item->unavailable_reason &&
      (item->type == SHELL_ROW_TOGGLE || item->type == SHELL_ROW_LIST)) {
    return 1;
  }
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Actions gated to the main-thread hook render as locked until it lands.
  if (item->type == SHELL_ROW_ACTION && gtav_features_action_is_gated(item->action)) {
    return 1;
  }
#endif
  return item->type == SHELL_ROW_DISABLED || item->type == SHELL_ROW_NUMERIC_PLACEHOLDER;
}

static void refresh_strings() {
  const uint32_t flags = bridge_flags();
  const char* mode = (flags & GTAV_NATIVE_BRIDGE_FLAG_DRAW_READY) ? "native-draw" : "manual";
  if (flags & GTAV_NATIVE_BRIDGE_FLAG_NO_NATIVE_TABLE) {
    mode = "manual-no-native-table";
  } else if (flags & GTAV_NATIVE_BRIDGE_FLAG_DRAW_CANARY_READY) {
    mode = "native-draw-canary";
#if GTAV_MENU_PHASE_DRAW_LIST
  } else if (flags & GTAV_NATIVE_BRIDGE_FLAG_TEXT_OVERLAY_READY) {
    mode = "native-phase-draw-list";
#else
#if GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY
  } else if (flags & GTAV_NATIVE_BRIDGE_FLAG_TEXT_OVERLAY_READY) {
#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
    mode = "native-worker-text-menu";
#else
    mode = "native-worker-text-overlay";
#endif
#endif
#endif
  } else if ((g_canary_flags & GTAV_NATIVE_CANARY_GET_GAME_TIMER) && g_table.get_game_timer) {
    mode = "native-timer-canary";
  }
  snprintf(g_snapshot.selected_label, sizeof(g_snapshot.selected_label), "%s", selected_label());
  snprintf(g_snapshot.mode, sizeof(g_snapshot.mode), "%s", mode);
}

static uint64_t resolved_frame_count(uint64_t fallback) {
  if (g_has_table && g_table.get_frame_count) {
    return invoke_return<uint32_t>(g_table.get_frame_count);
  }
  return fallback;
}

static void refresh_snapshot(uint64_t worker_tick, uint64_t frame_tick, int visible) {
#if GTAV_RENDER_DIAG_ISOLATED
  // Includes init, visibility and canary setters: no hidden GET_FRAME_COUNT or UI mutation.
  gtav_render_diag_suppressed_snapshot();
  return;
#endif
#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_snapshot(1);
#endif
  gtav_native_shell_state.magic = GTAV_NATIVE_SHELL_STATE_MAGIC;
  gtav_native_shell_state.abi_version = GTAV_NATIVE_BRIDGE_ABI_VERSION;
  gtav_native_shell_state.struct_size = sizeof(GtavNativeShellState);
  g_snapshot.abi_version = GTAV_NATIVE_BRIDGE_ABI_VERSION;
  g_snapshot.flags = bridge_flags();
  g_snapshot.visible = visible ? 1u : 0u;
  g_snapshot.item_count = shell_item_count();
  g_snapshot.worker_tick = worker_tick;
  g_snapshot.frame_count = resolved_frame_count(frame_tick);
  clamp_current_selection();
  refresh_strings();
  memcpy(&gtav_native_shell_state.snapshot, &g_snapshot, sizeof(g_snapshot));
  __sync_synchronize();
#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_snapshot(0);
#endif
}

// Re-publish the snapshot after a cursor/scroll change, from the current bridge state: refresh
// from the last known worker tick / frame / visibility. The change draws promptly because the
// visible panel re-renders every eligible tick (the over-render lane). The nav + adjust sites
// all repeat this.
static void republish_snapshot(void) {
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, (int)g_snapshot.visible);
}

static void refresh_worker_snapshot(uint64_t worker_tick, int visible) {
  uint64_t frame_tick = g_seen_frame_tick ? g_last_frame_tick : worker_tick;
  refresh_snapshot(worker_tick, frame_tick, visible);
}

static void refresh_frame_snapshot(uint64_t frame_tick, int visible) {
  g_last_frame_tick = frame_tick;
  g_seen_frame_tick = 1;
  refresh_snapshot(g_snapshot.worker_tick, frame_tick, visible);
}

static void draw_rect_canary(uint64_t frame_tick, int visible) {
  uint64_t draw_rect_address =
      g_draw_rect_call_override ? g_draw_rect_call_override : g_table.draw_rect;

  if (!visible) return;
  if (!(g_canary_flags & GTAV_NATIVE_CANARY_DRAW_RECT)) return;
  if (!g_has_table || !draw_rect_address) return;
  if (g_canary_max_frames && g_canary_drawn_frames >= g_canary_max_frames) return;
  if (g_canary_interval && (frame_tick % g_canary_interval) != 0) return;

  if (!g_reported_canary) {
    g_reported_canary = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native draw canary first frame addr=0x%llx",
                       (unsigned long long)draw_rect_address);
  }

  invoke_void(draw_rect_address, 0.50f, 0.50f, 0.32f, 0.12f, 30, 144, 255, 180);
  ++g_canary_drawn_frames;
}

[[maybe_unused]] static void draw_shell_rect(uint64_t draw_rect_address, float x, float y, float w,
                                             float h, int r, int g, int b, int a) {
#if GTAV_MENU_PHASE_DRAW_LIST
  (void)draw_rect_address;
  if (g_phase_draw_builder &&
      gtav_menu_draw_list_add_rect(g_phase_draw_builder, x, y, w, h, r, g, b, a) != 0)
    g_phase_draw_build_failed = 1;
#else
  invoke_void(draw_rect_address, x, y, w, h, r, g, b, a);
#endif
}

[[maybe_unused]] static void draw_shell_sprite(uint64_t draw_sprite_address, const char* dict,
                                               const char* texture, float x, float y, float w,
                                               float h, float heading, int r, int g, int b, int a,
                                               int p11, int p12) {
#if GTAV_MENU_PHASE_DRAW_LIST
  (void)draw_sprite_address;
  if (!g_phase_draw_builder ||
      gtav_menu_draw_list_add_sprite(g_phase_draw_builder, dict, texture, x, y, w, h, heading, r, g,
                                     b, a, p11, p12) != 0) {
    g_phase_draw_build_failed = 1;
    return;
  }
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
  snprintf(g_phase_draw_preview_dict, sizeof(g_phase_draw_preview_dict), "%s", dict);
#endif
#else
  invoke_void(draw_sprite_address, dict, texture, x, y, w, h, heading, r, g, b, a, p11, p12);
#endif
}

static void get_game_timer_canary(uint64_t worker_tick) {
  uint32_t value;

  if (!(g_canary_flags & GTAV_NATIVE_CANARY_GET_GAME_TIMER)) return;
  if (!g_has_table || !g_table.get_game_timer) return;
  if (g_canary_max_frames && g_canary_timer_reads >= g_canary_max_frames) return;
  if (g_canary_interval && (worker_tick % g_canary_interval) != 0) return;

  if (!g_reported_timer_canary) {
    g_reported_timer_canary = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native timer canary first call addr=0x%llx",
                       (unsigned long long)g_table.get_game_timer);
  }

  value = invoke_return<uint32_t>(g_table.get_game_timer);
  g_last_game_timer = value;
  ++g_canary_timer_reads;

  if (g_canary_timer_reads <= 3 || (g_canary_timer_reads % 60u) == 0) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native timer canary read=%u value=%u",
                       g_canary_timer_reads, g_last_game_timer);
  }
}

#if GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY
[[maybe_unused]] static void draw_worker_text_status_line() {
  const NativeTextStyle style = {0, 0.31f, 0.31f, 255, 255, 255, 230, 0, 0.0f, 1.0f, 0, 0};
  char line[64];

  snprintf(line, sizeof(line), "GTAV MENU > %s", selected_label());
  draw_native_text_line(line, 0.18f, 0.18f, style);
}

// Paints the menu chrome with DRAW_RECT before the text is queued, so the panel,
// header bar, and selection highlight sit behind the labels. Rendered only when a
// DRAW_RECT address is present (the background builds set it); text-only builds
// leave it 0 and skip this entirely. DRAW_RECT queues into the same render buffer
// as the text natives, so this needs no hook -- the proven-safe worker lane.
// Geometry shared with the text layout so the chrome frames the labels. Text is
// drawn left-aligned at GTAV_MENU_PANEL_TEXT_X; the panel insets a small pad to its
// left and is sized to typical row width. GTA text y is the glyph TOP, so the row
// boxes are nudged down by half a glyph height to vertically centre on the text.
// Default position is the RIGHT side of the screen: text starts at 0.75 and the
// ~0.205-wide panel ends near 0.94, leaving a clean right-edge margin. (Override
// GTAV_MENU_PANEL_TEXT_X to reposition; everything else derives from it.)
#ifndef GTAV_MENU_PANEL_TEXT_X
#define GTAV_MENU_PANEL_TEXT_X 0.75f
#endif
#define GTAV_MENU_PANEL_TITLE_Y 0.040f
#define GTAV_MENU_PANEL_ROW0_Y 0.085f
#define GTAV_MENU_PANEL_ROW_H 0.030f
// Shared panel geometry (single source for draw_menu_background + the text layout so the
// chrome always frames the labels). Width was widened (0.205 -> 0.225 -> 0.305) so long labels
// like "Disable All Features" lay out in full instead of ellipsizing; the anchor (REGION_RIGHT_X)
// was nudged left in step to keep the clean right-edge margin. PAD_L insets the label from the
// panel's left edge; VALUE_PAD insets the right-justified value column; GLYPH_MID is half a glyph
// height (GTA text y is the glyph TOP, so row boxes drop by this to centre on the text).
#define GTAV_MENU_PANEL_W 0.305f
#define GTAV_MENU_PANEL_PAD_L 0.014f
#define GTAV_MENU_PANEL_VALUE_PAD 0.012f
#define GTAV_MENU_PANEL_GLYPH_MID 0.012f
// Open-fade ramp length in worker ticks (alpha 0->1 + a small slide into place).
#define GTAV_MENU_PANEL_FADE_IN_TICKS 10u
// Selection-glide duration in worker ticks (eased travel of the highlight to the focused row).
#define GTAV_MENU_SEL_GLIDE_TICKS 6u

// Short help text for the footer, keyed by action. The per-action strings live in the
// shared single-source list include/gtavmenu/feature_descriptions.def (the same X-macro
// pattern as gtav_native_bridge_action_name), so adding a row's footer help is a one-line
// edit there instead of another switch case that can silently fall through to a blank
// footer. Coverage is enforced by tests/test_item_description_coverage.py.
[[maybe_unused]] static const char* item_description(const ShellItem* item) {
  if (!item) return "";
  if (shell_item_is_unavailable(item)) {
    return item->unavailable_reason ? item->unavailable_reason : "Needs main-thread hook";
  }
  if (item->type == SHELL_ROW_SUBMENU) return "Open this submenu";
  switch (item->action) {
#define GTAV_ACTION_DESC(suffix, desc)    \
  case GTAV_NATIVE_SHELL_ACTION_##suffix: \
    return desc;
#include "gtavmenu/feature_descriptions.def"
    default:
      return "";
  }
}

// Libm-free easing curves over t in [0,1] (clamped). The worker ELF does not link libm, so these
// are simple polynomials: smoothstep (3t^2-2t^3, symmetric ease-in-out) and ease_out_cubic
// (1-(1-t)^3, fast start / soft landing). Used by the tick-anchored animations so motion
// decelerates naturally instead of moving linearly. Pure and deterministic -> over-render safe.
[[maybe_unused]] static inline float ease_clamp01(float t) {
  return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}
[[maybe_unused]] static inline float ease_smoothstep(float t) {
  t = ease_clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}
[[maybe_unused]] static inline float ease_out_cubic(float t) {
  t = ease_clamp01(t);
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

// Open fade: 0 on the tick the panel becomes visible, ramping to 1 over FADE_IN_TICKS.
// Pure function of the tick counter + g_menu_visible_since so it is over-render safe (the
// whole panel is a function of this each tick, no stateful partial draws). The guard keeps
// the unsigned subtraction from underflowing if a stale tick is passed. Reduce Motion snaps it
// straight to 1 (no fade/slide).
[[maybe_unused]] static float menu_open_fade(uint64_t worker_tick) {
  if (g_reduce_motion) return 1.0f;
  const uint64_t e =
      (worker_tick > g_menu_visible_since) ? (worker_tick - g_menu_visible_since) : 0u;
  if (e >= GTAV_MENU_PANEL_FADE_IN_TICKS) return 1.0f;
  return ease_smoothstep((float)e / (float)GTAV_MENU_PANEL_FADE_IN_TICKS);
}

// Scale an 0-255 alpha by the fade factor (shared by chrome + text so the panel animates as one).
static inline int menu_fade_alpha(int a, float fade) {
  return (int)((float)a * fade);
}

// Motion-feedback decay: 1.0 on the event tick, ramping linearly to 0 over GTAV_MENU_FLASH_TICKS.
// Pure function of the tick counters (over-render safe). 0 when no flash is pending or it elapsed.
#define GTAV_MENU_FLASH_TICKS 8u
[[maybe_unused]] static float flash_decay(uint64_t tick, uint64_t flash_tick) {
  if (g_reduce_motion) return 0.0f;
  if (flash_tick == 0u || tick < flash_tick) return 0.0f;
  const uint64_t e = tick - flash_tick;
  if (e >= GTAV_MENU_FLASH_TICKS) return 0.0f;
  return 1.0f - (float)e / (float)GTAV_MENU_FLASH_TICKS;
}

// Controller-verb legend for the focused row, rendered as a native-style instructional-button bar
// (glyph chip + verb) along the bottom-right of the screen rather than a line of text inside the
// panel. The model is data: build_hint_buttons() turns the focused row into an ordered list of
// (glyph, verb) pairs, and draw_instructional_bar() lays them out. The static Controls page still
// documents the full chord grammar; this bar shows just the row-specific verbs (the global
// open/close + page/letter-jump chords are constant).
enum HintGlyph {
  HINT_GLYPH_CROSS = 0,  // PS Cross  -- select / toggle / run / open
  HINT_GLYPH_CIRCLE,     // PS Circle -- back
  HINT_GLYPH_DPAD_LR,    // D-pad left/right -- adjust a list value
  HINT_GLYPH_R3,         // R3 (right-stick click) -- pin to Quick / favourite
  HINT_GLYPH_L1R1,       // L1/R1 shoulders -- unused now (kept for table parity; not shown)
  HINT_GLYPH_COUNT
};

// GTA V FRONTEND control ids (-1 = none). Two consumers: the native instructional_buttons Scaleform
// (Route B, features.cpp) maps these to real glyphs via GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING,
// and the `~INPUT_*~` token names below render the same glyphs inline in our worker-drawn fallback
// bar. Single source; HW-tunable if a glyph comes out wrong (the ids are the only unverified part).
enum {
  GTAV_INPUT_FRONTEND_LEFT = 189,
  GTAV_INPUT_FRONTEND_RIGHT = 190,
  GTAV_INPUT_FRONTEND_ACCEPT = 201,  // Cross
  GTAV_INPUT_FRONTEND_CANCEL = 202,  // Circle
  GTAV_INPUT_FRONTEND_X = 203,       // Square
  GTAV_INPUT_FRONTEND_Y = 204,       // Triangle
  GTAV_INPUT_FRONTEND_LB = 205,      // L1
  GTAV_INPUT_FRONTEND_RB = 206,      // R1
  GTAV_INPUT_FRONTEND_LT = 207,      // L2
  GTAV_INPUT_FRONTEND_RT = 208,      // R2
  GTAV_INPUT_FRONTEND_RS = 210,      // R3 (right-stick click)
};

// Per-glyph descriptor. Three render tiers, picked at draw time:
//   1. real native Scaleform bar (Route B) -- uses ctrl_a/ctrl_b; drawn game-thread-side.
//   2. inline `~INPUT_*~` glyph token in our bar (Route A, GTAV_MENU_ENABLE_BUTTON_GLYPHS) -- GTA's
//      text formatter substitutes the controller's real button glyph for the token.
//   3. coloured ASCII chip (always-safe fallback) -- DRAW_RECT + `token` text.
// The legacy sprite (txd, tex) tier stays EMPTY (superseded by the inline-token glyphs, which need
// no streamed texture); a non-empty mapping would still draw a real sprite where verified.
struct HintGlyphInfo {
  const char* token;        // chip text, e.g. "X", "O", "< >"
  const char* glyph_token;  // "~INPUT_*~" markup -> real inline glyph (Route A)
  int16_t ctrl_a,
      ctrl_b;       // FRONTEND control ids for the Scaleform bar (Route B); ctrl_b -1 = one glyph
  uint8_t r, g, b;  // chip background colour
  const char* txd;  // sprite texture dict ("" => no sprite tier)
  const char* tex;  // sprite texture name
};
// [[maybe_unused]]: every consumer sits behind a render/Scaleform gate, so a build with
// those gates off (the host overlay tests) would otherwise trip -Wunused-const-variable.
[[maybe_unused]] static const HintGlyphInfo kHintGlyphInfo[HINT_GLYPH_COUNT] = {
    /* CROSS   */ {"X", "~INPUT_FRONTEND_ACCEPT~", GTAV_INPUT_FRONTEND_ACCEPT, -1, 64, 134, 224, "",
                   ""},
    /* CIRCLE  */
    {"O", "~INPUT_FRONTEND_CANCEL~", GTAV_INPUT_FRONTEND_CANCEL, -1, 214, 74, 74, "", ""},
    /* DPAD_LR */
    {"< >", "~INPUT_FRONTEND_LEFT~~INPUT_FRONTEND_RIGHT~", GTAV_INPUT_FRONTEND_LEFT,
     GTAV_INPUT_FRONTEND_RIGHT, 64, 70, 82, "", ""},
    /* R3      */  // right-stick click; INPUT_FRONTEND_RS glyph (+ ~INPUT_FRONTEND_RS~ token
                   // fallback), "R3" chip if neither resolves.
    {"R3", "~INPUT_FRONTEND_RS~", GTAV_INPUT_FRONTEND_RS, -1, 64, 70, 82, "", ""},
    /* L1R1    */  // retained for table parity; no longer shown (L1/R1 are not menu buttons)
    {"L1/R1", "~INPUT_FRONTEND_LB~~INPUT_FRONTEND_RB~", GTAV_INPUT_FRONTEND_LB,
     GTAV_INPUT_FRONTEND_RB, 64, 70, 82, "", ""},
};

struct HintButton {
  uint8_t glyph;      // HintGlyph
  const char* label;  // verb performed on the focused row
};

static void hint_push(HintButton* out, uint32_t* n, uint32_t cap, uint8_t glyph,
                      const char* label) {
  if (*n < cap) {
    out[*n].glyph = glyph;
    out[*n].label = label;
    ++*n;
  }
}

// Fill `out` (up to `cap`) with the focused row's (glyph, verb) pairs, ordered left-to-right.
[[maybe_unused]] static uint32_t build_hint_buttons(const ShellItem* sel, HintButton* out,
                                                    uint32_t cap) {
  uint32_t n = 0u;
  if (!sel) {
    hint_push(out, &n, cap, HINT_GLYPH_CROSS, "Select");
    hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
    return n;
  }
  if (shell_item_is_unavailable(sel)) {
    if (sel->type == SHELL_ROW_SUBMENU) hint_push(out, &n, cap, HINT_GLYPH_CROSS, "Open");
    hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
    return n;
  }
  switch (sel->type) {
    case SHELL_ROW_SUBMENU:
      hint_push(out, &n, cap, HINT_GLYPH_CROSS, "Open");
      hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
      break;
    case SHELL_ROW_TOGGLE:
      hint_push(out, &n, cap, HINT_GLYPH_CROSS, "Toggle");
      hint_push(out, &n, cap, HINT_GLYPH_R3, "Pin");
      hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
      break;
    case SHELL_ROW_LIST:
      hint_push(out, &n, cap, HINT_GLYPH_DPAD_LR, "Adjust");
      hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
      break;
    case SHELL_ROW_ACTION:
      hint_push(out, &n, cap, HINT_GLYPH_CROSS, "Run");
      hint_push(out, &n, cap, HINT_GLYPH_R3, "Pin");
      hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
      break;
    default:
      hint_push(out, &n, cap, HINT_GLYPH_CIRCLE, "Back");
      break;
  }
  return n;
}

[[maybe_unused]] static void draw_menu_background(uint64_t worker_tick, uint32_t visible_count,
                                                  uint32_t selected_row) {
  const uint64_t dr = g_table.draw_rect;
  if (!dr) return;
  const MenuTheme& th = active_theme();
  const float fade = menu_open_fade(worker_tick);
  const float y_off = (1.0f - fade) * 0.012f;  // slide into place on open (matches the toast slide)
  const float text_x = menu_panel_text_x();
  const float pad_l = GTAV_MENU_PANEL_PAD_L;
  const float panel_w = menu_panel_width();
  const float panel_left = text_x - pad_l;
  const float panel_cx = panel_left + panel_w * 0.5f;
  const float title_y = GTAV_MENU_PANEL_TITLE_Y + y_off;
  const float row0_y = GTAV_MENU_PANEL_ROW0_Y + y_off;
  const float row_h = GTAV_MENU_PANEL_ROW_H;
  const float glyph_mid = GTAV_MENU_PANEL_GLYPH_MID;
  const float header_top = title_y - 0.018f;
  const float rows_bottom = row0_y + row_h * (float)visible_count;
  // Footer strip below the rows (holds the description + position counter).
  const float footer_gap = 0.006f;
  const float footer_h = row_h;
  const float footer_top = rows_bottom + footer_gap;
  const float footer_cy = footer_top + footer_h * 0.5f;
  // The controller-verb legend used to live on a hint strip below the footer; it now renders as a
  // native-style instructional-button bar along the bottom of the screen (draw_instructional_bar),
  // so the panel ends at the footer -- one row shorter and less cluttered.
  const float bottom = footer_top + footer_h + 0.004f;
  const float panel_cy = (header_top + bottom) * 0.5f;
  const float panel_h = bottom - header_top;
  // Selection glide: ease the displayed row from where it was toward the target over a fixed
  // number of WORKER ticks (tick-anchored, so the speed is independent of render_interval -- the
  // old per-render relaxation sped up with the render cadence). Snap when the menu changed (so
  // entering a submenu does not sweep the bar across the panel) or when Reduce Motion is on. When
  // the target changes mid-glide, re-anchor from the current displayed position so it does not
  // jump.
  const float sel_target = (float)selected_row;
  if (g_sel_anim_menu != g_current_menu || g_reduce_motion) {
    g_sel_anim_menu = g_current_menu;
    g_sel_anim_from = sel_target;
    g_sel_anim_target = sel_target;
    g_sel_anim_start_tick = worker_tick;
    g_sel_anim_y = sel_target;
  } else if (sel_target != g_sel_anim_target) {
    const uint64_t e = worker_tick - g_sel_anim_start_tick;
    const float t = ease_out_cubic((float)e / (float)GTAV_MENU_SEL_GLIDE_TICKS);
    g_sel_anim_from = g_sel_anim_from + (g_sel_anim_target - g_sel_anim_from) * t;  // current pos
    g_sel_anim_target = sel_target;
    g_sel_anim_start_tick = worker_tick;
  }
  {
    const uint64_t e = worker_tick - g_sel_anim_start_tick;
    const float t = ease_out_cubic((float)e / (float)GTAV_MENU_SEL_GLIDE_TICKS);
    g_sel_anim_y = g_sel_anim_from + (g_sel_anim_target - g_sel_anim_from) * t;
  }
  const float sel_cy = row0_y + row_h * g_sel_anim_y + glyph_mid;
  // Subtle frame + main panel. A slightly larger dark rect CENTRED behind the panel reads as
  // a clean border/outline; the previous shadow was offset down-right at the panel's full size
  // so it looked like a second, misaligned box rather than a shadow.
  draw_shell_rect(dr, panel_cx, panel_cy, panel_w + 0.005f, panel_h + 0.006f, 0, 0, 0,
                  menu_fade_alpha(150, fade));
  draw_shell_rect(dr, panel_cx, panel_cy, panel_w, panel_h, th.panel[0], th.panel[1], th.panel[2],
                  menu_fade_alpha(th.panel[3], fade));
  // Accent header bar: TOP edge flush with the panel top, tall enough to frame the title.
  const float header_bar_h = (row0_y - header_top) - 0.010f;
  const float header_bar_cy = header_top + header_bar_h * 0.5f;
  draw_shell_rect(dr, panel_cx, header_bar_cy, panel_w, header_bar_h, th.accent[0], th.accent[1],
                  th.accent[2], menu_fade_alpha(th.accent[3], fade));
  // Per-row banding (behind the selection highlight, which is drawn next so it always wins): a very
  // faint zebra wash on alternating rows makes the label->value sweep easier to track across the
  // panel, and a darker wash on genuinely locked (hook-gated) action rows reads them as inactive at
  // a glance. Pure DRAW_RECT, over-render-safe. Info rows get only the zebra (no "locked" wash).
  {
    const ShellMenu* bg_menu = shell_menu(g_current_menu);
    const uint32_t bg_start = shell_window_start();
    if (bg_menu) {
      for (uint32_t row = 0; row < visible_count; ++row) {
        const uint32_t index = bg_start + row;
        if (index >= bg_menu->item_count) break;
        const float row_cy = row0_y + row_h * (float)row + glyph_mid;
        if (row & 1u) {
          draw_shell_rect(dr, panel_cx, row_cy, panel_w, row_h, 255, 255, 255,
                          menu_fade_alpha(10, fade));
        }
        const ShellItem* it = &bg_menu->items[index];
        // Wash only genuinely locked (hook-gated) rows -- not pure-info rows (DISABLED / numeric
        // placeholders), which carry no action to unlock and would just read as greyed clutter.
        if (shell_item_is_unavailable(it) && it->type != SHELL_ROW_DISABLED &&
            it->type != SHELL_ROW_NUMERIC_PLACEHOLDER) {
          draw_shell_rect(dr, panel_cx, row_cy, panel_w, row_h, 0, 0, 0, menu_fade_alpha(55, fade));
        }
      }
    }
  }
  // Selection highlight behind the active (glided) row, centred on the row glyphs.
  draw_shell_rect(dr, panel_cx, sel_cy, panel_w, row_h, th.selection[0], th.selection[1],
                  th.selection[2], menu_fade_alpha(th.selection[3], fade));
  // Activation flash: a quick white wash over the selected row on Cross, decaying over a few
  // ticks so a press is visibly acknowledged immediately (ahead of the toast).
  const float act_flash = flash_decay(worker_tick, g_activate_flash_tick);
  if (act_flash > 0.0f) {
    draw_shell_rect(dr, panel_cx, sel_cy, panel_w, row_h, 255, 255, 255,
                    menu_fade_alpha((int)(75.0f * act_flash), fade));
  }
  // Bright accent edge-bar on the left of the selection: a clear "you are here" marker that
  // reads even when the translucent highlight washes out over a bright scene (mirrors the toast
  // accent bar). Glides with the highlight since it shares sel_cy.
  const float accent_w = 0.0035f;
  draw_shell_rect(dr, panel_left + accent_w * 0.5f, sel_cy, accent_w, row_h, th.accent[0],
                  th.accent[1], th.accent[2], menu_fade_alpha(255, fade));
  // Footer accent strip (darker than the header) for the help text + counter.
  draw_shell_rect(dr, panel_cx, footer_cy, panel_w, footer_h, th.footer[0], th.footer[1],
                  th.footer[2], menu_fade_alpha(th.footer[3], fade));
  // Scrollbar: when the list is longer than the window, draw a track down the right edge with
  // a thumb sized + positioned by the visible window over the whole list. Clearer than the old
  // pair of top/bottom chips -- you can see both that there's more AND roughly where you are.
  const uint32_t wstart = shell_window_start();
  const uint32_t total = shell_item_count();
  if (total > visible_count && total > 0u) {
    const float bar_x = panel_left + panel_w - 0.006f;
    const float bar_w = 0.004f;
    const float track_top = row0_y - glyph_mid;
    const float track_h = rows_bottom - track_top;
    const float track_cy = track_top + track_h * 0.5f;
    draw_shell_rect(dr, bar_x, track_cy, bar_w, track_h, 90, 100, 120,
                    menu_fade_alpha(140, fade));  // dim track
    float thumb_h = track_h * ((float)visible_count / (float)total);
    if (thumb_h < 0.010f) thumb_h = 0.010f;  // keep it grabbable-looking on long lists
    float thumb_top = track_top + track_h * ((float)wstart / (float)total);
    if (thumb_top + thumb_h > track_top + track_h) thumb_top = track_top + track_h - thumb_h;
    const float thumb_cy = thumb_top + thumb_h * 0.5f;
    draw_shell_rect(dr, bar_x, thumb_cy, bar_w, thumb_h, th.scroll_thumb[0], th.scroll_thumb[1],
                    th.scroll_thumb[2], menu_fade_alpha(th.scroll_thumb[3], fade));  // bright thumb
  }
}

#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
// Relative luminance (0-255) with integer Rec.601-ish weights -- libm-free.
static inline int theme_luminance(int r, int g, int b) {
  return (54 * r + 183 * g + 19 * b) / 256;
}
// Soft-brick guard: if a text colour's luminance is too close to its row background, snap it to
// black or white so the row stays readable. The Theme Editor now lets the label / selected-label /
// status colours be set freely (incl. equal to the panel), so this is the safety net that keeps a
// bad custom palette from making the menu unusable. A pure post-step on the chosen colour.
static inline void ensure_text_contrast(int* r, int* g, int* b, int bg_lum) {
  const int kMinContrast = 70;
  const int d = theme_luminance(*r, *g, *b) - bg_lum;
  if (d >= kMinContrast || d <= -kMinContrast) return;  // already legible
  const int v = (bg_lum > 127) ? 0 : 255;               // light bg -> black text, dark bg -> white
  *r = v;
  *g = v;
  *b = v;
}
// Proportional text width in screen-space x units, summed from the same per-glyph advance table
// the toast pills measure with (kGlyphAdvance, 1/100 em). `scale` is the text scale_x; the 0.042
// em factor matches render_toasts' kEmWidth (calibrated so an average string reproduces the
// engine's layout). There is no text-measurement native on this lane, so this is the only ruler.
// A `~INPUT_*~` button-glyph token renders as a single inline glyph, not its literal characters, so
// the ruler must count one glyph advance per token instead of summing "~INPUT_FRONTEND_ACCEPT~"
// (21 chars). ~120/100 em ≈ a wide square button; HW-tunable with the bar's layout. No menu label
// contains a literal '~', so this only affects the instructional bar's glyph tokens.
static const uint32_t kGlyphTokenAdvance = 120u;
static float menu_text_width(const char* s, float scale) {
  uint32_t adv_sum = 0u;
  for (const char* p = s; *p; ++p) {
    if (*p == '~') {
      const char* q = p + 1;
      while (*q && *q != '~') ++q;  // skip to the matching close tilde
      adv_sum += kGlyphTokenAdvance;
      if (!*q) break;  // unterminated: stop (degenerate; never happens for our static tokens)
      p = q;           // loop's ++p steps past the close tilde
      continue;
    }
    const uint8_t c = (uint8_t)*p;
    adv_sum += (c < 128u) ? kGlyphAdvance[c] : 90u;
  }
  return (float)adv_sum * 0.01f * scale * 0.042f;
}
// Copy `src` into `dst` (cap bytes), appending a "..." ellipsis when it would exceed `max_w` at
// `scale`, so a long label can never lay out across the right-justified value column / panel edge
// (the engine has no clip-to-width for a single line). Fast-paths the common case (label fits ->
// verbatim copy). A degenerate budget yields just "..." rather than an overrun.
static const char* menu_truncate_to_width(char* dst, size_t cap, const char* src, float scale,
                                          float max_w) {
  if (cap == 0u) return dst;
  if (menu_text_width(src, scale) <= max_w) {
    snprintf(dst, cap, "%s", src);
    return dst;
  }
  const float ell_w = menu_text_width("...", scale);
  uint32_t adv_sum = 0u;
  size_t n = 0u;
  for (const char* p = src; *p && n + 4u < cap; ++p) {
    const uint8_t c = (uint8_t)*p;
    const uint32_t a = (c < 128u) ? kGlyphAdvance[c] : 90u;
    if ((float)(adv_sum + a) * 0.01f * scale * 0.042f + ell_w > max_w) break;
    adv_sum += a;
    ++n;
  }
  for (size_t k = 0; k < n; ++k) dst[k] = src[k];
  dst[n] = '.';
  dst[n + 1u] = '.';
  dst[n + 2u] = '.';
  dst[n + 3u] = '\0';
  return dst;
}
// Draw one visible menu row: the label (left, colour-coded selected/locked/normal) and its value
// (right, colour-coded by row type, bracketed "< value >" for a selected list). Factored out so the
// caller reads as setup -> background -> title -> rows -> footer.
static void draw_menu_row(const ShellItem* item, int selected, float text_x, float y,
                          float value_right, float fade, const MenuTheme& th) {
  const int locked = shell_item_is_unavailable(item);
  // Row background luminance for the contrast guard: the selection highlight sits behind the
  // focused row, the panel fill behind the rest. (An approximation -- ignores the highlight's alpha
  // + the faint zebra/locked washes -- but enough to catch a text==background custom palette.)
  const int bg_lum = selected ? theme_luminance(th.selection[0], th.selection[1], th.selection[2])
                              : theme_luminance(th.panel[0], th.panel[1], th.panel[2]);

  // Value (right, colour-coded) with a row-type affordance glyph so the row's KIND reads at a
  // glance (couch distance): toggles get a bracketed "[ ON ]"/"[ OFF ]" word, submenus a '>'
  // chevron, and list cyclers are always wrapped in "< >" (not only when selected). Locked rows win
  // first so a gated row never shows a misleading glyph. Built BEFORE the label so its width is
  // known and the label can be truncated to whatever space the value leaves (rather than laying out
  // over it).
  const char* base = selected_value_label(item);
  int vr = 165, vg = 178, vb = 196;  // muted default (ACT / STOP / HIDE)
  const char* value = base;
  char value_buf[56];
  if (base[0]) {
    if (locked) {
      vr = th.value_locked[0];
      vg = th.value_locked[1];
      vb = th.value_locked[2];
      // Prefix a [L] shape token so a locked row reads as locked without relying on colour (pairs
      // with the dimmed row band drawn behind it). Skip pure-info rows (their value is "").
      snprintf(value_buf, sizeof(value_buf), "[L] %s", base);
      value = value_buf;
    } else if (item->type == SHELL_ROW_TOGGLE) {
      const int on = (base[0] == 'O' && base[1] == 'N');
      if (on) {
        vr = th.toggle_on[0];
        vg = th.toggle_on[1];
        vb = th.toggle_on[2];
      } else {
        vr = th.toggle_off[0];
        vg = th.toggle_off[1];
        vb = th.toggle_off[2];
      }
      // Bracket the state word itself ("[ ON ]" / "[ OFF ]") rather than a separate "[x] ON"
      // checkbox: it reads cleaner and the word's own length/letters (plus the ON/OFF colour split)
      // carry the colourblind-safe shape redundancy the checkbox used to.
      snprintf(value_buf, sizeof(value_buf), "[ %s ]", base);
      value = value_buf;
    } else if (item->type == SHELL_ROW_LIST) {
      vr = th.value_list[0];
      vg = th.value_list[1];
      vb = th.value_list[2];
      snprintf(value_buf, sizeof(value_buf), "< %s >", base);
      value = value_buf;
    } else if (item->type == SHELL_ROW_SUBMENU) {
      // Lighten the theme accent so submenu "OPEN" reads distinctly from plain ACT/STOP, and
      // append a chevron -- the universal "drills into a screen" signal.
      vr = th.accent[0] + (255 - th.accent[0]) / 2;
      vg = th.accent[1] + (255 - th.accent[1]) / 2;
      vb = th.accent[2] + (255 - th.accent[2]) / 2;
      snprintf(value_buf, sizeof(value_buf), "%s >", base);
      value = value_buf;
    }
  }
  const float value_w = base[0] ? menu_text_width(value, 0.30f) : 0.0f;

  // Label (left). Selected = warm highlight; locked = grey; else off-white.
  NativeTextStyle label_style = {0, 0.30f, 0.30f, 220, 224, 232, 230, 0, 0.0f, 1.0f, 0, 1};
  if (selected) {
    label_style.r = th.sel_label[0];
    label_style.g = th.sel_label[1];
    label_style.b = th.sel_label[2];
    label_style.a = 245;
  } else if (locked) {
    label_style.r = 140;
    label_style.g = 144;
    label_style.b = 150;
    label_style.a = 200;
  }
  ensure_text_contrast(&label_style.r, &label_style.g, &label_style.b, bg_lum);
  label_style.a = menu_fade_alpha(label_style.a, fade);
  // Fit the label into the space the value column leaves. It stays at the base scale when it
  // already fits; otherwise it SHRINKS toward a readability floor so the FULL text still shows
  // (mod-menu style) instead of ellipsizing while half the row is still empty. Only a label too
  // long even at the floor scale is then ellipsized. menu_text_width is proportional, so the fit
  // factor is just avail/measured, and a label that already fits keeps scale 0.30 (fast path).
  const float kColGap = 0.010f;
  const float label_avail = (base[0] ? (value_right - value_w - kColGap) : value_right) - text_x;
  const float kLabelBaseScale = 0.30f;
  const float kLabelMinScale = 0.225f;  // ~75% of base -> ~1/3 more glyphs before any ellipsis
  float label_scale = kLabelBaseScale;
  const float full_w = menu_text_width(item->label, kLabelBaseScale);
  if (label_avail > 0.0f && full_w > label_avail) {
    label_scale = kLabelBaseScale * (label_avail / full_w);
    if (label_scale < kLabelMinScale) label_scale = kLabelMinScale;
  }
  label_style.scale_x = label_scale;
  label_style.scale_y = label_scale;
  // GTA text y is the glyph TOP; nudge a shrunk label down so it stays vertically centred on the
  // row (no-op at the base scale).
  const float label_y = y + GTAV_MENU_PANEL_GLYPH_MID * (1.0f - label_scale / kLabelBaseScale);
  char label_buf[64];
  const char* label =
      menu_truncate_to_width(label_buf, sizeof(label_buf), item->label, label_scale, label_avail);
  draw_native_text_shadowed(label, text_x, label_y, label_style);
  // Faux-bold the focused row's label: one extra non-shadowed pass nudged a hair right thickens the
  // glyphs, so focus reads by WEIGHT (not colour alone) -- helps at couch distance / low vision and
  // is colourblind-safe. Static (not animation), so it is independent of Reduce Motion.
  if (selected) {
    draw_native_text_line(label, text_x + 0.0006f, label_y, label_style);
  }

  if (base[0]) {
    // Value-change flash: briefly wash the selected row's value toward white when a toggle flips
    // or a cycler steps, so the change registers at a glance. g_last_worker_tick == the render
    // tick here, so this stays a pure function of the tick counters (over-render safe).
    if (selected) {
      const float vf = flash_decay(g_last_worker_tick, g_value_flash_tick);
      if (vf > 0.0f) {
        vr += (int)((float)(255 - vr) * vf);
        vg += (int)((float)(255 - vg) * vf);
        vb += (int)((float)(255 - vb) * vf);
      }
    }
    // Same soft-brick guard as the label: the value colours (toggle/list/locked) are user-editable
    // now, so keep them legible against the row background.
    ensure_text_contrast(&vr, &vg, &vb, bg_lum);
    NativeTextStyle value_style = {0, 0.30f,  0.30f,       vr, vg, vb, selected ? 245 : 220,
                                   0, text_x, value_right, 2,  1};
    value_style.a = menu_fade_alpha(value_style.a, fade);
    draw_native_text_shadowed(value, text_x, y, value_style);
  }
}

#if GTAV_MENU_ENABLE_BUTTON_GLYPHS && GTAV_MENU_ENABLE_NATIVE_FEATURES
// Try to draw the real controller-button glyph sprite for `gi` centred in the chip box. Returns 1
// if a sprite was drawn (caller then skips the coloured chip), 0 to fall back to the chip. Mirrors
// the vehicle-preview load handshake: request the dict while it streams, draw only once resident.
// An empty mapping or a missing native short-circuits to the chip, so this can never render a white
// "missing texture" quad.
static int draw_button_glyph_sprite(const HintGlyphInfo* gi, float cx, float cy, float w, float h,
                                    float fade) {
  if (!gi->txd[0] || !gi->tex[0]) return 0;
  const uint64_t ds = g_table.draw_sprite;
  const uint64_t req = g_table.request_streamed_texture_dict;
  const uint64_t loaded_fn = g_table.has_streamed_texture_dict_loaded;
  if (!ds || !req || !loaded_fn) return 0;
  if (!invoke_return<int>(loaded_fn, gi->txd)) {
    invoke_void(req, gi->txd, 0);  // stream it in; chip covers this frame
    return 0;
  }
  invoke_void(ds, gi->txd, gi->tex, cx, cy, w, h, 0.0f, 255, 255, 255, menu_fade_alpha(255, fade),
              0, 0);
  return 1;
}
#endif

#if GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && GTAV_MENU_ENABLE_NATIVE_FEATURES
// --- Native Scaleform bar feed (Route B) ---------------------------------------------------------
// A single per-tick arbiter (publish_active_instructional_bar) picks the source and publishes its
// button set to GTA's instructional_buttons Scaleform: an active custom-control MODE (object move /
// free cam -- both hide the menu) takes priority, else the focused menu ROW, else nothing (count 0
// hides the bar). Each entry is {control id -> real glyph via GET_CONTROL_INSTRUCTIONAL_BUTTONS_
// STRING, "~INPUT_*~" token fallback, verb label}; the game-thread tick draws whatever is published
// (instructional_buttons.inc), so the bar follows the active mode even while the panel is hidden.
enum { kInstrBarMax = 6 };  // matches GTAV_INSTR_MAX_BUTTONS; sizes the publish scratch arrays

// Control ids the mode bars need beyond the menu-row face/bumper set (sticks read group-0 gameplay
// ids; their glyphs are best-effort -- the text legend carries them if a glyph is absent).
enum {
  GTAV_INPUT_MOVE_LR = 30,  // left stick (horizontal)
  GTAV_INPUT_LOOK_LR = 1,   // right stick (horizontal)
  GTAV_INPUT_SPRINT = 21,   // Sprint / L3
  GTAV_INPUT_JUMP = 22,     // Jump (Cross on the pad)
};

static void instr_set(GtavInstructionalButton* b, int c, int c2, const char* tok,
                      const char* label) {
  b->control = c;
  b->control2 = c2;
  b->glyph = tok;
  b->label = label;
}

// Map the menu-row hint buttons (HintGlyph + verb) to the publish struct via kHintGlyphInfo.
static uint32_t fill_instr_from_hints(const HintButton* btns, uint32_t n,
                                      GtavInstructionalButton* out) {
  if (n > (uint32_t)kInstrBarMax) n = (uint32_t)kInstrBarMax;
  for (uint32_t i = 0; i < n; ++i) {
    const HintGlyphInfo* gi = &kHintGlyphInfo[btns[i].glyph];
    instr_set(&out[i], gi->ctrl_a, gi->ctrl_b, gi->glyph_token, btns[i].label);
  }
  return n;
}

// Object Spawner Editor: the reliable button actions (Place/Cancel + face/bumper/trigger verbs).
// The sticks (move/camera), d-pad select, L3 delete and the live pose stay in the text legend
// (render_object_move_overlay). Curated to <= kInstrBarMax.
static uint32_t build_object_move_instr_buttons(const GtavObjectMoveReadout* mv,
                                                GtavInstructionalButton* out) {
  uint32_t n = 0;
  instr_set(&out[n++], GTAV_INPUT_FRONTEND_ACCEPT, -1, "~INPUT_FRONTEND_ACCEPT~", "Place");
  if (mv->aim_mode) {
    instr_set(&out[n++], GTAV_INPUT_FRONTEND_LB, GTAV_INPUT_FRONTEND_RB,
              "~INPUT_FRONTEND_LB~~INPUT_FRONTEND_RB~", "Distance");
  } else {
    instr_set(&out[n++], GTAV_INPUT_FRONTEND_LB, GTAV_INPUT_FRONTEND_RB,
              "~INPUT_FRONTEND_LB~~INPUT_FRONTEND_RB~", "Spin");
    instr_set(&out[n++], GTAV_INPUT_FRONTEND_LT, GTAV_INPUT_FRONTEND_RT,
              "~INPUT_FRONTEND_LT~~INPUT_FRONTEND_RT~", "Height");
    instr_set(&out[n++], GTAV_INPUT_FRONTEND_X, -1, "~INPUT_FRONTEND_X~", "Ground");
    instr_set(&out[n++], GTAV_INPUT_FRONTEND_Y, -1, "~INPUT_FRONTEND_Y~", "Dup");
  }
  instr_set(&out[n++], GTAV_INPUT_FRONTEND_CANCEL, -1, "~INPUT_FRONTEND_CANCEL~", "Cancel");
  return n;
}

// Free Camera: stick move/look + up + boost + exit. Stick glyphs are best-effort (token fallback);
// Circle Exit is the reliable anchor. Down (Duck) + speed stay in the free-cam text HUD.
static uint32_t build_free_cam_instr_buttons(GtavInstructionalButton* out) {
  uint32_t n = 0;
  instr_set(&out[n++], GTAV_INPUT_MOVE_LR, -1, "~INPUT_MOVE_LR~", "Move");
  instr_set(&out[n++], GTAV_INPUT_LOOK_LR, -1, "~INPUT_LOOK_LR~", "Look");
  instr_set(&out[n++], GTAV_INPUT_JUMP, -1, "~INPUT_JUMP~", "Up");
  instr_set(&out[n++], GTAV_INPUT_SPRINT, -1, "~INPUT_SPRINT~", "Boost");
  instr_set(&out[n++], GTAV_INPUT_FRONTEND_CANCEL, -1, "~INPUT_FRONTEND_CANCEL~", "Exit");
  return n;
}

// Per-tick arbiter: pick the active source and publish its set (count 0 hides the bar). Runs every
// worker tick regardless of menu visibility, so a menu-hiding mode still drives the native bar.
static void publish_active_instructional_bar(int visible) {
  GtavInstructionalButton out[kInstrBarMax];
  uint32_t n = 0;
  GtavObjectMoveReadout mv;
  if (gtav_features_object_move_readout(&mv) && mv.active) {
    n = build_object_move_instr_buttons(&mv, out);
  } else if (gtav_features_free_cam_active()) {
    n = build_free_cam_instr_buttons(out);
  } else if (visible) {
    HintButton btns[4];
    const uint32_t hn = build_hint_buttons(selected_item(), btns, 4u);
    n = fill_instr_from_hints(btns, hn, out);
  }
  gtav_features_instructional_publish(n, out);
}
#endif

// Native-style instructional-button bar along the bottom-right of the screen: the focused row's
// (glyph, verb) pairs, right-aligned so the strip ends at a fixed margin like GTA's own
// instructional buttons. Three tiers, best-available:
//   - Route B (GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM): GTA's real instructional_buttons
//     Scaleform, drawn on the game thread. When it is up this worker-drawn bar stands down
//     (gtav_features_instructional_active) so the two never stack -- we only PUBLISH the buttons.
//   - Route A (GTAV_MENU_ENABLE_BUTTON_GLYPHS): real inline `~INPUT_*~` glyphs, no chip rect.
//   - Fallback: coloured PlayStation-style ASCII chips (always safe).
// Fades in lockstep with the panel (over-render safe -- pure function of the tick).
[[maybe_unused]] static void draw_instructional_bar(uint64_t worker_tick, const ShellItem* sel) {
  const uint64_t dr = g_table.draw_rect;
  if (!dr || !text_native_ready()) return;
  const float fade = menu_open_fade(worker_tick);
  if (fade <= 0.0f) return;

  HintButton btns[4];
  const uint32_t n = build_hint_buttons(sel, btns, 4u);
  if (n == 0u) return;

#if GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && GTAV_MENU_ENABLE_NATIVE_FEATURES
  // The per-tick arbiter (publish_active_instructional_bar) owns feeding the Scaleform now; here we
  // only stand this worker fallback bar down once GTA's own bar is drawing so the two never stack.
  if (gtav_features_instructional_active()) return;
#endif

#if GTAV_MENU_ENABLE_BUTTON_GLYPHS
  const int use_inline_glyphs = 1;  // draw the real glyph inline, no chip rect behind it
#else
  const int use_inline_glyphs = 0;
#endif

  // Layout (screen-normalised). The bar hugs the bottom-right corner.
  const float y_cy = 0.955f;        // vertical centre of the bar
  const float right_edge = 0.952f;  // entries end here (matches the panel's right-edge margin)
  const float chip_h = 0.030f;
  const float chip_pad_x = 0.006f;  // padding inside a chip around its token
  const float chip_gap = 0.006f;    // glyph -> its label
  const float entry_gap = 0.016f;   // between entries
  const float label_scale = 0.28f;
  const float token_scale = 0.26f;
  const float glyph_mid = 0.011f;  // half glyph height -> vertical text centring

  // Per-entry the "glyph cell" is either the real inline glyph token (Route A) or the ASCII chip
  // text. Measure first so the whole bar can be right-aligned; a cell never goes narrower than tall
  // so a single glyph still reads as a square button (and a future sprite has a square box).
  float chip_w[4];
  float label_w[4];
  float total_w = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const HintGlyphInfo* gi = &kHintGlyphInfo[btns[i].glyph];
    const char* cell = use_inline_glyphs ? gi->glyph_token : gi->token;
    float cw = menu_text_width(cell, token_scale) + (use_inline_glyphs ? 0.0f : chip_pad_x * 2.0f);
    if (cw < chip_h) cw = chip_h;
    chip_w[i] = cw;
    label_w[i] = menu_text_width(btns[i].label, label_scale);
    total_w += cw + chip_gap + label_w[i];
    if (i + 1u < n) total_w += entry_gap;
  }

  // Backing strip behind the whole bar so it reads over a bright scene (subtle, like the toast
  // pills). Spans the measured width plus a little breathing room.
  const float strip_pad = 0.010f;
  const float strip_w = total_w + strip_pad * 2.0f;
  const float strip_cx = right_edge - total_w * 0.5f;
  draw_shell_rect(dr, strip_cx, y_cy, strip_w, chip_h + 0.014f, 0, 0, 0,
                  menu_fade_alpha(120, fade));

  float x = right_edge - total_w;  // left edge of the first glyph cell
  for (uint32_t i = 0; i < n; ++i) {
    const HintGlyphInfo* gi = &kHintGlyphInfo[btns[i].glyph];
    const float cw = chip_w[i];
    const float chip_cx = x + cw * 0.5f;

    if (use_inline_glyphs) {
      int drew = 0;
#if GTAV_MENU_ENABLE_BUTTON_GLYPHS && GTAV_MENU_ENABLE_NATIVE_FEATURES
      // A verified sprite (if ever filled into kHintGlyphInfo) wins; otherwise the inline glyph
      // token, which GTA's text formatter substitutes for the controller's real button glyph.
      drew = draw_button_glyph_sprite(gi, chip_cx, y_cy, cw, chip_h, fade);
#endif
      if (!drew) {
        NativeTextStyle g = {0, token_scale, token_scale, 255, 255, 255, 0, 1, 0.0f, 1.0f, 0, 1};
        g.a = menu_fade_alpha(255, fade);
        draw_native_text_shadowed(gi->glyph_token, chip_cx, y_cy - glyph_mid, g);
      }
    } else {
      // Chip: a filled rect in the button's PlayStation colour (dark outline behind) with the token
      // centred in white. DRAW_RECT only gives square corners, but at this size it reads as a
      // button.
      draw_shell_rect(dr, chip_cx, y_cy, cw + 0.0035f, chip_h + 0.0045f, 0, 0, 0,
                      menu_fade_alpha(190, fade));
      draw_shell_rect(dr, chip_cx, y_cy, cw, chip_h, gi->r, gi->g, gi->b,
                      menu_fade_alpha(235, fade));
      NativeTextStyle tok = {0, token_scale, token_scale, 255, 255, 255, 0, 1, 0.0f, 1.0f, 0, 1};
      tok.a = menu_fade_alpha(245, fade);
      draw_native_text_shadowed(gi->token, chip_cx, y_cy - glyph_mid, tok);
    }

    // Verb label to the right of the glyph.
    NativeTextStyle lab = {0, label_scale, label_scale, 232, 238, 248, 0, 0, 0.0f, 1.0f, 0, 1};
    lab.a = menu_fade_alpha(235, fade);
    draw_native_text_shadowed(btns[i].label, x + cw + chip_gap, y_cy - glyph_mid, lab);

    x += cw + chip_gap + label_w[i] + entry_gap;
  }
}
#endif

static void draw_worker_text_menu_list(uint64_t worker_tick) {
#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
  const ShellMenu* menu = shell_menu(g_current_menu);
  const uint32_t start = shell_window_start();
  const uint32_t visible_count = shell_visible_count();
  const uint32_t total = shell_item_count();
  const uint32_t selected_index = g_snapshot.selected_index;
  char line[96];

  // Geometry (kept in sync with draw_menu_background) + the open fade/slide so the text
  // animates in lockstep with the chrome.
  const MenuTheme& th = active_theme();
  const float fade = menu_open_fade(worker_tick);
  const float y_off = (1.0f - fade) * 0.012f;
  const float text_x = menu_panel_text_x();
  const float panel_left = text_x - GTAV_MENU_PANEL_PAD_L;
  const float panel_w = menu_panel_width();
  const float value_right = panel_left + panel_w - GTAV_MENU_PANEL_VALUE_PAD;
  const float row0_y = GTAV_MENU_PANEL_ROW0_Y + y_off;
  const float row_h = GTAV_MENU_PANEL_ROW_H;

  const uint32_t selected_row = selected_index >= start ? selected_index - start : 0u;
  draw_menu_background(worker_tick, visible_count, selected_row);

  // Title: a breadcrumb path so deep navigation stays legible. Depth 0 (root) is just the menu
  // label. At depth >= 2 prefer the FULL ancestor path (Top > ... > Current) for orientation --
  // e.g. so "City Landmarks" reads as "Self > Teleport > City Landmarks", not just the ambiguous
  // "Teleport > City Landmarks". To stay within the panel width the title already tolerates, the
  // full path is used only when it is no longer than the two-label form (~two longest menu labels
  // + " > "); anything longer falls back to the original always-fits "Parent > Current".
  char crumb[96];
  if (g_menu_depth == 0u) {
    snprintf(crumb, sizeof(crumb), "%s", menu->label);
  } else {
    const ShellMenu* parent = shell_menu(g_menu_stack[g_menu_depth - 1u]);
    const size_t kCrumbMaxChars = 36u;  // ~longest 2-label crumb; bounds the title to today's width
    char full[96];
    size_t n = 0;
    for (uint32_t i = 0; i < g_menu_depth && n < sizeof(full); ++i) {
      const ShellMenu* anc = shell_menu(g_menu_stack[i]);
      n += (size_t)snprintf(full + n, sizeof(full) - n, "%s > ", anc->label);
    }
    if (n < sizeof(full)) n += (size_t)snprintf(full + n, sizeof(full) - n, "%s", menu->label);
    if (n <= kCrumbMaxChars) {
      snprintf(crumb, sizeof(crumb), "%s", full);
    } else {
      snprintf(crumb, sizeof(crumb), "%s > %s", parent->label, menu->label);
    }
  }
  NativeTextStyle title_style = {0, 0.32f, 0.32f, 235, 242, 255, 245, 0, 0.0f, 1.0f, 0, 0};
  title_style.a = menu_fade_alpha(title_style.a, fade);
  draw_native_text_shadowed(crumb, text_x, GTAV_MENU_PANEL_TITLE_Y + y_off, title_style);

  for (uint32_t row = 0; row < visible_count; ++row) {
    const uint32_t index = start + row;
    const ShellItem* item = &menu->items[index];
    const int selected = index == selected_index;
    const float y = row0_y + row_h * (float)row;
    draw_menu_row(item, selected, text_x, y, value_right, fade, th);
  }

  // Footer: selected item's description (left) + position counter (right). On the dark footer
  // strip, so it needs no shadow -- just fade with the panel.
  const float footer_y = row0_y + row_h * (float)visible_count + 0.010f;
  const ShellItem* sel = (selected_index < total) ? &menu->items[selected_index] : nullptr;
  NativeTextStyle desc_style = {0, 0.235f, 0.235f, 185, 205, 235, 225, 0, 0.0f, 1.0f, 0, 1};
  desc_style.a = menu_fade_alpha(desc_style.a, fade);
  const char* desc = item_description(sel);
  if (desc[0]) {
    draw_native_text_line(desc, text_x, footer_y, desc_style);
  }
  if (total) {
    snprintf(line, sizeof(line), "%u / %u", (unsigned)(selected_index + 1u), (unsigned)total);
    NativeTextStyle counter_style = {0,   0.235f, 0.235f, 185,         205, 235,
                                     225, 0,      text_x, value_right, 2,   1};
    counter_style.a = menu_fade_alpha(counter_style.a, fade);
    draw_native_text_line(line, text_x, footer_y, counter_style);
  }

  // The per-row controller-verb legend now renders as a native-style instructional-button bar along
  // the bottom-right of the screen (draw_instructional_bar), called from worker_text_overlay.
#else
  (void)worker_tick;
  draw_worker_text_status_line();
#endif
}

#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
// Emit a preview-state status event, but only when the state actually changes (model / dict /
// texture / loaded / drawn), so the log shows each distinct hover + transition without flooding.
// The "reason" makes a blank card self-explanatory on hardware: no-mapping (vehicle absent from
// the preview registry), streaming (mapped dict not yet resident), or drawn.
static void report_vehicle_preview(const char* model, const char* dict, const char* texture,
                                   int loaded, int drawn) {
  const char* reason = drawn ? "drawn" : (!dict[0] ? "no-mapping" : "streaming");
  char crumb[160];
  snprintf(crumb, sizeof(crumb), "model=%s dict=%s tex=%s loaded=%d %s", model[0] ? model : "-",
           dict[0] ? dict : "-", texture[0] ? texture : "-", loaded, reason);
  static char last[160];
  if (strncmp(crumb, last, sizeof(last)) == 0) return;
  snprintf(last, sizeof(last), "%s", crumb);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "vehicle preview %s", crumb);
}

// Vehicle preview card: a 16:9 thumbnail of the highlighted vehicle, drawn beside the menu
// panel. On a 16:9 screen, equal normalized width/height yields a 16:9 image in pixels
// (img_w*screenW / img_h*screenH == screenW/screenH == 16/9). Shows the streamed sprite once the
// game-thread streamer reports the dict resident AND the vehicle has a verified registry mapping;
// otherwise a placeholder card (no mapping, or the dict still streaming). Caption below the card.
static void draw_vehicle_preview(uint64_t worker_tick) {
  if (!g_preview_model[0]) return;  // set only while hovering a spawn row
  const uint64_t dr = g_table.draw_rect;
  if (!dr) return;  // need chrome for the card/placeholder backing
  const MenuTheme& th = active_theme();
  const float fade = menu_open_fade(worker_tick);
  const float text_x = menu_panel_text_x();
  const float panel_left = text_x - GTAV_MENU_PANEL_PAD_L;
  const float panel_w = menu_panel_width();
  const float img_w = 0.20f;  // == img_h -> 16:9 in pixels on a 16:9 screen
  const float img_h = 0.20f;
  const float gap = 0.014f;
  // Sit beside the panel on whichever side has room: a left-anchored panel puts the preview to
  // its right, the default right-anchored panel puts it to the left.
  const float img_cx = (text_x < 0.5f) ? (panel_left + panel_w + gap + img_w * 0.5f)
                                       : (panel_left - gap - img_w * 0.5f);
  const float img_top = GTAV_MENU_PANEL_ROW0_Y;  // top-align with the first menu row
  const float img_cy = img_top + img_h * 0.5f;
  // Backing border + card.
  draw_shell_rect(dr, img_cx, img_cy, img_w + 0.006f, img_h + 0.008f, 0, 0, 0,
                  menu_fade_alpha(170, fade));
  // Draw the sprite ONLY for a verified mapping whose dict is resident: a mapped entry guarantees
  // a non-empty (dict, texture) from the registry, and the streamer confirms the dict loaded.
  // Without a mapping we never call DRAW_SPRITE with a guessed model/model pair (that renders the
  // white card), so unmapped vehicles fall back cleanly to the placeholder.
  const uint64_t ds = g_table.draw_sprite;
  const char* dict = gtav_features_preview_dict();
  const char* texture = gtav_features_preview_texture();
  const int loaded = gtav_features_preview_texture_loaded();
  // Draw ONLY when the game thread confirmed (via GET_TEXTURE_RESOLUTION) that this exact
  // (dict, texture) actually exists with a non-zero size. A model-named dict can load yet not
  // contain the texture we ask for (res 0x0) -> DRAW_SPRITE would render a white quad. Gating on
  // a real resolution makes the white card structurally impossible and the registry
  // self-correcting: a wrong (dict, texture) row simply falls back to the placeholder.
  const int has_texture = gtav_features_preview_res_w() > 0u;
  const int draw_sprite = ds && g_preview_entry && loaded && has_texture && dict[0] && texture[0];
  if (draw_sprite) {
    // DRAW_SPRITE(dict, name, cx, cy, w, h, heading, r, g, b, a, p11, p12). dict and name come
    // from the verified registry and are independent (name need not equal dict).
    draw_shell_sprite(ds, dict, texture, img_cx, img_cy, img_w, img_h, 0.0f, 255, 255, 255,
                      menu_fade_alpha(255, fade), 0, 0);
  } else {
    // Placeholder while a mapped dict streams in, or permanently for a vehicle with no mapping.
    draw_shell_rect(dr, img_cx, img_cy, img_w, img_h, th.panel[0], th.panel[1], th.panel[2],
                    menu_fade_alpha(th.panel[3], fade));
    if (text_native_ready()) {
      NativeTextStyle ph = {0, 0.30f, 0.30f, 190, 198, 210, menu_fade_alpha(210, fade),
                            1, 0.0f,  1.0f,  0,   1};
      draw_native_text_shadowed("No preview", img_cx, img_cy - 0.012f, ph);
      // Mapped vehicle whose (dict, texture) did not resolve on this build (e.g. a Legacy->Enhanced
      // dict rename): show the dict name so it can be corrected in the TSV. Never shown for
      // unmapped rows or working previews, so it stays out of the way in the common cases.
      // (Investigation aid.)
      if (g_preview_entry && dict[0]) {
        NativeTextStyle miss = {0, 0.20f, 0.20f, 235, 170, 90, menu_fade_alpha(220, fade),
                                1, 0.0f,  1.0f,  0,   1};
        draw_native_text_shadowed(dict, img_cx, img_cy + 0.020f, miss);
      }
    }
  }
  // Caption: the registry caption (vehicle label) under the card, falling back to the model name.
  if (text_native_ready()) {
    const char* caption = gtav_features_preview_caption();
    if (!caption[0]) caption = g_preview_model;
    NativeTextStyle cap = {0, 0.26f, 0.26f, 225, 230, 240, menu_fade_alpha(235, fade),
                           1, 0.0f,  1.0f,  0,   1};
    draw_native_text_shadowed(caption, img_cx, img_cy + img_h * 0.5f + 0.006f, cap);
  }
  // Debug breadcrumb (worker-side status is allowed): report the preview state whenever it
  // changes, so on-hardware verification can see model / dict / texture / loaded and WHY a card
  // is blank (no registry mapping vs the mapped dict not yet streamed vs drawn).
  report_vehicle_preview(g_preview_model, dict, texture, loaded, draw_sprite);
}
#endif

static void worker_text_overlay(uint64_t worker_tick, int visible) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
  if (!visible)
    gtav_render_diag_skip(GTAV_RD_SKIP_HIDDEN);
  else if (!text_native_ready())
    gtav_render_diag_skip(GTAV_RD_SKIP_ADDRESS);
  else if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0)
    gtav_render_diag_skip(GTAV_RD_SKIP_INTERVAL);
#endif
  if (!visible) return;
  if (!text_native_ready()) return;
  // No per-frame draw cap on the VISIBLE panel: an open menu must be re-submitted every
  // eligible tick or GTA's one-shot text buffer leaves a blank frame (reads as flicker). An
  // earlier per-tick draw budget predated the proven over-render lane and could starve a
  // visible menu to nothing, so it was removed; the panel re-renders every eligible tick.
  // Runtime-tunable cadence (see g_render_interval / SET_RENDER_INTERVAL).
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;

  if (!g_reported_worker_text_overlay) {
    g_reported_worker_text_overlay = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
                       "worker text menu first draw begin=0x%llx add=0x%llx end=0x%llx",
#else
                       "worker text overlay first draw begin=0x%llx add=0x%llx end=0x%llx",
#endif
                       (unsigned long long)g_table.begin_text_command_display_text,
                       (unsigned long long)g_table.add_text_component_substring_player_name,
                       (unsigned long long)g_table.end_text_command_display_text);
  }

  draw_worker_text_menu_list(worker_tick);
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
  draw_vehicle_preview(worker_tick);
#endif
#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
  // Native-style instructional-button bar (bottom-right): the focused row's controller verbs.
  draw_instructional_bar(worker_tick, selected_item());
#endif

  ++g_worker_text_overlay_draws;
  if (g_worker_text_overlay_draws <= 3u || (g_worker_text_overlay_draws % 60u) == 0) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
#if GTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST
                       "worker text menu draw=%u item=%s",
#else
                       "worker text overlay draw=%u item=%s",
#endif
                       g_worker_text_overlay_draws, selected_label());
  }
}
#else
static void worker_text_overlay(uint64_t worker_tick, int visible) {
  (void)worker_tick;
  (void)visible;
}
#endif

// Estimate the game frame rate from GET_FRAME_COUNT / GET_GAME_TIMER deltas, refreshed
// about twice a second so the readout is steady rather than jittery. Returns 0 until it
// has two samples (or when the timer natives are absent).
[[maybe_unused]] static float hud_estimate_fps() {
  static uint32_t s_last_ms;
  static uint32_t s_last_frames;
  static float s_fps;
  if (!g_table.get_frame_count || !g_table.get_game_timer) return 0.0f;
  uint32_t now_ms = invoke_return<uint32_t>(g_table.get_game_timer);
  uint32_t frames = invoke_return<uint32_t>(g_table.get_frame_count);
  if (s_last_ms == 0u) {
    s_last_ms = now_ms;
    s_last_frames = frames;
    return 0.0f;
  }
  uint32_t dms = now_ms - s_last_ms;
  if (dms >= 500u) {
    uint32_t dframes = frames - s_last_frames;
    if (dms > 0u) s_fps = (float)dframes * 1000.0f / (float)dms;
    s_last_ms = now_ms;
    s_last_frames = frames;
  }
  return s_fps;
}

// Draw the HUD info overlay (speed/coords/heading/FPS) at the top-left, using the same
// proven text lane as the menu. Runs every render tick REGARDLESS of menu visibility,
// so it stays up while the panel is closed. Self-guards: no text natives or HUD master
// off -> no-op. Drawing is independent of the menu panel so the two never clip.
[[maybe_unused]] static void render_hud_overlay(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  GtavHudReadout hud;
  if (!gtav_features_hud_readout(&hud)) return;  // master toggle off

  const float x = 0.015f;
  float y = 0.020f;
  const float line_h = 0.028f;
  char line[64];
  // Off-white, drop-shadowed, left-justified -- legible over arbitrary scenes.
  NativeTextStyle style = {0, 0.32f, 0.32f, 235, 242, 255, 245, 0, 0.0f, 1.0f, 0, 1};

  if (hud.show_speedo && hud.in_vehicle) {
    snprintf(line, sizeof(line), "%d km/h", (int)(hud.speed_kmh + 0.5f));
    draw_native_text_line(line, x, y, style);
    y += line_h;
  }
  if (hud.show_coords) {
    snprintf(line, sizeof(line), "X %.1f  Y %.1f  Z %.1f", hud.x, hud.y, hud.z);
    draw_native_text_line(line, x, y, style);
    y += line_h;
    snprintf(line, sizeof(line), "HDG %d", (int)(hud.heading + 0.5f));
    draw_native_text_line(line, x, y, style);
    y += line_h;
  }
  if (hud.show_fps) {
    float fps = hud_estimate_fps();
    if (fps > 0.0f) {
      snprintf(line, sizeof(line), "FPS %d", (int)(fps + 0.5f));
      draw_native_text_line(line, x, y, style);
      y += line_h;
    }
  }
  if (hud.show_distance) {
    if (hud.has_waypoint) {
      snprintf(line, sizeof(line), "WP %dm", (int)(hud.distance_m + 0.5f));
    } else {
      snprintf(line, sizeof(line), "WP --");
    }
    draw_native_text_line(line, x, y, style);
    y += line_h;
  }
#else
  (void)worker_tick;
#endif
}

// Draw the Zombie Outbreak survival HUD: a compact wave/kills/remaining stack at the top-right
// (clear of the top-left info overlay), plus a centred banner during the wave-cleared breather
// and on game over. Same proven text lane as the info HUD; runs every render tick regardless of
// menu visibility, self-guards on text natives, and only draws while Zombie Outbreak is the
// active minigame. Worker-safe: reads the survival snapshot, never a getter native.
[[maybe_unused]] static void render_zombie_hud(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  GtavZombieReadout z;
  if (!gtav_features_zombie_readout(&z) || !z.active) return;

  // Top-right stack, right-justified to the screen edge (wrap_start..wrap_end = 0.70..0.985).
  const float wrap_start = 0.70f;
  const float wrap_end = 0.985f;
  float y = 0.020f;
  const float line_h = 0.030f;
  char line[64];
  // Blood-red title, then off-white stat lines -- both drop-shadowed for legibility.
  const NativeTextStyle title = {0, 0.42f, 0.42f, 220, 50, 50, 245, 0, wrap_start, wrap_end, 2, 1};
  const NativeTextStyle stat = {0, 0.34f, 0.34f, 235, 242, 255, 245, 0, wrap_start, wrap_end, 2, 1};
  draw_native_text_line("ZOMBIE OUTBREAK", wrap_start, y, title);
  y += line_h;
  snprintf(line, sizeof(line), "WAVE %u", z.wave);
  draw_native_text_line(line, wrap_start, y, stat);
  y += line_h;
  snprintf(line, sizeof(line), "KILLS %u", z.kills);
  draw_native_text_line(line, wrap_start, y, stat);
  y += line_h;
  snprintf(line, sizeof(line), "LEFT %u", z.remaining);
  draw_native_text_line(line, wrap_start, y, stat);

  // Centred banners for the between-wave breather and game over (SET_TEXT_CENTRE at x = 0.5).
  if (z.state == GTAV_ZOMBIE_STATE_INTERMISSION) {
    const NativeTextStyle banner = {0, 0.70f, 0.70f, 255, 190, 60, 245, 1, 0.0f, 1.0f, 0, 1};
    snprintf(line, sizeof(line), "WAVE %u CLEARED", z.wave);
    draw_native_text_line(line, 0.5f, 0.150f, banner);
  } else if (z.state == GTAV_ZOMBIE_STATE_GAMEOVER) {
    const NativeTextStyle banner = {0, 0.95f, 0.95f, 220, 40, 40, 250, 1, 0.0f, 1.0f, 0, 1};
    const NativeTextStyle sub = {0, 0.50f, 0.50f, 235, 242, 255, 245, 1, 0.0f, 1.0f, 0, 1};
    draw_native_text_line("GAME OVER", 0.5f, 0.420f, banner);
    snprintf(line, sizeof(line), "SURVIVED %u WAVES", z.wave);
    draw_native_text_line(line, 0.5f, 0.520f, sub);
  }
#else
  (void)worker_tick;
#endif
}

// Read-only diagnostics overlay (TOGGLE_DEBUG_OVERLAY): frame-hook health, job-queue
// throughput, resolved-native count, and the explosive/fire-ammo counters. Like the HUD
// overlay it draws independently of the menu panel, so you can toggle it on, CLOSE the
// menu, fire, and watch the weapon-fx counters climb -- the explosive-ammo diagnostic
// workflow. Drawn lower-left to sit clear of a typical HUD. Self-guards on text natives.
[[maybe_unused]] static void render_debug_overlay(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  GtavDebugStats st;
  if (!gtav_features_debug_stats(&st)) return;  // overlay toggle off

  const float x = 0.015f;
  float y = 0.300f;
  const float line_h = 0.026f;
  char line[64];
  NativeTextStyle style = {0, 0.30f, 0.30f, 235, 242, 255, 245, 0, 0.0f, 1.0f, 0, 1};

  draw_native_text_line("-- DEBUG --", x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "hook %s calls %u", st.hook_active ? "live" : "off", st.hook_calls);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "jobs run %u drop %u", st.jobs_run, st.jobs_dropped);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "natives %u ready %u", st.native_count, st.native_ready);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "ammo tick %u hit %u fx %u", st.wfx_tick_fired, st.wfx_impact_hit,
           st.wfx_effect_requested);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  // Impact coord the ammo tick last read: sane values near the player => impact native
  // good; 0/0/0 or absurd values => impact native (0x199ded0) wrong. The bisection key.
  snprintf(line, sizeof(line), "imp %.1f %.1f %.1f", st.wfx_last_x, st.wfx_last_y, st.wfx_last_z);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  // Gun toolkit bisection: t=tick alive, fire=R2 seen, ray=aim hit, fx=effect native fired.
  // The first counter that stops advancing pinpoints the broken stage; ghit shows the hit point.
  snprintf(line, sizeof(line), "gun t %u fire %u ray %u fx %u", st.gun_tick_fired,
           st.gun_fire_detected, st.gun_raycast_hit, st.gun_effect_applied);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "ghit %.1f %.1f %.1f", st.gun_last_x, st.gun_last_y, st.gun_last_z);
  draw_native_text_line(line, x, y, style);
  y += line_h;
  snprintf(line, sizeof(line), "act %u res %u", st.last_action, st.last_result);
  draw_native_text_line(line, x, y, style);
  // Script-global watch: one line per baked global, showing its live value (n/a until the
  // block pool is up). Empty unless this is a GTAV_MENU_ENABLE_SCRIPT_GLOBALS build with the
  // base anchor set, so it costs a render line only where it is meaningful.
  for (uint32_t i = 0; i < st.sg_watch_count && i < GTAV_DEBUG_GLOBAL_WATCH_MAX; ++i) {
    y += line_h;
    if (st.sg_watch_valid[i]) {
      snprintf(line, sizeof(line), "g[%u] = %d", st.sg_watch_index[i], st.sg_watch_value[i]);
    } else {
      snprintf(line, sizeof(line), "g[%u] = n/a", st.sg_watch_index[i]);
    }
    draw_native_text_line(line, x, y, style);
  }
#else
  (void)worker_tick;
#endif
}

// Draw the interactive object-placement HUD: a compact centred stack of control hints + the
// live object pose, shown only while move mode is engaged. Same proven text lane as the other
// overlays; runs every render tick regardless of menu visibility (the menu is hidden during
// move mode), self-guards on the text natives, and is a no-op unless move mode is active.
// Worker-safe: reads the move readout snapshot, never a getter native.
[[maybe_unused]] static void render_object_move_overlay(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  GtavObjectMoveReadout mv;
  if (!gtav_features_object_move_readout(&mv) || !mv.active) return;

  const float x = 0.5f;  // centred at the top, clear of the top-left HUD and top-right zombie HUD
  float y = 0.040f;
  const float line_h = 0.030f;
  char line[80];
  // Amber title, off-white hint/pose lines -- centred and drop-shadowed for legibility.
  const NativeTextStyle title = {0, 0.42f, 0.42f, 255, 190, 60, 245, 1, 0.0f, 1.0f, 0, 1};
  const NativeTextStyle hint = {0, 0.30f, 0.30f, 235, 242, 255, 235, 1, 0.0f, 1.0f, 0, 1};

  draw_native_text_line("PLACE OBJECT", x, y, title);
  y += line_h + 0.004f;
  // Status: sub-mode (+ aim distance / snap) and the edit-target position in the live roster.
  if (mv.aim_mode) {
    snprintf(line, sizeof(line), "AIM  %.1fm%s    Object %u/%u", (double)mv.aim_dist,
             mv.snap_mode ? "  SNAP" : "", mv.sel_index, mv.sel_count);
  } else {
    snprintf(line, sizeof(line), "FREE MOVE%s    Object %u/%u",
             mv.ground_lock ? "  GROUND-LOCK" : "", mv.sel_index, mv.sel_count);
  }
  draw_native_text_line(line, x, y, hint);
  y += line_h;
  // Movement hints depend on the sub-mode.
  if (mv.aim_mode) {
    draw_native_text_line("Look to aim    L1/R1 Distance", x, y, hint);
    y += line_h;
  } else {
    draw_native_text_line("LStick Move    RStick Camera", x, y, hint);
    y += line_h;
    draw_native_text_line("L1/R1 Spin    L2/R2 Height", x, y, hint);
    y += line_h;
    if (mv.can_rotate3) {
      draw_native_text_line("Hold Duck: L1/R1 Roll  L2/R2 Pitch", x, y, hint);
      y += line_h;
    }
  }
  // Selection + the aim sub-mode + ground-lock toggles (only hint the ones whose natives resolved).
  if (mv.aim_avail) {
    draw_native_text_line("DPad L/R Select    Up Aim    Dn Ground-Lock", x, y, hint);
  } else {
    draw_native_text_line("DPad L/R Select    Dn Ground-Lock", x, y, hint);
  }
  y += line_h;
  draw_native_text_line("Square Ground    Triangle Dup    L3 Delete", x, y, hint);
  y += line_h;
  draw_native_text_line("Sprint Coarse    Cross Place    Circle Cancel", x, y, hint);
  y += line_h;
  snprintf(line, sizeof(line), "X %.2f  Y %.2f  Z %.2f", (double)mv.x, (double)mv.y, (double)mv.z);
  draw_native_text_line(line, x, y, hint);
  y += line_h;
  if (mv.can_rotate3) {
    snprintf(line, sizeof(line), "HDG %d  PITCH %d  ROLL %d%s", (int)(mv.heading + 0.5f),
             (int)(mv.pitch + 0.5f), (int)(mv.roll + 0.5f), mv.coarse ? "   COARSE" : "");
  } else {
    snprintf(line, sizeof(line), "HDG %d%s", (int)(mv.heading + 0.5f),
             mv.coarse ? "   COARSE" : "");
  }
  draw_native_text_line(line, x, y, hint);
#else
  (void)worker_tick;
#endif
}

// Free Camera HUD: visibility-independent (the menu is hidden while flying). Free cam has no legend
// of its own, so -- per the "augment" model -- this carries the stick/up-down/exit text while the
// native glyph bar (built in build_free_cam_instr_buttons) shows the same verbs as real glyphs at
// the bottom. Self-guards on the active flag + text natives, so it is a no-op otherwise.
[[maybe_unused]] static void render_free_cam_overlay(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  if (!gtav_features_free_cam_active()) return;
  const float x = 0.5f;  // centred at the top, matching the object-move legend
  float y = 0.040f;
  const float line_h = 0.030f;
  const NativeTextStyle title = {0, 0.42f, 0.42f, 255, 190, 60, 245, 1, 0.0f, 1.0f, 0, 1};
  const NativeTextStyle hint = {0, 0.30f, 0.30f, 235, 242, 255, 235, 1, 0.0f, 1.0f, 0, 1};
  draw_native_text_line("FREE CAMERA", x, y, title);
  y += line_h + 0.004f;
  draw_native_text_line("LStick Move    RStick Look", x, y, hint);
  y += line_h;
  draw_native_text_line("Cross Up    Duck Down    Sprint Boost", x, y, hint);
  y += line_h;
  draw_native_text_line("Circle Exit", x, y, hint);
#else
  (void)worker_tick;
#endif
}

// Draw the transient toast/confirmation stack bottom-centre, using the same proven
// text lane as the menu/HUD. Runs every render tick REGARDLESS of menu visibility so
// confirmations show with the panel closed. Bottom-centre clears the menu panel (right
// column x~0.736-0.929) and the HUD overlay (top-left). Self-guards: no text natives ->
// no-op; expiry is checked against the live worker_tick so toasts never freeze.
//
// Each pill is sized to hug its message: there is no text-width native available, so the
// width is summed from the per-glyph kGlyphAdvance table (1/100 em) * scale * kEmWidth.
// This tracks the proportional font far better than a flat per-char estimate, so narrow
// ("Saved") and wide ("Weapons unlocked") messages both fit snugly. The dark translucent
// panel uses the menu's layered-border look (a larger dark rect behind the fill) with a
// status-coloured accent bar down the left edge and near-white, left-justified text. Long
// messages auto-shrink their scale to stay one line; every toast fades in/out (and slides
// up slightly on entry) by ramping alpha over its lifetime -- the only continuously
// variable channel DRAW_RECT/text expose.
[[maybe_unused]] static void render_toasts(uint64_t worker_tick) {
#if GTAV_MENU_ENABLE_TOASTS && GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (!text_native_ready()) return;
  if (g_render_interval > 1u && (worker_tick % g_render_interval) != 0) return;
  const uint64_t draw_rect =
      g_draw_rect_call_override ? g_draw_rect_call_override : g_table.draw_rect;

  const float cx = 0.5f;           // bottom-centre column (pills stack as a centred column)
  const float base_scale = 0.30f;  // matches the menu label scale
  const float kEmWidth = 0.042f;   // em width: glyph advance (1/100 em) * scale * kEmWidth
  const float kMaxTextW = 0.34f;   // cap the text area; longer text shrinks to fit
  const float kMinScale = 0.20f;   // readability floor for very long messages
  const float kMinPanelW = 0.14f;  // floor so short toasts ("Saved") aren't tiny stubs
  const float left_margin = 0.006f;
  const float accent_w = 0.006f;
  const float gap = 0.008f;  // accent bar -> text gap
  const float right_pad = 0.012f;
  const float pad_total = left_margin + accent_w + gap + right_pad;
  const float panel_h = 0.038f;
  const float glyph_mid = 0.012f;  // half glyph height -> vertical text centring
  const float step = 0.046f;       // vertical spacing between stacked pills
  // Fade ramps (worker ticks). 12 + 18 == 30 == the minimum toast lifetime, so even the
  // shortest toast reaches full opacity. With g_render_interval > 1 the renderer samples
  // only every Nth tick, so the ramp looks steppier but stays correct.
  const uint64_t kFadeIn = 12u;
  const uint64_t kFadeOut = 18u;
  float y = 0.85f;  // newest toast sits lowest; older stack upward

  for (uint32_t i = 0; i < GTAV_TOAST_SLOTS; ++i) {
    // Iterate newest -> oldest for a stable bottom-up stack.
    const uint32_t idx = (g_toast_head + GTAV_TOAST_SLOTS - 1u - i) % GTAV_TOAST_SLOTS;
    Toast* t = &g_toasts[idx];
    if (!t->active) continue;
    if (worker_tick >= t->expire_tick) {
      t->active = 0;
      continue;
    }

    // Coalesced repeats render with a "(xN)" suffix so a mashed action reads as one event.
    char disp[GTAV_TOAST_TEXT_LEN + 12];
    if (t->repeat > 1u) {
      snprintf(disp, sizeof(disp), "%s (x%u)", t->text, (unsigned)t->repeat);
    } else {
      snprintf(disp, sizeof(disp), "%s", t->text);
    }

    // Size the pill to the message: sum the proportional per-glyph advances (1/100 em)
    // so the panel hugs the real text, then auto-shrink the scale so long text stays one
    // line. Unmapped / non-ASCII bytes use an average advance (the right-edge wrap clamps
    // any residual overflow anyway).
    uint32_t adv_sum = 0u;  // accumulates in 1/100-em units
    for (const char* p = disp; *p; ++p) {
      const uint8_t c = (uint8_t)*p;
      adv_sum += (c < 128u) ? kGlyphAdvance[c] : 90u;
    }
    float scale = base_scale;
    float text_w = (float)adv_sum * 0.01f * scale * kEmWidth;
    if (text_w > kMaxTextW) {
      scale = base_scale * (kMaxTextW / text_w);
      if (scale < kMinScale) scale = kMinScale;
      text_w = kMaxTextW;
    }
    float panel_w = text_w + pad_total;
    if (panel_w < kMinPanelW) panel_w = kMinPanelW;
    if (panel_w > kMaxTextW + pad_total) panel_w = kMaxTextW + pad_total;
    const float panel_left = cx - panel_w * 0.5f;

    // Fade in over the first kFadeIn ticks, out over the last kFadeOut, full opacity
    // between. The guards keep elapsed/remaining from underflowing the unsigned counter.
    const uint64_t elapsed = (worker_tick > t->birth_tick) ? (worker_tick - t->birth_tick) : 0u;
    const uint64_t remaining = (t->expire_tick > worker_tick) ? (t->expire_tick - worker_tick) : 0u;
    float fade = 1.0f;
    if (elapsed < kFadeIn) fade = (float)elapsed / (float)kFadeIn;
    if (remaining < kFadeOut) {
      const float out = (float)remaining / (float)kFadeOut;
      if (out < fade) fade = out;  // min() so short-lived toasts still fade cleanly
    }
    if (fade < 0.0f) fade = 0.0f;
    if (fade > 1.0f) fade = 1.0f;
    // Subtle slide-up on entry only (exit slide fights the stack offset).
    const float y_off = (elapsed < kFadeIn) ? (1.0f - fade) * 0.012f : 0.0f;
    const float yt = y + y_off;

    if (draw_rect) {
      // Layered border (a larger dark rect behind the fill, matching the menu chrome),
      // the dark translucent panel, then the status-coloured left accent bar. Every alpha
      // is scaled by fade so the toast animates in and out.
      draw_shell_rect(draw_rect, cx, yt, panel_w + 0.005f, panel_h + 0.006f, 0, 0, 0,
                      (int)(150.0f * fade));
      draw_shell_rect(draw_rect, cx, yt, panel_w, panel_h, 12, 14, 20, (int)(220.0f * fade));
      const float accent_x = panel_left + left_margin + accent_w * 0.5f;
      draw_shell_rect(draw_rect, accent_x, yt, accent_w, panel_h - 0.010f, t->r, t->g, t->b,
                      (int)(255.0f * fade));
    }

    // Near-white, left-justified text; wrap bounds clamp the right edge.
    const float text_x = panel_left + left_margin + accent_w + gap;
    NativeTextStyle style = {0,
                             scale,
                             scale,
                             235,
                             242,
                             255,
                             (int)(255.0f * fade),
                             0,
                             text_x,
                             panel_left + panel_w - right_pad,
                             0,
                             1};
    draw_native_text_line(disp, text_x, yt - glyph_mid, style);
    y -= step;
  }
#else
  (void)worker_tick;
#endif
}

static void emit_shell_event(const char* message) {
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
}

static void emit_unavailable_item(const ShellItem* item) {
  const char* reason = item && item->unavailable_reason ? item->unavailable_reason : "unavailable";

  // Surface the reason on-screen so pressing a locked (game-thread-gated) row is not a dead
  // button -- it previously only wrote a telemetry line the player never sees. Debounce
  // identical consecutive presses (same action within a short window) so mashing Cross on a
  // locked row does not spam the toast stack.
  {
    static uint32_t s_last_toast_action = 0xffffffffu;
    static uint64_t s_last_toast_tick = 0u;
    const uint32_t action = item ? item->action : 0u;
    const uint64_t now = g_snapshot.worker_tick;
    if (action != s_last_toast_action || (now - s_last_toast_tick) >= 30u) {
      gtav_native_bridge_push_toast(reason, GTAV_FEATURE_RESULT_UNAVAILABLE);
      s_last_toast_action = action;
      s_last_toast_tick = now;
    }
  }

  if (item && item->action != GTAV_NATIVE_SHELL_ACTION_NONE) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                       "feature unavailable item=%s action=%s reason=%s", item->label,
                       gtav_native_bridge_action_name(item->action), reason);
    return;
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell unavailable item=%s type=%s reason=%s",
                     item ? item->label : "", item ? shell_row_type_name(item->type) : "unknown",
                     reason);
}

}  // namespace

// Menu customisation: theme + draw-region selectors. State lives in the anon namespace
// above (read by the render path each frame); these accessors clamp into range so a value
// restored from /data can never index out of bounds. Pure state writes, thread-agnostic.
extern "C" void gtav_native_bridge_set_theme(uint32_t index) {
  g_theme_index = index % kThemeSelectableCount;  // includes the synthetic Custom slot
}
extern "C" uint32_t gtav_native_bridge_theme(void) {
  return g_theme_index;
}
extern "C" uint32_t gtav_native_bridge_theme_count(void) {
  return kThemeSelectableCount;
}
extern "C" const char* gtav_native_bridge_theme_label(uint32_t index) {
  if (index >= kThemeCount) return "Custom";
  return kThemes[index].name;
}

// Theme Editor accessors. Stepping a channel auto-selects the Custom slot so the recolour previews
// live in the running menu; values clamp 0-255 (step 5). Reset re-seeds the default palette.
extern "C" void gtav_native_bridge_adjust_custom_color(uint32_t channel, int dir) {
  uint8_t* c = custom_color_channel(channel);
  if (!c) return;
  const int step = 5;
  int v = (int)*c + (dir < 0 ? -step : step);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  *c = (uint8_t)v;
  g_theme_index = kThemeCount;  // editing selects Custom so the change is visible immediately
}
extern "C" uint32_t gtav_native_bridge_custom_color(uint32_t channel) {
  const uint8_t* c = custom_color_channel(channel);
  return c ? (uint32_t)*c : 0u;
}
extern "C" void gtav_native_bridge_reset_custom_theme(void) {
  g_custom_theme = kThemes[0];
  g_custom_theme_seeded = 1;
}
// Profile bridge: the 9 editable channels (accent/selection/panel RGB) as plain 0-255 values.
extern "C" void gtav_native_bridge_get_custom_theme(uint32_t* channels) {
  if (!channels) return;
  for (uint32_t i = 0; i < GTAV_CUSTOM_THEME_CHANNELS; ++i) {
    const uint8_t* c = custom_color_channel(i);
    channels[i] = c ? (uint32_t)*c : 0u;
  }
}
// Apply the first `count` editable channels, leaving the rest at the current (seeded) value. Used
// by the profile import so an older file that only stored the 9 chrome channels keeps the default-
// palette seed for the colour groups it never had (rather than loading them as black).
extern "C" void gtav_native_bridge_set_custom_theme_count(const uint32_t* channels,
                                                          uint32_t count) {
  if (!channels) return;
  ensure_custom_theme_seeded();
  if (count > GTAV_CUSTOM_THEME_CHANNELS) count = GTAV_CUSTOM_THEME_CHANNELS;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* c = custom_color_channel(i);
    if (c) *c = (uint8_t)(channels[i] & 0xFFu);
  }
}
extern "C" void gtav_native_bridge_set_custom_theme(const uint32_t* channels) {
  gtav_native_bridge_set_custom_theme_count(channels, GTAV_CUSTOM_THEME_CHANNELS);
}
extern "C" void gtav_native_bridge_set_region(uint32_t index) {
  g_region_index = index % kRegionCount;
}
extern "C" uint32_t gtav_native_bridge_region(void) {
  return g_region_index;
}
extern "C" uint32_t gtav_native_bridge_region_count(void) {
  return kRegionCount;
}
extern "C" const char* gtav_native_bridge_region_label(uint32_t index) {
  return kRegionNames[index % kRegionCount];
}
extern "C" void gtav_native_bridge_set_panel_width_index(uint32_t index) {
  g_panel_width_index = index % kPanelWidthCount;
}
extern "C" uint32_t gtav_native_bridge_panel_width_index(void) {
  return g_panel_width_index;
}
extern "C" uint32_t gtav_native_bridge_panel_width_count(void) {
  return kPanelWidthCount;
}
extern "C" const char* gtav_native_bridge_panel_width_label(uint32_t index) {
  return kPanelWidthNames[index % kPanelWidthCount];
}

extern "C" void gtav_native_bridge_init(const GtavNativeAddressTable* table) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
  if (table && table->abi_version == GTAV_NATIVE_BRIDGE_ABI_VERSION) {
    gtav_render_diag_bind_counter(table->get_frame_count);
    gtav_render_diag_bind(GTAV_RD_RECT, table->draw_rect);
    gtav_render_diag_bind(GTAV_RD_SPRITE, table->draw_sprite);
    gtav_render_diag_bind(GTAV_RD_TEXT_BEGIN, table->begin_text_command_display_text);
    gtav_render_diag_bind(GTAV_RD_TEXT_ADD, table->add_text_component_substring_player_name);
    gtav_render_diag_bind(GTAV_RD_TEXT_END, table->end_text_command_display_text);
  }
#if GTAV_RENDER_DIAG_ISOLATED
  memset(&g_table, 0, sizeof(g_table));
  g_has_table = 0;
  if (table && table->abi_version == GTAV_NATIVE_BRIDGE_ABI_VERSION) {
    g_table = *table;
    g_has_table = 1;
  }
#if GTAV_MENU_PHASE_DRAW_LIST
  g_static_menu_config = 0;
#endif
  return;  // No menus, profile, snapshots, resources or natives in primitive initialization.
#endif
#endif
  NB_TRACE("nb: enter; memsets...");
  memset(&g_table, 0, sizeof(g_table));
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  memset(g_selected_by_menu, 0, sizeof(g_selected_by_menu));
  memset(g_scroll_by_menu, 0, sizeof(g_scroll_by_menu));
  memset(g_menu_stack, 0, sizeof(g_menu_stack));
  g_last_frame_tick = 0;
  g_current_menu = SHELL_MENU_MAIN;
  g_menu_depth = 0;
  g_has_table = 0;
  g_reported_mode = 0;
  g_seen_frame_tick = 0;
  g_canary_flags = 0;
  g_canary_interval = 0;
  g_canary_max_frames = 0;
  g_canary_drawn_frames = 0;
  g_worker_text_overlay_draws = 0;
  g_canary_timer_reads = 0;
  g_last_game_timer = 0;
  g_draw_rect_call_override = 0;
  g_reported_canary = 0;
  g_reported_worker_text_overlay = 0;
  g_reported_timer_canary = 0;
#if GTAV_MENU_PHASE_DRAW_LIST
  g_phase_draw_builder = nullptr;
  g_phase_draw_build_failed = 0;
#endif
  g_feature_toggle_mask = 0;
  g_last_action_param = 0;
  memset(g_list_values, 0, sizeof(g_list_values));
  NB_TRACE("nb: memsets done; build_menus...");
  build_menus();
  NB_TRACE("nb: build_menus done (spawner_count=%u)", g_spawner_item_count);

  if (table && table->abi_version == GTAV_NATIVE_BRIDGE_ABI_VERSION) {
    memcpy(&g_table, table, sizeof(g_table));
    g_has_table = 1;
  }

  NB_TRACE("nb: pre refresh_snapshot has_table=%d", g_has_table);
  refresh_snapshot(0, 0, 0);
  NB_TRACE("nb: refresh_snapshot done");
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                     "native bridge initialized mode=%s flags=0x%08x", g_snapshot.mode,
                     g_snapshot.flags);
}

extern "C" void gtav_native_bridge_set_canary(uint32_t flags, uint32_t interval,
                                              uint32_t max_frames) {
  g_canary_flags = flags;
  g_canary_interval = interval ? interval : 1u;
  g_canary_max_frames = max_frames;
  g_canary_drawn_frames = 0;
  g_worker_text_overlay_draws = 0;
  g_canary_timer_reads = 0;
  g_last_game_timer = 0;
  g_reported_canary = 0;
  g_reported_worker_text_overlay = 0;
  g_reported_timer_canary = 0;
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, (int)g_snapshot.visible);
  gtav_status_eventf(
      GTAV_MENU_EVENT_NATIVE_BRIDGE,
      "native canary flags=0x%08x interval=%u max_frames=%u draw_rect=0x%llx timer=0x%llx",
      g_canary_flags, g_canary_interval, g_canary_max_frames, (unsigned long long)g_table.draw_rect,
      (unsigned long long)g_table.get_game_timer);
}

extern "C" void gtav_native_bridge_set_draw_rect_call_override(uint64_t address) {
  g_draw_rect_call_override = address;
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, (int)g_snapshot.visible);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native draw rect call override=0x%llx",
                     (unsigned long long)g_draw_rect_call_override);
}

extern "C" void gtav_native_bridge_set_visible(int visible) {
  // Stamp the open tick on the 0->1 transition so the panel fades/slides in (g_snapshot.visible
  // still holds the previous state here, before refresh_snapshot below updates it).
  if (visible && !g_snapshot.visible) {
    g_menu_visible_since = g_snapshot.worker_tick;
  }
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, visible);
}

extern "C" uint32_t gtav_native_bridge_poll_input_command(uint64_t worker_tick, int visible) {
  (void)worker_tick;
  (void)visible;
  return GTAV_MENU_COMMAND_NONE;
}

// Menu-navigation input suppression (block GTA from reacting to D-pad/Cross/Circle
// while the panel is open) is done on the game thread in real script context -- see
// suppress_menu_input_controls() in src/module/features/weapon_fx.inc. The old
// worker-thread DISABLE_CONTROL_ACTION lane raced the game's per-frame control read
// and missed the radio/context controls, so it was removed from here.

#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
// On hover-change, resolve the hovered vehicle's model name (for the breadcrumb + placeholder
// caption) from the spawn catalog and its preview-registry entry (dict/texture/caption), if any.
// Then hand the target to the feature layer every tick so the game-thread streamer keeps the
// mapped dict resident (or releases it when there is no mapping / no hover). The O(n) catalog
// scans run only when the hovered hash changes; the per-tick setter call is cheap and drives the
// streamer's keep-alive cadence (the feature layer throttles the actual request).
static void preview_set_hovered_model(uint32_t model_hash) {
  static uint32_t s_last_hash = 0xffffffffu;
  if (model_hash != s_last_hash) {
    s_last_hash = model_hash;
    g_preview_model[0] = '\0';
    g_preview_entry = nullptr;
    if (model_hash) {
      for (uint32_t i = 0; i < gtav_vehicle_catalog_count; ++i) {
        if (gtav_vehicle_catalog[i].model_hash == model_hash) {
          snprintf(g_preview_model, sizeof(g_preview_model), "%s", gtav_vehicle_catalog[i].model);
          break;
        }
      }
      for (uint32_t i = 0; i < gtav_vehicle_preview_catalog_count; ++i) {
        if (gtav_vehicle_preview_catalog[i].model_hash == model_hash) {
          g_preview_entry = &gtav_vehicle_preview_catalog[i];
          break;
        }
      }
    }
  }
  // Mapped vehicle: stream its dict + draw its texture. Unmapped (or no hover): empty dict/texture
  // (no sprite -> placeholder) but keep the model label as the placeholder caption.
  if (g_preview_entry)
    gtav_features_preview_set_target(g_preview_entry->dict, g_preview_entry->texture,
                                     g_preview_entry->caption);
  else
    gtav_features_preview_set_target("", "", g_preview_model);
}
#endif

#if GTAV_MENU_PHASE_DRAW_LIST
// Phase-side consumer shared by the static certification gate and the real interactive renderer.
// It reads only one claimed immutable generation and performs no allocation, blocking, formatting,
// logging, file I/O, gameplay native, or mutable menu-state access.
static int consume_phase_draw_list(void) {
  if (render_is_parked() || !g_has_table || !g_table.draw_rect || !text_native_ready()) return 0;
  const GtavMenuDrawList* list = gtav_menu_draw_list_acquire();
  if (!list) return 0;
  // Refuse the whole generation before issuing its first native if a command has no available
  // submission primitive. Partial menu frames are worse than reusing the last valid frame.
  for (uint32_t i = 0; i < list->count; ++i) {
    const uint32_t kind = list->commands[i].kind;
    if (kind != GTAV_MENU_DRAW_RECT && kind != GTAV_MENU_DRAW_TEXT &&
        kind != GTAV_MENU_DRAW_SPRITE) {
      gtav_menu_draw_list_release(list);
      return 0;
    }
    if (kind == GTAV_MENU_DRAW_SPRITE && !g_table.draw_sprite) {
      gtav_menu_draw_list_release(list);
      return 0;
    }
  }
  for (uint32_t i = 0; i < list->count; ++i) {
    const GtavMenuDrawCommand* command = &list->commands[i];
    if (command->kind == GTAV_MENU_DRAW_RECT) {
      const GtavMenuDrawRect* r = &command->payload.rect;
      invoke_void(g_table.draw_rect, r->x, r->y, r->width, r->height, r->r, r->g, r->b, r->a);
    } else if (command->kind == GTAV_MENU_DRAW_TEXT) {
      const GtavMenuDrawText* t = &command->payload.text;
      invoke_void(g_table.set_text_font, t->font);
      invoke_void(g_table.set_text_scale, t->scale_x, t->scale_y);
      invoke_void(g_table.set_text_colour, t->r, t->g, t->b, t->a);
      if (t->drop_shadow && g_table.set_text_drop_shadow) invoke_void(g_table.set_text_drop_shadow);
      if (t->outline && g_table.set_text_outline) invoke_void(g_table.set_text_outline);
      invoke_void(g_table.set_text_centre, t->centre);
      invoke_void(g_table.set_text_justification, t->justification == 2 ? 2 : 1);
      invoke_void(g_table.set_text_wrap, t->wrap_start, t->wrap_end);
      invoke_void(g_table.begin_text_command_display_text, "STRING");
      invoke_void(g_table.add_text_component_substring_player_name, t->text);
      invoke_void(g_table.end_text_command_display_text, t->x, t->y);
    } else if (command->kind == GTAV_MENU_DRAW_SPRITE) {
      const GtavMenuDrawSprite* s = &command->payload.sprite;
      invoke_void(g_table.draw_sprite, s->dict, s->texture, s->x, s->y, s->width, s->height,
                  s->heading, s->r, s->g, s->b, s->a, s->p11, s->p12);
    }
  }
  gtav_menu_draw_list_release(list);
#if GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && GTAV_MENU_ENABLE_NATIVE_FEATURES
  (void)gtav_features_instructional_phase_draw();
#endif
  return 1;
}
#endif

#if GTAV_RENDER_DIAG_ISOLATED
static void diagnostic_primitive(uint64_t tick, int visible) {
  const uint64_t config = gtav_render_diag_config();
  const unsigned mode = config & 255u;
  uint32_t skip = 0;
  if (mode == GTAV_RD_DISABLED)
    skip = GTAV_RD_SKIP_DISABLED;
  else if (mode >= GTAV_RD_GAME_RECT && mode <= GTAV_RD_GAME_STATIC_MENU)
    skip = GTAV_RD_SKIP_LANE;
  else if (render_is_parked())
    skip = GTAV_RD_SKIP_PARKED;
  else if (!visible)
    skip = GTAV_RD_SKIP_HIDDEN;
  else if (!worker_natives_ready())
    skip = GTAV_RD_SKIP_CONTEXT;
  else if (g_render_interval > 1 && tick % g_render_interval)
    skip = GTAV_RD_SKIP_INTERVAL;
  else if (gtav_render_diag_warming())
    skip = GTAV_RD_SKIP_WARMUP;
  else if (!g_table.draw_rect)
    skip = GTAV_RD_SKIP_ADDRESS;
  if (!skip && mode == GTAV_RD_WORKER_TEXT &&
      (!g_table.set_text_font || !g_table.set_text_scale || !g_table.set_text_colour ||
       !g_table.set_text_centre || !g_table.set_text_justification || !g_table.set_text_wrap ||
       !g_table.begin_text_command_display_text ||
       !g_table.add_text_component_substring_player_name || !g_table.end_text_command_display_text))
    skip = GTAV_RD_SKIP_ADDRESS;
  gtav_render_diag_skip(skip);
  if (skip) return;
  const int alpha = (config >> 8) & 255u;
  // One fixed rectangle. The invocation helper zeroes the unused trailing slots, as in baseline.
  invoke_void(g_table.draw_rect, 0.50f, 0.50f, 0.32f, 0.12f, 30, 144, 255, alpha);
  if (mode != GTAV_RD_WORKER_TEXT) return;
  static const char text[] = "GTAV DRAW DIAGNOSTIC";
  invoke_void(g_table.set_text_font, 0);
  invoke_void(g_table.set_text_scale, 0.35f, 0.35f);
  invoke_void(g_table.set_text_colour, 255, 255, 255, 255);
  invoke_void(g_table.set_text_centre, 0);
  invoke_void(g_table.set_text_justification, 1);
  invoke_void(g_table.set_text_wrap, 0.0f, 1.0f);
  invoke_void(g_table.begin_text_command_display_text, "STRING");
  invoke_void(g_table.add_text_component_substring_player_name, text);
  invoke_void(g_table.end_text_command_display_text, 0.40f, 0.48f);
}

#if GTAV_RENDER_PHASE_INTERCEPT
static int diagnostic_phase_primitive(uint64_t epoch, int visible) {
  const uint64_t config = gtav_render_diag_config();
  const unsigned mode = config & 255u;
  const int wants_list = GTAV_MENU_PHASE_DRAW_LIST && mode == GTAV_RD_GAME_STATIC_MENU;
  const int wants_rect = mode == GTAV_RD_GAME_RECT || mode == GTAV_RD_GAME_COMBINED || wants_list;
  const int wants_text = mode == GTAV_RD_GAME_TEXT || mode == GTAV_RD_GAME_COMBINED || wants_list;
  uint32_t skip = 0;
  if (!wants_rect && !wants_text)
    skip = GTAV_RD_SKIP_LANE;
  else if (render_is_parked())
    skip = GTAV_RD_SKIP_PARKED;
  else if (!visible)
    skip = GTAV_RD_SKIP_HIDDEN;
  else if (gtav_render_diag_warming())
    skip = GTAV_RD_SKIP_WARMUP;
  else if (wants_rect && !g_table.draw_rect)
    skip = GTAV_RD_SKIP_ADDRESS;
  if (!skip && wants_text &&
      (!g_table.set_text_font || !g_table.set_text_scale || !g_table.set_text_colour ||
       !g_table.set_text_centre || !g_table.set_text_justification || !g_table.set_text_wrap ||
       !g_table.begin_text_command_display_text ||
       !g_table.add_text_component_substring_player_name || !g_table.end_text_command_display_text))
    skip = GTAV_RD_SKIP_ADDRESS;
  if (skip) return 0;
#if GTAV_MENU_PHASE_DRAW_LIST
  if (wants_list) {
    (void)epoch;
    return consume_phase_draw_list();
  }
#endif
  const int alpha = (config >> 8) & 255u;
  if (wants_rect) invoke_void(g_table.draw_rect, 0.50f, 0.50f, 0.32f, 0.12f, 30, 144, 255, alpha);
  if (wants_text) {
    static const char text[] = "GTAV PHASE DIAGNOSTIC";
    invoke_void(g_table.set_text_font, 0);
    invoke_void(g_table.set_text_scale, 0.35f, 0.35f);
    invoke_void(g_table.set_text_colour, 255, 255, 255, 255);
    invoke_void(g_table.set_text_centre, 0);
    invoke_void(g_table.set_text_justification, 1);
    invoke_void(g_table.set_text_wrap, 0.0f, 1.0f);
    invoke_void(g_table.begin_text_command_display_text, "STRING");
    invoke_void(g_table.add_text_component_substring_player_name, text);
    invoke_void(g_table.end_text_command_display_text, 0.40f, 0.48f);
  }
  (void)epoch;
  return 1;
}
#endif
#endif

#if GTAV_RENDER_DIAG_ISOLATED && GTAV_MENU_PHASE_DRAW_LIST
static void publish_static_phase_menu(void) {
  const uint64_t config = gtav_render_diag_config();
  if ((config & 255u) != GTAV_RD_GAME_STATIC_MENU || config == g_static_menu_config) return;
  GtavMenuDrawList* list = gtav_menu_draw_list_begin(config >> 16u);
  if (!list) return;
  int rc = 0;
#define ADD_RECT(x, y, w, h, r, g, b, a) \
  rc |= gtav_menu_draw_list_add_rect(list, x, y, w, h, r, g, b, a)
#define ADD_TEXT(text, x, y, scale, r, g, b, a, justify)                                           \
  rc |= gtav_menu_draw_list_add_text(list, text, x, y, scale, scale, 0, r, g, b, a, 0, 0.0f, 1.0f, \
                                     justify, 1, 0)
  ADD_RECT(0.815f, 0.365f, 0.285f, 0.480f, 0, 0, 0, 165);
  ADD_RECT(0.815f, 0.365f, 0.280f, 0.474f, 12, 18, 28, 225);
  ADD_RECT(0.815f, 0.145f, 0.280f, 0.054f, 30, 144, 255, 235);
  for (uint32_t row = 0; row < 8u; ++row) {
    const float y = 0.205f + 0.038f * (float)row;
    if (row & 1u) ADD_RECT(0.815f, y, 0.280f, 0.038f, 255, 255, 255, 8);
  }
  ADD_RECT(0.815f, 0.281f, 0.280f, 0.038f, 30, 144, 255, 165);
  ADD_RECT(0.677f, 0.281f, 0.004f, 0.038f, 120, 210, 255, 255);
  ADD_RECT(0.815f, 0.535f, 0.280f, 0.058f, 8, 12, 20, 235);
  ADD_TEXT("GTAV PHASE MENU", 0.684f, 0.132f, 0.34f, 235, 242, 255, 255, 0);
  ADD_TEXT("Player", 0.684f, 0.193f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("Vehicle", 0.684f, 0.231f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("Weapons", 0.684f, 0.269f, 0.30f, 255, 255, 255, 255, 0);
  ADD_TEXT("Teleport", 0.684f, 0.307f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("World", 0.684f, 0.345f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("Recovery", 0.684f, 0.383f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("Settings", 0.684f, 0.421f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("About", 0.684f, 0.459f, 0.30f, 225, 232, 242, 255, 0);
  ADD_TEXT("Static immutable draw list", 0.684f, 0.515f, 0.235f, 185, 205, 235, 235, 0);
  ADD_TEXT("3 / 8", 0.925f, 0.515f, 0.235f, 185, 205, 235, 235, 2);
#undef ADD_TEXT
#undef ADD_RECT
  const int publish_rc = gtav_menu_draw_list_publish(list);
  if (rc != 0 || publish_rc != 0) return;
  g_static_menu_config = config;
}
#endif

extern "C" void gtav_native_bridge_worker_tick(uint64_t worker_tick, int visible) {
#if GTAV_RENDER_DIAG_ISOLATED
#if GTAV_MENU_PHASE_DRAW_LIST
  publish_static_phase_menu();
#endif
  diagnostic_primitive(worker_tick, visible);
  return;
#endif
  g_last_worker_tick = worker_tick;  // base for toast expiry + motion-feedback flash decay
  refresh_worker_snapshot(worker_tick, visible);  // string/status only -- no natives
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Wardrobe full-player camera: engaged only while the Wardrobe submenu is open AND the menu is
  // actually rendering (not parked). Set BEFORE the park-return below so it clears to 0 when the
  // renderer parks for teardown/suspend, letting the game-thread driver tear the scripted cam down.
  gtav_features_set_wardrobe_cam_active(visible && !render_is_parked() &&
                                        g_current_menu == SHELL_MENU_WARDROBE);
#endif
  // Teardown/suspend park: stand down all draw/getter natives so the GPU graphics pipe can go
  // idle for the OS app-suspend handshake (a force-close that finds the worker still drawing trips
  // the SYSTEM_SUSPEND_BLOCK_TIMEOUT crash). The snapshot above already ran, so status stays live
  // and shows the parked state. Reversible: quit_guard unparks if the game thread comes back.
  if (render_is_parked()) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_skip(GTAV_RD_SKIP_PARKED);
#endif
    return;
  }
  // Until SP gameplay is live (the frame hook has fired in script context), worker-thread
  // natives crash the load. Skip ALL native work until then; the snapshot above keeps
  // status/commands responsive meanwhile. No-op (always ready) unless the gate is built in.
  if (!worker_natives_ready()) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_skip(GTAV_RD_SKIP_CONTEXT);
#endif
    return;
  }
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Warm the highlighted vehicle's model on the worker while the user browses, so the
  // streamer has it resident before they confirm the spawn. Pass 0 when the cursor is
  // not on a spawn row (the class cycler, another menu, or the panel hidden) so the
  // previously-warmed model is released and scrolling does not pin every model.
  {
    uint32_t hovered_vehicle_model = 0;
    if (visible) {
      const ShellItem* item = selected_item();
      if (item && item->action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE) {
        hovered_vehicle_model = item->param;
      }
    }
    gtav_features_preload_highlighted_vehicle(hovered_vehicle_model);
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
    // Same highlighted-vehicle signal drives the image preview: resolve its model name and keep
    // the website-thumbnail texture dict streamed (released when the cursor leaves spawn rows).
    preview_set_hovered_model(hovered_vehicle_model);
#endif
  }
#endif
  if (!g_reported_mode) {
    g_reported_mode = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native shell mode=%s item=%s",
                       g_snapshot.mode, g_snapshot.selected_label);
  }
#if GTAV_MENU_PHASE_DRAW_LIST
  // Render interval now controls immutable snapshot publication cadence. The phase callback keeps
  // resubmitting the last complete generation on intervening frames; publishing an empty list on a
  // skipped worker tick would reintroduce visible blank frames.
  if (g_render_interval <= 1u || (worker_tick % g_render_interval) == 0u) {
    g_phase_draw_builder = gtav_menu_draw_list_begin(worker_tick);
    g_phase_draw_build_failed = 0;
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
    g_phase_draw_preview_dict[0] = '\0';
#endif
  }
#endif
  get_game_timer_canary(worker_tick);
  worker_text_overlay(worker_tick, visible);
  // The HUD overlay is independent of the menu panel: it draws whether or not the
  // shell is visible (self-guards on the HUD master toggle + text natives).
  render_hud_overlay(worker_tick);
  // Zombie Outbreak survival HUD: visibility-independent like the info HUD; self-guards on the
  // active-minigame gate + text natives, so it is a no-op unless the survival mode is running.
  render_zombie_hud(worker_tick);
  // Toasts are likewise visibility-independent so a confirmation shows even after the
  // panel auto-hides; self-guards on the toast build gate + text natives.
  render_toasts(worker_tick);
  // Debug overlay: independent of the menu panel and its own toggle; lets the diagnostics
  // stay on screen with the menu closed (e.g. watching the weapon-fx counters while firing).
  render_debug_overlay(worker_tick);
  // Interactive object-placement HUD: visibility-independent (the menu is hidden during move
  // mode); self-guards on the move-mode gate + text natives, so it is a no-op otherwise.
  render_object_move_overlay(worker_tick);
  // Free Camera HUD: same visibility-independent pattern; no-op unless the free cam is flying.
  render_free_cam_overlay(worker_tick);
#if GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Feed GTA's native instructional-button Scaleform: an active menu-hiding mode (object move /
  // free cam) wins, else the focused menu row when the panel is open, else nothing. Drives the bar
  // even while the menu is hidden -- the inverse of the worker fallback bar, which only draws
  // menu-open.
  publish_active_instructional_bar(visible);
#endif
#if GTAV_MENU_PHASE_DRAW_LIST
  if (g_phase_draw_builder) {
    GtavMenuDrawList* completed = g_phase_draw_builder;
    g_phase_draw_builder = nullptr;
    if (g_phase_draw_build_failed) completed->flags |= GTAV_MENU_DRAW_LIST_OVERFLOW;
    const int publish_rc = gtav_menu_draw_list_publish(completed);
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_NATIVE_FEATURES
    gtav_features_preview_phase_published(completed->generation, publish_rc == 0 && completed->count
                                                                     ? g_phase_draw_preview_dict
                                                                     : "");
#else
    (void)publish_rc;
#endif
  }
#endif
}

extern "C" void gtav_native_bridge_frame_tick(uint64_t frame_tick, int visible) {
#if GTAV_RENDER_DIAG_ISOLATED
  return;  // Includes legacy callers: game-side diagnostic rendering is not validated.
#endif
  refresh_frame_snapshot(frame_tick, visible);
  // Parked for teardown/suspend: skip the game-thread draw canary too. (When parked the .text
  // frame hook is normally un-hooked already, so this is belt-and-suspenders against a late fire.)
  if (render_is_parked()) return;
  draw_rect_canary(frame_tick, visible);
}

extern "C" int gtav_native_bridge_phase_tick(uint64_t epoch, int visible) {
#if GTAV_RENDER_DIAG_ISOLATED && GTAV_RENDER_PHASE_INTERCEPT
  return diagnostic_phase_primitive(epoch, visible);
#elif GTAV_RENDER_PHASE_INTERCEPT && GTAV_MENU_PHASE_DRAW_LIST
  (void)epoch;
  (void)visible;  // Visibility and independent overlays are already encoded in the list.
  return consume_phase_draw_list();
#else
  (void)epoch;
  (void)visible;
  return 0;
#endif
}

extern "C" void gtav_native_bridge_tick(uint64_t worker_tick, int visible) {
  gtav_native_bridge_worker_tick(worker_tick, visible);
}

// Pure-info rows the d-pad cursor should skip over (the Controls reference page, numeric stubs, the
// empty-Quick hint). Deliberately NOT hook-gated LOCK rows -- the user should still be able to land
// on a locked action to read why it is unavailable, so only the genuinely non-interactive row types
// are skipped.
static int shell_row_is_info(const ShellItem* item) {
  return item && (item->type == SHELL_ROW_DISABLED || item->type == SHELL_ROW_NUMERIC_PLACEHOLDER);
}

// Count selectable (non-info) rows in the current menu.
static uint32_t shell_selectable_count() {
  const ShellMenu* menu = shell_menu(g_current_menu);
  if (!menu) return 0u;
  uint32_t n = 0u;
  for (uint32_t i = 0; i < menu->item_count; ++i) {
    if (!shell_row_is_info(&menu->items[i])) ++n;
  }
  return n;
}

// Step the cursor one row in dir (>=0 next, <0 prev) with wrap, skipping pure-info rows so the
// cursor never parks on a non-interactive line. If the page has NO selectable rows (e.g. the
// all-info Controls page or the empty-Quick hint), fall back to a plain single step so the page is
// still scrollable to read every row.
static uint32_t next_selectable_index(uint32_t from, int dir) {
  const ShellMenu* menu = shell_menu(g_current_menu);
  const uint32_t count = shell_item_count();
  if (!menu || count == 0u) return 0u;
  const uint32_t plain = (dir < 0) ? ((from + count - 1u) % count) : ((from + 1u) % count);
  if (shell_selectable_count() == 0u) return plain;  // all-info page: plain scroll
  uint32_t i = from;
  for (uint32_t steps = 0; steps < count; ++steps) {
    i = (dir < 0) ? ((i == 0u) ? count - 1u : i - 1u) : ((i + 1u) % count);
    if (!shell_row_is_info(&menu->items[i])) return i;
  }
  return plain;  // unreachable when selectable_count > 0, but safe
}

extern "C" void gtav_native_bridge_next(void) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  clamp_current_selection();
  g_selected_by_menu[g_current_menu] = next_selectable_index(g_snapshot.selected_index, 1);
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell next item=%s", g_snapshot.selected_label);
}

extern "C" void gtav_native_bridge_prev(void) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  clamp_current_selection();
  g_selected_by_menu[g_current_menu] = next_selectable_index(g_snapshot.selected_index, -1);
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell prev item=%s", g_snapshot.selected_label);
}

// Page the cursor by one visible window (kShellVisibleRows). Clamps at both ends with NO
// wrap (deliberately different from next/prev) so a single L1/R1 lands predictably at the
// list edge. ensure_selection_visible (via refresh_snapshot) snaps the scroll window to the
// multi-row jump in one shot -- no per-row scroll loop needed.
extern "C" void gtav_native_bridge_page(int dir) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  clamp_current_selection();
  uint32_t sel = g_snapshot.selected_index;
  const uint32_t step = kShellVisibleRows;
  if (dir < 0) {
    sel = (sel > step) ? sel - step : 0u;
  } else {
    sel = (sel + step < count) ? sel + step : count - 1u;
  }
  g_selected_by_menu[g_current_menu] = sel;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell page dir=%d item=%s", dir,
                     g_snapshot.selected_label);
}
extern "C" void gtav_native_bridge_page_prev(void) {
  gtav_native_bridge_page(-1);
}
extern "C" void gtav_native_bridge_page_next(void) {
  gtav_native_bridge_page(1);
}

// Jump the cursor to the first / last row (Triangle / Square). Pairs with paging for the big
// dynamic lists: page to get close, then Home/End to snap to an edge.
extern "C" void gtav_native_bridge_home(void) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  g_selected_by_menu[g_current_menu] = 0u;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell home item=%s", g_snapshot.selected_label);
}
extern "C" void gtav_native_bridge_end(void) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  g_selected_by_menu[g_current_menu] = count - 1u;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell end item=%s", g_snapshot.selected_label);
}

// First alphabetic character of a label (lowercased), skipping a leading "* " favorite marker.
// Returns 0 if the label has no letter. Used by the letter-seek to find first-letter boundaries.
static char label_initial(const char* s) {
  if (!s) return 0;
  if (s[0] == '*' && s[1] == ' ') s += 2;
  for (; *s; ++s) {
    const char c = *s;
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    if (c >= 'a' && c <= 'z') return c;
  }
  return 0;
}

// Seek the cursor to the next (dir>=0) / previous (dir<0) first-letter boundary (R3 / L3). On a
// sorted catalog this turns a 1000-row scroll into a handful of presses; on an unsorted list it
// still steps to the next row whose initial differs (harmless). Pure cursor math over the
// in-memory labels -- no native calls.
extern "C" void gtav_native_bridge_letter_jump(int dir) {
  const ShellMenu* menu = shell_menu(g_current_menu);
  const uint32_t count = shell_item_count();
  if (!menu || count < 2u) return;
  clamp_current_selection();
  const uint32_t cur = g_snapshot.selected_index;
  const char from = label_initial(menu->items[cur].label);
  uint32_t i = cur;
  for (uint32_t steps = 0; steps < count; ++steps) {
    i = (dir < 0) ? ((i == 0u) ? count - 1u : i - 1u) : (i + 1u) % count;
    const char c = label_initial(menu->items[i].label);
    if (c && c != from) {
      // A backward seek first hits the LAST row of the previous letter group; walk back to that
      // group's FIRST row so repeated L3 presses move group-by-group, mirroring forward R3.
      if (dir < 0) {
        while (i > 0u && label_initial(menu->items[i - 1u].label) == c) --i;
      }
      g_selected_by_menu[g_current_menu] = i;
      break;
    }
  }
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell letter dir=%d item=%s", dir,
                     g_snapshot.selected_label);
}

// L1/R1 behaviour for non-cycler rows: page within the list, but when already at the list edge
// AND this is a child screen with sibling submenus, roll laterally into the previous/next sibling
// instead -- the console "page to the edge, then flip to the next tab" idiom. Long lists page
// first; short leaf screens (already at both edges) flip on the first press. Keeps the parent on
// the menu stack (just swaps g_current_menu), so the breadcrumb + Back stay correct.
extern "C" void gtav_native_bridge_page_or_sibling(int dir) {
  const uint32_t count = shell_item_count();
  if (!count) return;
  clamp_current_selection();
  const uint32_t sel = g_snapshot.selected_index;
  const int at_edge = (dir < 0) ? (sel == 0u) : (sel + 1u >= count);
  if (!at_edge || g_menu_depth == 0u) {
    gtav_native_bridge_page(dir);
    return;
  }
  const ShellMenu* parent = shell_menu(g_menu_stack[g_menu_depth - 1u]);
  if (!parent) {
    gtav_native_bridge_page(dir);
    return;
  }
  uint32_t sibs[64];
  uint32_t nsib = 0u, cur_at = 0u;
  int found = 0;
  for (uint32_t i = 0; i < parent->item_count && nsib < 64u; ++i) {
    const ShellItem* it = &parent->items[i];
    if (it->type != SHELL_ROW_SUBMENU || it->submenu >= SHELL_MENU_COUNT) continue;
    if (it->submenu == g_current_menu) {
      cur_at = nsib;
      found = 1;
    }
    sibs[nsib++] = it->submenu;
  }
  if (!found || nsib < 2u) {
    gtav_native_bridge_page(dir);  // no siblings to roll across; stays put at the edge
    return;
  }
  const uint32_t target = (dir < 0) ? (cur_at + nsib - 1u) % nsib : (cur_at + 1u) % nsib;
  g_current_menu = sibs[target];
  clamp_current_selection();  // sticky cursor restores the row last focused in the sibling
  g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell sibling dir=%d menu=%s", dir,
                     shell_menu(g_current_menu)->label);
}

// True when the cursor sits on an adjustable list cycler that should fast-step (jump several
// values) on a page button. The vehicle-class cycler is excluded -- each step rebuilds the
// whole spawner list, so a coarse multi-step there would be expensive.
extern "C" int gtav_native_bridge_selected_faststeppable(void) {
  const ShellItem* item = selected_item();
  return item && item->type == SHELL_ROW_LIST && !shell_item_is_unavailable(item) &&
         item->action != GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_CLASS &&
         item->action != GTAV_NATIVE_SHELL_ACTION_CYCLE_PED_CATEGORY &&
         item->action != GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_CATEGORY &&
         item->action != GTAV_NATIVE_SHELL_ACTION_CYCLE_SCENARIO_CATEGORY;
}

// Pin/unpin the selected row to the global Quick menu, or toggle a browser favorite on a spawn/skin
// row. Bound to R3 (the right-stick click) -- a dedicated gesture, separate from the D-pad value
// adjust. The browser-favorite branch is checked FIRST because spawn rows are ACTION rows; the
// Quick pin handles every other action/toggle row. Runs regardless of the row's unavailable
// (hook-gated) state, so Quick / favorites can be curated before the game-thread hook is live. The
// whole profile is saved immediately so pins and favorites survive a restart.
extern "C" void gtav_native_bridge_pin_selected(void) {
  const ShellItem* item = selected_item();
  if (!item) return;
  if (item->param != 0u) {
    if (item->action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE) {
      hashlist_toggle(g_fav_vehicles, GTAV_FAVORITE_VEHICLES_MAX, item->param);
      rebuild_spawner_items();
      clamp_current_selection();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // persist immediately so favorites survive a restart
#endif
      republish_snapshot();
      return;
    }
    if (item->action == GTAV_NATIVE_SHELL_ACTION_SPAWN_PED ||
        item->action == GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL ||
        item->action == GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD) {
      hashlist_toggle(g_fav_peds, GTAV_FAVORITE_PEDS_MAX, item->param);
      rebuild_ped_pickers();
      clamp_current_selection();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // persist immediately so favorites survive a restart
#endif
      republish_snapshot();
      return;
    }
  }
  if ((item->type == SHELL_ROW_ACTION || item->type == SHELL_ROW_TOGGLE) &&
      item->action != GTAV_NATIVE_SHELL_ACTION_NONE) {
    const int pinned = quick_toggle(item->action, item->param);
    build_quick_items();
    clamp_current_selection();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
    gtav_features_profile_save_default();  // persist immediately so pins survive a restart
#endif
    gtav_native_bridge_push_toast(pinned ? "Pinned to Quick" : "Unpinned from Quick",
                                  GTAV_FEATURE_RESULT_OK);
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell quick %s item=%s",
                       pinned ? "pin" : "unpin", g_snapshot.selected_label);
  }
}

// D-pad Left/Right on the selected row. SHELL_ROW_LIST rows cycle values and
// return their action with the direction encoded in the action param (2 =
// prev/left, 1 = next/right) WITHOUT moving the cursor. On non-list rows,
// D-pad Left is a controller-friendly Back/Hide fallback for builds where
// Circle/Back is unavailable through the pad path. Pin/favorite is no longer here -- it has its
// own R3 gesture (gtav_native_bridge_pin_selected).
extern "C" uint32_t gtav_native_bridge_adjust(int dir) {
  const ShellItem* item = selected_item();
  if (!item) {
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  // Theme Editor channel: step the custom-theme colour directly (the row's param picks the
  // channel), ahead of the favorite/pin/generic-cycle paths. Pure render state. NOT saved per
  // step (a held ramp auto-repeats ~15x/s -- saving each would thrash profile.cfg); the palette
  // is persisted once on leaving the editor (see gtav_native_bridge_back_action).
  if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR) {
    gtav_native_bridge_adjust_custom_color(item->param, dir);
    g_value_flash_tick = g_last_worker_tick;  // motion feedback: flash the channel value
    republish_snapshot();
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  if (item->type != SHELL_ROW_LIST || shell_item_is_unavailable(item)) {
    if (dir < 0) return gtav_native_bridge_back_action();
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  g_value_flash_tick = g_last_worker_tick;  // motion feedback: flash the value on a cycler step
  // The vehicle-class filter is handled entirely here: stepping it immediately
  // re-filters the spawner list (it is not staged/applied via the feature layer
  // like weather/time). Keep the cursor on the cycler row so the user can keep
  // flicking through classes; the rebuilt rows appear below it.
  if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_CLASS) {
    uint32_t n = (uint32_t)(sizeof(kVehicleClassNames) / sizeof(kVehicleClassNames[0]));
    g_spawner_class_index =
        (dir < 0) ? (g_spawner_class_index + n - 1u) % n : (g_spawner_class_index + 1u) % n;
    rebuild_spawner_items();
    g_selected_by_menu[SHELL_MENU_VEHICLE_SPAWNER] = 0;
    g_scroll_by_menu[SHELL_MENU_VEHICLE_SPAWNER] = 0;
    clamp_current_selection();
    g_last_action_param = (dir < 0) ? 2u : 1u;
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell vehicle class=%s rows=%u",
                       kVehicleClassNames[g_spawner_class_index % n], g_spawner_item_count);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  // The ped-category filter is shared by the skin/spawn-ped/bodyguard browsers; stepping it
  // re-filters all three and resets their cursors to the cycler row (row 0), so whichever the
  // user is on stays put while the rows below it change.
  if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_PED_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kPedCategoryNames) / sizeof(kPedCategoryNames[0]));
    g_ped_category_index =
        (dir < 0) ? (g_ped_category_index + n - 1u) % n : (g_ped_category_index + 1u) % n;
    rebuild_ped_pickers();
    g_selected_by_menu[SHELL_MENU_SKIN_PICKER] = 0;
    g_scroll_by_menu[SHELL_MENU_SKIN_PICKER] = 0;
    g_selected_by_menu[SHELL_MENU_PED_SPAWNER] = 0;
    g_scroll_by_menu[SHELL_MENU_PED_SPAWNER] = 0;
    g_selected_by_menu[SHELL_MENU_BODYGUARD_SPAWNER] = 0;
    g_scroll_by_menu[SHELL_MENU_BODYGUARD_SPAWNER] = 0;
    clamp_current_selection();
    g_last_action_param = (dir < 0) ? 2u : 1u;
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell ped category=%s",
                       kPedCategoryNames[g_ped_category_index % n]);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  // The object-browser category filter: stepping it re-filters the object spawn list and resets
  // the cursor to the cycler row (row 0), so the user can keep flicking while the rows below
  // change. The placement cyclers (rows 1-4) are ordinary feature-layer list rows and fall
  // through to the staged/applied path below.
  if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kObjectCategoryNames) / sizeof(kObjectCategoryNames[0]));
    g_object_category_index =
        (dir < 0) ? (g_object_category_index + n - 1u) % n : (g_object_category_index + 1u) % n;
    build_object_spawner_items();
    g_selected_by_menu[SHELL_MENU_OBJECT_SPAWNER] = 0;
    g_scroll_by_menu[SHELL_MENU_OBJECT_SPAWNER] = 0;
    clamp_current_selection();
    g_last_action_param = (dir < 0) ? 2u : 1u;
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell object category=%s rows=%u",
                       kObjectCategoryNames[g_object_category_index % n],
                       g_object_spawner_item_count);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  // The scenario-browser category filter: stepping it re-filters the scenario list and resets the
  // cursor to the cycler row (row 0), so the user can keep flicking while the rows below change.
  if (item->action == GTAV_NATIVE_SHELL_ACTION_CYCLE_SCENARIO_CATEGORY) {
    uint32_t n = (uint32_t)(sizeof(kScenarioCategoryNames) / sizeof(kScenarioCategoryNames[0]));
    g_scenario_category_index =
        (dir < 0) ? (g_scenario_category_index + n - 1u) % n : (g_scenario_category_index + 1u) % n;
    rebuild_scenario_picker();
    g_selected_by_menu[SHELL_MENU_SELF_SCENARIO_PICKER] = 0;
    g_scroll_by_menu[SHELL_MENU_SELF_SCENARIO_PICKER] = 0;
    clamp_current_selection();
    g_last_action_param = (dir < 0) ? 2u : 1u;
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell scenario category=%s rows=%u",
                       kScenarioCategoryNames[g_scenario_category_index % n],
                       g_scenario_picker_item_count);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  g_last_action_param = (dir < 0) ? 2u : 1u;
  g_snapshot.last_action = item->action;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell adjust item=%s dir=%d",
                     g_snapshot.selected_label, dir);
  return item->action;
}

extern "C" uint32_t gtav_native_bridge_left(void) {
  return gtav_native_bridge_adjust(-1);
}
extern "C" uint32_t gtav_native_bridge_right(void) {
  return gtav_native_bridge_adjust(1);
}

extern "C" int gtav_native_bridge_selected_is_cycler(void) {
  const ShellItem* item = selected_item();
  return (item && item->type == SHELL_ROW_LIST) ? 1 : 0;
}

extern "C" uint32_t gtav_native_bridge_activate(void) {
  const ShellItem* item = selected_item();
  uint32_t action = GTAV_NATIVE_SHELL_ACTION_NONE;
  g_activate_flash_tick = g_last_worker_tick;  // motion feedback: flash the selected row on Cross

  if (item && item->type == SHELL_ROW_SUBMENU && item->submenu != SHELL_MENU_NONE &&
      item->submenu < SHELL_MENU_COUNT) {
    if (g_menu_depth < kShellMaxMenuDepth) {
      g_menu_stack[g_menu_depth++] = g_current_menu;
    }
    g_current_menu = item->submenu;
    // Sticky cursor: keep the row last focused in this submenu instead of snapping to row 0, so
    // toggling something deep in a long list and re-entering lands where you left. clamp +
    // ensure_selection_visible (inside clamp_current_selection) re-window a remembered index and
    // bound it if a dynamic browser rebuilt smaller. The filter cyclers still reset on rebuild.
    clamp_current_selection();
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell submenu=%s item=%s",
                       shell_menu(g_current_menu)->label, g_snapshot.selected_label);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }

  if (shell_item_is_unavailable(item)) {
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
    republish_snapshot();
    emit_unavailable_item(item);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }

  if (item) {
    action = item->action;
    g_last_action_param = item->param;
    // Record the model in the browser "Recents" list. This is past the unavailable gate, so it
    // only fires when the spawn/skin action is actually being dispatched (hook live). Recents
    // ride along to disk at the next profile save (favoriting, Save Profile) -- not auto-saved
    // per spawn to avoid file churn.
    if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE) {
      recents_push(g_recent_vehicles, GTAV_RECENT_VEHICLES_MAX, item->param);
    } else if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_PED ||
               action == GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL ||
               action == GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD) {
      recents_push(g_recent_peds, GTAV_RECENT_PEDS_MAX, item->param);
    }
  }
  if (action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY) {
    g_snapshot.telemetry_enabled = !g_snapshot.telemetry_enabled;
    gtav_log_set_level(g_snapshot.telemetry_enabled ? GTAV_LOG_DEBUG : GTAV_LOG_INFO);
  }
  g_snapshot.last_action = action;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell activate item=%s action=%s",
                     g_snapshot.selected_label, gtav_native_bridge_action_name(action));
  return action;
}

extern "C" uint32_t gtav_native_bridge_last_action_param(void) {
  return g_last_action_param;
}

extern "C" void gtav_native_bridge_set_feature_toggles(uint64_t mask) {
  g_feature_toggle_mask = mask;
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, (int)g_snapshot.visible);
}

// Browser favorites/recents persistence bridge (see native_bridge.h). The buffers are the
// profile's fixed-capacity arrays; sizes are single-sourced from feature_profile.h so a memcpy
// is exact. set_* rebuilds the browsers so imported lists show their "* " markers immediately.
extern "C" void gtav_native_bridge_get_favorites(uint32_t* veh, uint32_t* ped) {
  if (veh) memcpy(veh, g_fav_vehicles, sizeof(g_fav_vehicles));
  if (ped) memcpy(ped, g_fav_peds, sizeof(g_fav_peds));
}

extern "C" void gtav_native_bridge_set_favorites(const uint32_t* veh, const uint32_t* ped) {
  if (veh) memcpy(g_fav_vehicles, veh, sizeof(g_fav_vehicles));
  if (ped) memcpy(g_fav_peds, ped, sizeof(g_fav_peds));
  rebuild_spawner_items();
  rebuild_ped_pickers();
}

extern "C" void gtav_native_bridge_get_recents(uint32_t* veh, uint32_t* ped) {
  if (veh) memcpy(veh, g_recent_vehicles, sizeof(g_recent_vehicles));
  if (ped) memcpy(ped, g_recent_peds, sizeof(g_recent_peds));
}

extern "C" void gtav_native_bridge_set_recents(const uint32_t* veh, const uint32_t* ped) {
  if (veh) memcpy(g_recent_vehicles, veh, sizeof(g_recent_vehicles));
  if (ped) memcpy(g_recent_peds, ped, sizeof(g_recent_peds));
}

// Global Quick pins persistence bridge (see native_bridge.h). Buffers are the profile's
// fixed-capacity parallel arrays; sizes are single-sourced from feature_profile.h. set_* rebuilds
// the Quick menu so an imported pin list shows immediately.
extern "C" void gtav_native_bridge_get_quick(uint32_t* action, uint32_t* param) {
  if (action) memcpy(action, g_quick_action, sizeof(g_quick_action));
  if (param) memcpy(param, g_quick_param, sizeof(g_quick_param));
}

extern "C" void gtav_native_bridge_set_quick(const uint32_t* action, const uint32_t* param) {
  if (action) memcpy(g_quick_action, action, sizeof(g_quick_action));
  if (param) memcpy(g_quick_param, param, sizeof(g_quick_param));
  build_quick_items();
}

// Push the current display value for a SHELL_ROW_LIST cycler (e.g. weather/time),
// from the features module. Mirrors gtav_native_bridge_set_feature_toggles.
extern "C" void gtav_native_bridge_set_list_value(uint32_t action, const char* value) {
  if (action == GTAV_NATIVE_SHELL_ACTION_NONE) return;
  const char* v = value ? value : "";
  const size_t n = sizeof(g_list_values) / sizeof(g_list_values[0]);
  for (size_t i = 0; i < n; ++i) {
    if (g_list_values[i].action == action) {
      snprintf(g_list_values[i].value, sizeof(g_list_values[i].value), "%s", v);
      return;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    if (g_list_values[i].action == GTAV_NATIVE_SHELL_ACTION_NONE) {
      g_list_values[i].action = action;
      snprintf(g_list_values[i].value, sizeof(g_list_values[i].value), "%s", v);
      return;
    }
  }
  // Out of slots: every entry holds a different action. The write is dropped (this row will
  // render "..."), so latch + log the shortfall once instead of failing silently.
  if (!g_list_values_saturated) {
    g_list_values_saturated = 1;
    NB_TRACE("list-value store saturated (%u slots); cycler %u dropped -- raise g_list_values",
             (unsigned)n, action);
  }
}

// Diagnostic accessors for the list-value store. Capacity is the compile-time slot count;
// saturated() latches if set_list_value() ever dropped a write (see g_list_values_saturated).
extern "C" uint32_t gtav_native_bridge_list_values_capacity(void) {
  return (uint32_t)(sizeof(g_list_values) / sizeof(g_list_values[0]));
}
extern "C" int gtav_native_bridge_list_values_saturated(void) {
  return g_list_values_saturated;
}

extern "C" uint32_t gtav_native_bridge_toggle_telemetry(void) {
  g_snapshot.telemetry_enabled = !g_snapshot.telemetry_enabled;
  gtav_log_set_level(g_snapshot.telemetry_enabled ? GTAV_LOG_DEBUG : GTAV_LOG_INFO);
  g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY;
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell direct action=%s enabled=%u",
                     gtav_native_bridge_action_name(GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY),
                     g_snapshot.telemetry_enabled);
  return GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY;
}

extern "C" uint32_t gtav_native_bridge_back_action(void) {
  if (g_menu_depth > 0) {
    // Persist the custom palette once, when leaving the Theme Editor (the per-step adjusts don't
    // save, to avoid thrashing profile.cfg during a held colour ramp).
    const int leaving_theme_editor = (g_current_menu == SHELL_MENU_THEME_EDITOR ||
                                      g_current_menu == SHELL_MENU_THEME_EDITOR_MORE);
    g_current_menu = g_menu_stack[--g_menu_depth];
    g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
    if (leaving_theme_editor) gtav_features_profile_save_default();
#else
    (void)leaving_theme_editor;
#endif
    republish_snapshot();
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell back submenu=%s item=%s",
                       shell_menu(g_current_menu)->label, g_snapshot.selected_label);
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }

  g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_HIDE;
  republish_snapshot();
  emit_shell_event("shell back action=hide");
  return GTAV_NATIVE_SHELL_ACTION_HIDE;
}

extern "C" void gtav_native_bridge_back(void) {
  (void)gtav_native_bridge_back_action();
}

// Collapse straight to the root menu (Circle held past the hold threshold). Pops every level at
// once instead of one Circle tap per level. Preserves the Theme-Editor save side-effect that a
// single Back performs when leaving that screen, so a held collapse out of the editor still
// persists the palette. No-op when already at the root.
extern "C" uint32_t gtav_native_bridge_back_to_root(void) {
  if (g_menu_depth == 0u) {
    return GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  const int leaving_theme_editor =
      (g_current_menu == SHELL_MENU_THEME_EDITOR || g_current_menu == SHELL_MENU_THEME_EDITOR_MORE);
  g_menu_depth = 0u;
  g_current_menu = g_menu_stack[0];
  if (g_current_menu >= SHELL_MENU_COUNT) g_current_menu = SHELL_MENU_MAIN;
  g_snapshot.last_action = GTAV_NATIVE_SHELL_ACTION_NONE;
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (leaving_theme_editor) gtav_features_profile_save_default();
#else
  (void)leaving_theme_editor;
#endif
  clamp_current_selection();
  republish_snapshot();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell back-root submenu=%s item=%s",
                     shell_menu(g_current_menu)->label, g_snapshot.selected_label);
  return GTAV_NATIVE_SHELL_ACTION_NONE;
}

extern "C" void gtav_native_bridge_snapshot(GtavNativeShellSnapshot* out) {
  if (!out) return;
  refresh_snapshot(g_snapshot.worker_tick, g_snapshot.frame_count, (int)g_snapshot.visible);
  memcpy(out, &g_snapshot, sizeof(*out));
}

extern "C" const char* gtav_native_bridge_action_name(uint32_t action) {
  // Generated from the shared action-name list so the bridge and the feature layer can
  // never disagree (see include/gtavmenu/feature_actions.def). The per-slot mod pickers
  // are a contiguous range indexed by (action - MOD_SLOT_FIRST).
  if (action >= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST &&
      action <= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_LAST) {
    static const char* const kModNames[] = {
#define GTAV_MOD_SLOT_NAME(name) name,
#include "gtavmenu/feature_actions.def"
    };
    return kModNames[action - GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST];
  }
  switch (action) {
#define GTAV_ACTION_NAME(suffix, name)    \
  case GTAV_NATIVE_SHELL_ACTION_##suffix: \
    return name;
#include "gtavmenu/feature_actions.def"
    default:
      return "none";
  }
}
