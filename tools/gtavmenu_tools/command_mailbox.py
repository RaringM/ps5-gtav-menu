"""Host codec for the worker command mailbox (``include/gtavmenu/command_mailbox.h``).

The PS5 client (``tools/menu_client.py``) and the PS4 live tooling (``ps4_live.py``) share this
module. A host request writes ``command``/``status``/``argument`` first and then bumps
``request_sequence``; the worker consumes at most one request per tick and completes it by writing
``status``/``last_command``/``last_result`` before ``ack_sequence``. The literal tables below are
checked against the C header by ``tests/test_command_mailbox.py``.
"""

from __future__ import annotations

import struct

MAILBOX_MAGIC = 0x584F424D56415447  # "GTAVMBOX"
MAILBOX_ABI_VERSION = 1
MAILBOX_LAST_COMMAND_LEN = 32
MAILBOX_RESULT_LEN = 80
MAILBOX_FORMAT = f"<QIIQQIIQ{MAILBOX_LAST_COMMAND_LEN}s{MAILBOX_RESULT_LEN}s"
MAILBOX_SIZE = struct.calcsize(MAILBOX_FORMAT)
MAILBOX_MAGIC_BYTES = struct.pack("<Q", MAILBOX_MAGIC)
REQUEST_SEQUENCE_OFFSET = struct.calcsize("<QII")
ACK_SEQUENCE_OFFSET = struct.calcsize("<QIIQ")
FIELDS_OFFSET = struct.calcsize("<QIIQQ")
FIELDS_FORMAT = "<IIQ"  # command, status, argument

STATUS_IDLE = 0
STATUS_PENDING = 1
STATUS_ACKED = 2
STATUS_UNKNOWN = 3
COMMAND_STATUS_NAMES = {
    STATUS_IDLE: "idle",
    STATUS_PENDING: "pending",
    STATUS_ACKED: "acked",
    STATUS_UNKNOWN: "unknown",
}

COMMAND_NONE = 0
COMMAND_TOGGLE = 1
COMMAND_NEXT = 2
COMMAND_PREV = 3
COMMAND_SELECT = 4
COMMAND_BACK = 5
COMMAND_STOP = 6
COMMAND_TELEMETRY = 7
COMMAND_GOD = 8
COMMAND_HEAL_ARMOR = 9
COMMAND_CLEAR_WANTED = 10
COMMAND_SHOW = 11
COMMAND_HIDE = 12
COMMAND_SET_RENDER_INTERVAL = 13
COMMAND_SET_WORKER_HZ = 14
COMMAND_SPAWN_VEHICLE = 15
COMMAND_LEFT = 16
COMMAND_RIGHT = 17
COMMAND_LOAD_MODULE = 18
COMMAND_ACTIVATE_ACTION = 19
COMMAND_SET_TOAST_TICKS = 20
COMMAND_HOTKEY = 21
COMMAND_PAGE_PREV = 22
COMMAND_PAGE_NEXT = 23
COMMAND_HOME = 24
COMMAND_END = 25
COMMAND_LETTER_PREV = 26
COMMAND_LETTER_NEXT = 27
COMMAND_BACK_ROOT = 28
COMMAND_PIN = 29
COMMAND_RENDER_DIAG_MODE = 30
COMMAND_RENDER_DIAG_CAPTURE = 31
COMMAND_RENDER_CYCLE_PROBE = 32
COMMAND_RENDER_PATH_LOW = 33
COMMAND_RENDER_PATH_HIGH = 34
COMMAND_RENDER_PATH_CONTROL = 35
COMMAND_RENDER_BANK_CONTROL = 36
COMMAND_RENDER_PHASE_SLOT = 37
COMMAND_RENDER_PHASE_CONTROL = 38
COMMAND_ACTIVATE_ACTION_PARAM = 39

# Display name per GTAV_MENU_COMMAND_* id (tests/test_menu_client_enum_sync.py keeps it complete).
COMMAND_NAMES = {
    COMMAND_NONE: "none",
    COMMAND_TOGGLE: "toggle",
    COMMAND_NEXT: "next",
    COMMAND_PREV: "prev",
    COMMAND_SELECT: "select",
    COMMAND_BACK: "back",
    COMMAND_STOP: "stop",
    COMMAND_TELEMETRY: "telemetry",
    COMMAND_GOD: "god",
    COMMAND_HEAL_ARMOR: "heal",
    COMMAND_CLEAR_WANTED: "wanted",
    COMMAND_SHOW: "show",
    COMMAND_HIDE: "hide",
    COMMAND_SET_RENDER_INTERVAL: "render_interval",
    COMMAND_SET_WORKER_HZ: "worker_hz",
    COMMAND_SPAWN_VEHICLE: "spawn",
    COMMAND_LEFT: "left",
    COMMAND_RIGHT: "right",
    COMMAND_LOAD_MODULE: "load_module",
    COMMAND_ACTIVATE_ACTION: "activate_action",
    COMMAND_SET_TOAST_TICKS: "toast_ticks",
    COMMAND_HOTKEY: "hotkey",
    COMMAND_PAGE_PREV: "page_prev",
    COMMAND_PAGE_NEXT: "page_next",
    COMMAND_HOME: "home",
    COMMAND_END: "end",
    COMMAND_LETTER_PREV: "letter_prev",
    COMMAND_LETTER_NEXT: "letter_next",
    COMMAND_BACK_ROOT: "back_root",
    COMMAND_PIN: "pin",
    COMMAND_RENDER_DIAG_MODE: "render_diag_mode",
    COMMAND_RENDER_DIAG_CAPTURE: "render_diag_capture",
    COMMAND_RENDER_CYCLE_PROBE: "render_cycle_probe",
    COMMAND_RENDER_PATH_LOW: "render_path_low",
    COMMAND_RENDER_PATH_HIGH: "render_path_high",
    COMMAND_RENDER_PATH_CONTROL: "render_path_control",
    COMMAND_RENDER_BANK_CONTROL: "render_bank_control",
    COMMAND_RENDER_PHASE_SLOT: "render_phase_slot",
    COMMAND_RENDER_PHASE_CONTROL: "render_phase_control",
    COMMAND_ACTIVATE_ACTION_PARAM: "activate_action_param",
}

# Accepted spellings for a command name on the command line (lowercase; spaces become dashes).
COMMAND_IDS = {
    "toggle": COMMAND_TOGGLE,
    "next": COMMAND_NEXT,
    "down": COMMAND_NEXT,
    "prev": COMMAND_PREV,
    "previous": COMMAND_PREV,
    "up": COMMAND_PREV,
    "select": COMMAND_SELECT,
    "activate": COMMAND_SELECT,
    "back": COMMAND_BACK,
    "stop": COMMAND_STOP,
    "telemetry": COMMAND_TELEMETRY,
    "god": COMMAND_GOD,
    "heal": COMMAND_HEAL_ARMOR,
    "armor": COMMAND_HEAL_ARMOR,
    "heal-armor": COMMAND_HEAL_ARMOR,
    "heal_armor": COMMAND_HEAL_ARMOR,
    "wanted": COMMAND_CLEAR_WANTED,
    "clear-wanted": COMMAND_CLEAR_WANTED,
    "clear_wanted": COMMAND_CLEAR_WANTED,
    "show": COMMAND_SHOW,
    "refresh": COMMAND_SHOW,
    "redraw": COMMAND_SHOW,
    "hide": COMMAND_HIDE,
    "render_interval": COMMAND_SET_RENDER_INTERVAL,
    "set_render_interval": COMMAND_SET_RENDER_INTERVAL,
    "render-interval": COMMAND_SET_RENDER_INTERVAL,
    "worker_hz": COMMAND_SET_WORKER_HZ,
    "worker-hz": COMMAND_SET_WORKER_HZ,
    "render-diag-mode": COMMAND_RENDER_DIAG_MODE,
    "render-diag-capture": COMMAND_RENDER_DIAG_CAPTURE,
    "render_diag_mode": COMMAND_RENDER_DIAG_MODE,
    "render_diag_capture": COMMAND_RENDER_DIAG_CAPTURE,
    "render-cycle-probe": COMMAND_RENDER_CYCLE_PROBE,
    "render-path-low": COMMAND_RENDER_PATH_LOW,
    "render-path-high": COMMAND_RENDER_PATH_HIGH,
    "render-path-control": COMMAND_RENDER_PATH_CONTROL,
    "render-phase-slot": COMMAND_RENDER_PHASE_SLOT,
    "render-phase-control": COMMAND_RENDER_PHASE_CONTROL,
    "render-bank-control": COMMAND_RENDER_BANK_CONTROL,
    "render_cycle_probe": COMMAND_RENDER_CYCLE_PROBE,
    "set_worker_hz": COMMAND_SET_WORKER_HZ,
    "spawn": COMMAND_SPAWN_VEHICLE,
    "spawn_vehicle": COMMAND_SPAWN_VEHICLE,
    "left": COMMAND_LEFT,
    "right": COMMAND_RIGHT,
    "load_module": COMMAND_LOAD_MODULE,
    "load-module": COMMAND_LOAD_MODULE,
    "activate_action": COMMAND_ACTIVATE_ACTION,
    "activate-action": COMMAND_ACTIVATE_ACTION,
    "activate_action_param": COMMAND_ACTIVATE_ACTION_PARAM,
    "activate-action-param": COMMAND_ACTIVATE_ACTION_PARAM,
    "toast_ticks": COMMAND_SET_TOAST_TICKS,
    "set_toast_ticks": COMMAND_SET_TOAST_TICKS,
    "toast-ticks": COMMAND_SET_TOAST_TICKS,
    "hotkey": COMMAND_HOTKEY,
    "page_prev": COMMAND_PAGE_PREV,
    "page-prev": COMMAND_PAGE_PREV,
    "page_next": COMMAND_PAGE_NEXT,
    "page-next": COMMAND_PAGE_NEXT,
    "home": COMMAND_HOME,
    "end": COMMAND_END,
    "letter_prev": COMMAND_LETTER_PREV,
    "letter-prev": COMMAND_LETTER_PREV,
    "letter_next": COMMAND_LETTER_NEXT,
    "letter-next": COMMAND_LETTER_NEXT,
    "back_root": COMMAND_BACK_ROOT,
    "back-root": COMMAND_BACK_ROOT,
    "pin": COMMAND_PIN,
}


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def command_id(value: str | int) -> int:
    """Resolve a command name, alias, or numeric id (``"0x13"`` works) to a known command id."""
    if isinstance(value, int):
        command = value
    else:
        text = value.strip().lower().replace(" ", "-")
        if not text:
            raise ValueError("empty command")
        if text in COMMAND_IDS:
            return COMMAND_IDS[text]
        try:
            command = int(text, 0)
        except ValueError as exc:
            raise ValueError(f"unknown menu command: {value}") from exc
    if command not in COMMAND_NAMES:
        raise ValueError(f"unknown menu command id: {command}")
    return command


def decode_command_mailbox(raw: bytes, address: int | None = None) -> dict[str, object]:
    """Decode and validate one mailbox snapshot (magic, ABI and declared size)."""
    if len(raw) < MAILBOX_SIZE:
        raise ValueError(f"command mailbox blob too small: {len(raw)} < {MAILBOX_SIZE}")
    (
        magic,
        abi_version,
        struct_size,
        request_sequence,
        ack_sequence,
        command,
        status,
        argument,
        last_command,
        last_result,
    ) = struct.unpack_from(MAILBOX_FORMAT, raw, 0)
    if magic != MAILBOX_MAGIC:
        raise ValueError(f"bad command mailbox magic: 0x{magic:016x}")
    if abi_version != MAILBOX_ABI_VERSION:
        raise ValueError(f"unsupported command mailbox ABI: {abi_version}")
    if struct_size < MAILBOX_SIZE or struct_size > len(raw):
        raise ValueError(f"invalid command mailbox struct size: {struct_size}")
    return {
        "address": address,
        "addressHex": f"0x{address:x}" if address is not None else None,
        "magic": f"0x{magic:016x}",
        "abiVersion": abi_version,
        "structSize": struct_size,
        "requestSequence": request_sequence,
        "ackSequence": ack_sequence,
        "command": command,
        "commandName": COMMAND_NAMES.get(command, f"unknown_{command}"),
        "status": status,
        "statusName": COMMAND_STATUS_NAMES.get(status, f"unknown_{status}"),
        "argument": argument,
        "argumentHex": f"0x{argument:x}",
        "lastCommand": _cstr(last_command),
        "lastResult": _cstr(last_result),
    }


def encode_request(command: int, argument: int, sequence: int) -> tuple[tuple[int, bytes], tuple[int, bytes]]:
    """Return the two ordered mailbox writes ``(offset, data)`` that post one request.

    The first write stores ``command``, ``status = PENDING`` and ``argument``; the second publishes
    ``request_sequence``. The writes must be issued in this order: the worker reads the fields only
    after it observes a new sequence.
    """
    if not 0 <= command <= 0xFFFFFFFF:
        raise ValueError(f"command id out of range: {command}")
    if not 0 <= argument < (1 << 64):
        raise ValueError(f"command argument out of range: {argument}")
    if not 0 < sequence < (1 << 64):
        raise ValueError(f"command sequence out of range: {sequence}")
    return (
        (FIELDS_OFFSET, struct.pack(FIELDS_FORMAT, command, STATUS_PENDING, argument)),
        (REQUEST_SEQUENCE_OFFSET, struct.pack("<Q", sequence)),
    )


def activate_argument(action: int, param: int | None = None) -> tuple[int, int]:
    """Return ``(command, argument)`` that activates a shell action through the mailbox."""
    if not 0 < action <= 0xFFFFFFFF:
        raise ValueError(f"shell action id out of range: {action}")
    if param is None:
        return COMMAND_ACTIVATE_ACTION, action
    if not 0 <= param <= 0xFFFFFFFF:
        raise ValueError(f"shell action parameter out of range: {param}")
    return COMMAND_ACTIVATE_ACTION_PARAM, action | (param << 32)
