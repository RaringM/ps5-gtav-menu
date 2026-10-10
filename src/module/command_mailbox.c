#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/strutil.h"

#include <string.h>

_Static_assert(sizeof(GtavMenuCommandMailbox) == GTAV_MENU_COMMAND_MAILBOX_SIZE,
               "GtavMenuCommandMailbox size changed");

GtavMenuCommandMailbox gtav_menu_command_mailbox __attribute__((used, visibility("default"))) = {
    .magic = GTAV_MENU_COMMAND_MAILBOX_MAGIC,
    .abi_version = GTAV_MENU_COMMAND_MAILBOX_ABI_VERSION,
    .struct_size = sizeof(GtavMenuCommandMailbox),
    .status = GTAV_MENU_COMMAND_STATUS_IDLE,
};

const char gtav_menu_command_mailbox_marker[] __attribute__((used, visibility("default"))) =
    "GTAVMENU_COMMAND_MAILBOX_V1";

const char* gtav_command_mailbox_command_name(uint32_t command) {
  switch (command) {
    case GTAV_MENU_COMMAND_TOGGLE:
      return "toggle";
    case GTAV_MENU_COMMAND_NEXT:
      return "next";
    case GTAV_MENU_COMMAND_PREV:
      return "prev";
    case GTAV_MENU_COMMAND_SELECT:
      return "select";
    case GTAV_MENU_COMMAND_BACK:
      return "back";
    case GTAV_MENU_COMMAND_STOP:
      return "stop";
    case GTAV_MENU_COMMAND_TELEMETRY:
      return "telemetry";
    case GTAV_MENU_COMMAND_GOD:
      return "god";
    case GTAV_MENU_COMMAND_HEAL_ARMOR:
      return "heal";
    case GTAV_MENU_COMMAND_CLEAR_WANTED:
      return "wanted";
    case GTAV_MENU_COMMAND_SHOW:
      return "show";
    case GTAV_MENU_COMMAND_HIDE:
      return "hide";
    case GTAV_MENU_COMMAND_SET_RENDER_INTERVAL:
      return "render_interval";
    case GTAV_MENU_COMMAND_SET_WORKER_HZ:
      return "worker_hz";
    case GTAV_MENU_COMMAND_SPAWN_VEHICLE:
      return "spawn";
    case GTAV_MENU_COMMAND_LEFT:
      return "left";
    case GTAV_MENU_COMMAND_RIGHT:
      return "right";
    case GTAV_MENU_COMMAND_LOAD_MODULE:
      return "load_module";
    case GTAV_MENU_COMMAND_SET_TOAST_TICKS:
      return "toast_ticks";
    case GTAV_MENU_COMMAND_PAGE_PREV:
      return "page_prev";
    case GTAV_MENU_COMMAND_PAGE_NEXT:
      return "page_next";
    case GTAV_MENU_COMMAND_HOME:
      return "home";
    case GTAV_MENU_COMMAND_END:
      return "end";
    case GTAV_MENU_COMMAND_LETTER_PREV:
      return "letter_prev";
    case GTAV_MENU_COMMAND_LETTER_NEXT:
      return "letter_next";
    case GTAV_MENU_COMMAND_BACK_ROOT:
      return "back_root";
    case GTAV_MENU_COMMAND_PIN:
      return "pin";
    case GTAV_MENU_COMMAND_RENDER_DIAG_MODE:
      return "render_diag_mode";
    case GTAV_MENU_COMMAND_RENDER_DIAG_CAPTURE:
      return "render_diag_capture";
    case GTAV_MENU_COMMAND_RENDER_CYCLE_PROBE:
      return "render_cycle_probe";
    case GTAV_MENU_COMMAND_RENDER_PATH_LOW:
      return "render_path_low";
    case GTAV_MENU_COMMAND_RENDER_PATH_HIGH:
      return "render_path_high";
    case GTAV_MENU_COMMAND_RENDER_PATH_CONTROL:
      return "render_path_control";
    case GTAV_MENU_COMMAND_RENDER_BANK_CONTROL:
      return "render_bank_control";
    case GTAV_MENU_COMMAND_RENDER_PHASE_SLOT:
      return "render_phase_slot";
    case GTAV_MENU_COMMAND_RENDER_PHASE_CONTROL:
      return "render_phase_control";
    case GTAV_MENU_COMMAND_ACTIVATE_ACTION_PARAM:
      return "activate_action_param";
    case GTAV_MENU_COMMAND_NONE:
    default:
      return "none";
  }
}

void gtav_command_mailbox_reset(void) {
  memset(&gtav_menu_command_mailbox, 0, sizeof(gtav_menu_command_mailbox));
  gtav_menu_command_mailbox.magic = GTAV_MENU_COMMAND_MAILBOX_MAGIC;
  gtav_menu_command_mailbox.abi_version = GTAV_MENU_COMMAND_MAILBOX_ABI_VERSION;
  gtav_menu_command_mailbox.struct_size = sizeof(GtavMenuCommandMailbox);
  gtav_menu_command_mailbox.status = GTAV_MENU_COMMAND_STATUS_IDLE;
  gtav_copy_string(gtav_menu_command_mailbox.last_result,
                   sizeof(gtav_menu_command_mailbox.last_result), "reset");
  __sync_synchronize();
}

int gtav_command_mailbox_consume(GtavMenuCommandRequest* request) {
  uint64_t sequence;
  uint32_t command;
  uint64_t argument;

  __sync_synchronize();

  if (gtav_menu_command_mailbox.magic != GTAV_MENU_COMMAND_MAILBOX_MAGIC ||
      gtav_menu_command_mailbox.abi_version != GTAV_MENU_COMMAND_MAILBOX_ABI_VERSION ||
      gtav_menu_command_mailbox.struct_size != sizeof(GtavMenuCommandMailbox)) {
    return 0;
  }

  sequence = gtav_menu_command_mailbox.request_sequence;
  if (!sequence || sequence == gtav_menu_command_mailbox.ack_sequence) {
    return 0;
  }

  command = gtav_menu_command_mailbox.command;
  argument = gtav_menu_command_mailbox.argument;
  gtav_menu_command_mailbox.status = GTAV_MENU_COMMAND_STATUS_PENDING;

  if (request) {
    request->sequence = sequence;
    request->command = command;
    request->argument = argument;
  }

  __sync_synchronize();
  return 1;
}

void gtav_command_mailbox_complete(uint64_t sequence, uint32_t status, const char* result) {
  if (!sequence) return;

  gtav_copy_string(gtav_menu_command_mailbox.last_command,
                   sizeof(gtav_menu_command_mailbox.last_command),
                   gtav_command_mailbox_command_name(gtav_menu_command_mailbox.command));
  gtav_copy_string(gtav_menu_command_mailbox.last_result,
                   sizeof(gtav_menu_command_mailbox.last_result), result);
  gtav_menu_command_mailbox.status = status;
  __sync_synchronize();
  gtav_menu_command_mailbox.ack_sequence = sequence;
  __sync_synchronize();
}
