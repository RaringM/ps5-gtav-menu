#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_DETOUR_JUMP_LEN 14u
#define GTAV_MENU_DETOUR_MAX_LEN 32u
#define GTAV_MENU_DETOUR_GATEWAY_MAX_LEN (GTAV_MENU_DETOUR_MAX_LEN + GTAV_MENU_DETOUR_JUMP_LEN)

typedef struct GtavDetour {
  uintptr_t address;
  size_t length;
  void* gateway;
  size_t gateway_length;
  uint8_t original[GTAV_MENU_DETOUR_MAX_LEN];
  uint8_t patched[GTAV_MENU_DETOUR_MAX_LEN];
  int prevalidated_no_read;
  int installed;
} GtavDetour;

typedef void (*GtavDetourPublishOriginalFn)(void* original, void* context);

int gtav_detour_validate(uintptr_t address, const uint8_t* expected, size_t expected_len);
int gtav_detour_encode_abs_jump(uint8_t* out, size_t out_len, void* destination);
int gtav_detour_stolen_window_is_safe(const uint8_t* code, size_t len);
/* Smallest whole-instruction window length (>= the 14-byte abs jump) that can be
 * safely stolen from this prologue, or -1 if it contains a RIP-relative or
 * position-dependent instruction before the jump length is reached. */
int gtav_detour_safe_patch_len(const uint8_t* code, size_t max_len);
int gtav_detour_build_gateway(uint8_t* out, size_t out_len, uintptr_t address,
                              const uint8_t* stolen, size_t stolen_len);
int gtav_detour_install_abs_jump(GtavDetour* detour, uintptr_t address, void* destination,
                                 size_t patch_len, const uint8_t* expected, size_t expected_len,
                                 int dry_run);
int gtav_detour_install_abs_jump_prevalidated(GtavDetour* detour, uintptr_t address,
                                              void* destination, size_t patch_len,
                                              const uint8_t* original, size_t original_len,
                                              int dry_run);
int gtav_detour_install_trampoline(GtavDetour* detour, uintptr_t address, void* destination,
                                   size_t patch_len, const uint8_t* expected, size_t expected_len,
                                   void** original_out);
int gtav_detour_prepare_trampoline_prevalidated(GtavDetour* detour, uintptr_t address,
                                                size_t patch_len, const uint8_t* original,
                                                size_t original_len, void** original_out);
int gtav_detour_probe_patch_protection(uintptr_t address, size_t patch_len);
int gtav_detour_install_trampoline_prevalidated(GtavDetour* detour, uintptr_t address,
                                                void* destination, size_t patch_len,
                                                const uint8_t* original, size_t original_len,
                                                void** original_out);
int gtav_detour_install_trampoline_prevalidated_publish(
    GtavDetour* detour, uintptr_t address, void* destination, size_t patch_len,
    const uint8_t* original, size_t original_len, GtavDetourPublishOriginalFn publish_original,
    void* publish_context, void** original_out);
int gtav_detour_restore(GtavDetour* detour);

#ifdef __cplusplus
}
#endif
