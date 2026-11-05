#pragma once

/* Menu cadence.
 *
 * Menu durations (fades, toasts, pad auto-repeat) are counted in worker ticks: one call of
 * gtav_native_bridge_worker_tick. The PS5 worker ticks at 60 Hz by default, while a PS4 one-shot
 * worker ticks once per game frame (~30 Hz). Every such constant is written in 60 Hz ticks and
 * wrapped in GTAV_MENU_TICKS_60 so it keeps the same wall-clock duration on every target. At the
 * default 60 Hz the macro is the identity, so PS5 builds compile to the same constants. */

#ifndef GTAV_MENU_TICK_HZ
#define GTAV_MENU_TICK_HZ 60u
#endif

#define GTAV_MENU_TICKS_60_RAW(n) ((((n) * (GTAV_MENU_TICK_HZ)) + 30u) / 60u)
/* Rounded to the nearest tick, never below one tick. */
#define GTAV_MENU_TICKS_60(n) (GTAV_MENU_TICKS_60_RAW(n) ? GTAV_MENU_TICKS_60_RAW(n) : 1u)
