#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PATCH_BROKER_MAGIC 0x4b52425056415447ull
#define GTAV_PATCH_BROKER_ABI_VERSION 1u
#define GTAV_PATCH_BROKER_MAX_BYTES 32u
#define GTAV_PATCH_BROKER_TARGET_ID_LEN 64u
#define GTAV_PATCH_BROKER_NOTE_LEN 96u
#define GTAV_PATCH_BROKER_SIZE 368u

enum {
  GTAV_PATCH_BROKER_STATE_DISABLED = 0,
  GTAV_PATCH_BROKER_STATE_READY = 1,
  GTAV_PATCH_BROKER_STATE_INSTALLED = 2,
  GTAV_PATCH_BROKER_STATE_RESTORED = 3,
  GTAV_PATCH_BROKER_STATE_ERROR = 4,
};

enum {
  GTAV_PATCH_BROKER_REQUEST_NONE = 0,
  GTAV_PATCH_BROKER_REQUEST_FRAME_HOOK = 1,
};

enum {
  GTAV_PATCH_BROKER_FLAG_RESTORE_REQUIRED = 1u << 0,
  GTAV_PATCH_BROKER_FLAG_EXTERNAL_WRITE_ONLY = 1u << 1,
};

enum {
  GTAV_PATCH_BROKER_ERROR_NONE = 0,
  GTAV_PATCH_BROKER_ERROR_INVALID_REQUEST = 1,
  GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED = 2,
  GTAV_PATCH_BROKER_ERROR_INSTALL_FAILED = 3,
  GTAV_PATCH_BROKER_ERROR_RESTORE_FAILED = 4,
};

typedef struct GtavPatchBrokerState {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t state;
  uint32_t request_kind;
  uint32_t flags;
  uint32_t error;
  uint64_t target_addr;
  uint64_t thunk_addr;
  uint64_t gateway_addr;
  uint64_t continuation_addr;
  uint64_t broker_updated_ticks;
  uint64_t thunk_call_count;
  uint64_t jobs_run;
  uint32_t patch_len;
  uint32_t stolen_len;
  uint32_t expected_len;
  uint32_t jump_len;
  uint32_t restore_len;
  uint32_t reserved0;
  uint8_t expected[GTAV_PATCH_BROKER_MAX_BYTES];
  uint8_t original[GTAV_PATCH_BROKER_MAX_BYTES];
  uint8_t jump[GTAV_PATCH_BROKER_MAX_BYTES];
  char target_id[GTAV_PATCH_BROKER_TARGET_ID_LEN];
  char note[GTAV_PATCH_BROKER_NOTE_LEN];
} GtavPatchBrokerState;

extern GtavPatchBrokerState gtav_patch_broker_state;
extern const char gtav_patch_broker_marker[];

void gtav_patch_broker_reset(void);
void gtav_patch_broker_set_state(uint32_t state, uint32_t error, const char* note);
void gtav_patch_broker_publish_frame_hook(uint64_t target_addr, uint32_t patch_len,
                                          uint32_t stolen_len, const uint8_t* expected,
                                          uint32_t expected_len, uint64_t thunk_addr,
                                          uint64_t gateway_addr, uint64_t continuation_addr,
                                          const char* target_id);
void gtav_patch_broker_tick(uint64_t ticks, uint64_t thunk_call_count, uint64_t jobs_run);

#ifdef __cplusplus
}
#endif
