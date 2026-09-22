#include "gtavmenu/patch_broker.h"
#include "gtavmenu/strutil.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(GtavPatchBrokerState) == GTAV_PATCH_BROKER_SIZE,
               "GtavPatchBrokerState size changed");

GtavPatchBrokerState gtav_patch_broker_state __attribute__((used, visibility("default"))) = {
    .magic = GTAV_PATCH_BROKER_MAGIC,
    .abi_version = GTAV_PATCH_BROKER_ABI_VERSION,
    .struct_size = sizeof(GtavPatchBrokerState),
    .state = GTAV_PATCH_BROKER_STATE_DISABLED,
    .request_kind = GTAV_PATCH_BROKER_REQUEST_NONE,
};

const char gtav_patch_broker_marker[] __attribute__((used, visibility("default"))) =
    "GTAVMENU_PATCH_BROKER_V1";

static void init_header(void) {
  gtav_patch_broker_state.magic = GTAV_PATCH_BROKER_MAGIC;
  gtav_patch_broker_state.abi_version = GTAV_PATCH_BROKER_ABI_VERSION;
  gtav_patch_broker_state.struct_size = sizeof(GtavPatchBrokerState);
}

static void encode_abs_jump(uint8_t* out, uint32_t out_len, uint64_t destination) {
  if (!out || out_len < 14u) return;
  memset(out, 0x90, out_len);
  out[0] = 0xff;
  out[1] = 0x25;
  out[2] = 0x00;
  out[3] = 0x00;
  out[4] = 0x00;
  out[5] = 0x00;
  memcpy(out + 6, &destination, sizeof(destination));
}

void gtav_patch_broker_reset(void) {
  memset(&gtav_patch_broker_state, 0, sizeof(gtav_patch_broker_state));
  init_header();
  gtav_patch_broker_state.state = GTAV_PATCH_BROKER_STATE_DISABLED;
  gtav_patch_broker_state.request_kind = GTAV_PATCH_BROKER_REQUEST_NONE;
}

void gtav_patch_broker_set_state(uint32_t state, uint32_t error, const char* note) {
  init_header();
  gtav_patch_broker_state.state = state;
  gtav_patch_broker_state.error = error;
  gtav_copy_string(gtav_patch_broker_state.note, sizeof(gtav_patch_broker_state.note), note);
}

void gtav_patch_broker_publish_frame_hook(uint64_t target_addr, uint32_t patch_len,
                                          uint32_t stolen_len, const uint8_t* expected,
                                          uint32_t expected_len, uint64_t thunk_addr,
                                          uint64_t gateway_addr, uint64_t continuation_addr,
                                          const char* target_id) {
  gtav_patch_broker_reset();
  if (!target_addr || !thunk_addr || patch_len < 14u || patch_len > GTAV_PATCH_BROKER_MAX_BYTES ||
      stolen_len > GTAV_PATCH_BROKER_MAX_BYTES || expected_len > GTAV_PATCH_BROKER_MAX_BYTES ||
      expected_len < patch_len || !expected) {
    gtav_patch_broker_set_state(GTAV_PATCH_BROKER_STATE_ERROR,
                                GTAV_PATCH_BROKER_ERROR_INVALID_REQUEST,
                                "invalid broker frame-hook request");
    return;
  }

  gtav_patch_broker_state.state = GTAV_PATCH_BROKER_STATE_READY;
  gtav_patch_broker_state.request_kind = GTAV_PATCH_BROKER_REQUEST_FRAME_HOOK;
  gtav_patch_broker_state.flags =
      GTAV_PATCH_BROKER_FLAG_RESTORE_REQUIRED | GTAV_PATCH_BROKER_FLAG_EXTERNAL_WRITE_ONLY;
  gtav_patch_broker_state.error = GTAV_PATCH_BROKER_ERROR_NONE;
  gtav_patch_broker_state.target_addr = target_addr;
  gtav_patch_broker_state.thunk_addr = thunk_addr;
  gtav_patch_broker_state.gateway_addr = gateway_addr;
  gtav_patch_broker_state.continuation_addr = continuation_addr;
  gtav_patch_broker_state.patch_len = patch_len;
  gtav_patch_broker_state.stolen_len = stolen_len;
  gtav_patch_broker_state.expected_len = expected_len;
  gtav_patch_broker_state.jump_len = 14u;
  gtav_patch_broker_state.restore_len = stolen_len;
  memcpy(gtav_patch_broker_state.expected, expected, expected_len);
  memcpy(gtav_patch_broker_state.original, expected, expected_len);
  encode_abs_jump(gtav_patch_broker_state.jump, GTAV_PATCH_BROKER_MAX_BYTES, thunk_addr);
  gtav_copy_string(gtav_patch_broker_state.target_id, sizeof(gtav_patch_broker_state.target_id),
                   target_id);
  snprintf(gtav_patch_broker_state.note, sizeof(gtav_patch_broker_state.note),
           "external frame hook ready");
}

void gtav_patch_broker_tick(uint64_t ticks, uint64_t thunk_call_count, uint64_t jobs_run) {
  if (gtav_patch_broker_state.magic != GTAV_PATCH_BROKER_MAGIC) {
    init_header();
  }
  gtav_patch_broker_state.broker_updated_ticks = ticks;
  gtav_patch_broker_state.thunk_call_count = thunk_call_count;
  gtav_patch_broker_state.jobs_run = jobs_run;
}
