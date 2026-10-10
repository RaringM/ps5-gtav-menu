"""Weapon wheel icons for add-on weapons (the pack.cfg ``wicon`` row).

The HUD movie (scaleform_generic.rpf hud.gfx, 01.010.002) draws a wheel icon with
``wT<wheel slot>.gotoAndStop("INT" + weapon hash)`` on the symbol ``SLOT_WEAPONS_<wheel slot>``. The
hash is the CWeaponInfo name hash; no CWeaponInfo field changes it, and the wheel hands the same
hash back to the game as the selection, so it cannot be remapped either. A new weapon name has no
``INT<hash>`` frame label and shows an empty frame. Two ways give it an icon:

- a name that has a label with artwork but no retail CWeaponInfo (cut weapons), listed here per
  wheel slot (WICON-run1);
- a pack ``wicon WEAPON DONOR`` row: after the weapon loads, the worker adds the label
  ``INT<weapon hash>`` to the loaded movie's ``SLOT_WEAPONS_<n>`` and ``MASTER_WEAPONS`` frame-label
  tables, pointing at the frame of the retail DONOR (same wheel slot). The file is not patched. The
  pack builder adds a row to the slot's DEFAULT_DONORS weapon for every pack weapon that has neither
  (a weapon without a label shows the wheel's last drawn icon, WICON-run2).
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from collections.abc import Iterable

from .hashes import joaat

# CWeaponInfo WheelSlot enum order = hud.gfx SLOT_WEAPONS_<n>.
WHEEL_SLOTS = (
    "WHEEL_PISTOL",
    "WHEEL_SMG",
    "WHEEL_RIFLE",
    "WHEEL_SNIPER",
    "WHEEL_UNARMED_MELEE",
    "WHEEL_SHOTGUN",
    "WHEEL_HEAVY",
    "WHEEL_THROWABLE_SPECIAL",
)

# Names whose INT<signed hash> label in SLOT_WEAPONS_<n> places artwork (also in MASTER_WEAPONS)
# while no retail weapons meta defines them. WEAPON_LOUDHAILER is labelled in SLOT_WEAPONS_7 but
# that frame is empty, so it is not listed. The worker still refuses a name that exists live.
BORROWABLE = {
    "WHEEL_SMG": ("WEAPON_ASSAULTMG",),
    "WHEEL_RIFLE": ("WEAPON_PROGRAMMABLEAR",),
    "WHEEL_SNIPER": ("WEAPON_ASSAULTSNIPER",),
    "WHEEL_THROWABLE_SPECIAL": ("WEAPON_THERMALCHARGE",),
}


# One retail donor per wheel slot for pack weapons without their own icon. A weapon with no frame
# label does not clear the wheel sprite, so it shows whatever icon was drawn last (WICON-run2): every
# pack weapon gets a ``wicon`` row, by default to this donor. Names are public weapon identifiers.
# Source (2026-10-07, PPSA04264 01.010.002 game copy, read-only): the WheelSlot of each donor in
# common.rpf data/ai/weapons.meta, and its INT<signed hash> label present in both the matching
# SLOT_WEAPONS_<n> sprite and MASTER_WEAPONS of update.rpf scaleform_generic.rpf hud.gfx
# (its INT frame labels). The worker still checks the live WheelSlot.
DEFAULT_DONORS = {
    "WHEEL_PISTOL": "WEAPON_PISTOL",
    "WHEEL_SMG": "WEAPON_SMG",
    "WHEEL_RIFLE": "WEAPON_CARBINERIFLE",
    "WHEEL_SNIPER": "WEAPON_SNIPERRIFLE",
    "WHEEL_UNARMED_MELEE": "WEAPON_KNIFE",
    "WHEEL_SHOTGUN": "WEAPON_PUMPSHOTGUN",
    "WHEEL_HEAVY": "WEAPON_RPG",
    "WHEEL_THROWABLE_SPECIAL": "WEAPON_GRENADE",
}


def signed_hash(name: str) -> int:
    """The value hud.gfx frame labels use: INT + joaat as a signed 32-bit integer."""
    value = joaat(name)
    return value - (1 << 32) if value & 0x80000000 else value


def weapon_wheel_slots(meta: str | bytes) -> list[tuple[str, str]]:
    """(Name, WheelSlot) of every CWeaponInfo in a weapons meta (XML)."""
    root = ET.fromstring(meta)
    return [
        ((item.findtext("Name") or "").strip(), (item.findtext("WheelSlot") or "").strip())
        for item in root.iter("Item")
        if item.get("type") == "CWeaponInfo"
    ]


def has_wheel_icon(name: str, wheel_slot: str) -> bool:
    return name.upper() in BORROWABLE.get(wheel_slot, ())


def missing_wheel_icons(meta: str | bytes, aliased: Iterable[str] = ()) -> list[tuple[str, str]]:
    """(Name, WheelSlot) of every CWeaponInfo with neither its own wheel icon nor a ``wicon`` row.

    `aliased`: weapon names a ``wicon`` row covers (case-insensitive). Borrowed cut names placed in
    their own wheel slot have art and are not listed.
    """
    covered = {name.upper() for name in aliased}
    return [
        (name, slot)
        for name, slot in weapon_wheel_slots(meta)
        if name and not has_wheel_icon(name, slot) and name.upper() not in covered
    ]


def default_wheel_icons(meta: str | bytes, aliased: Iterable[str] = ()) -> list[tuple[str, str]]:
    """Lowercase ``wicon`` rows (weapon, default donor) for every CWeaponInfo that has no wheel icon
    and no row in `aliased`; weapons whose WheelSlot is not a wheel slot are skipped (icon_warnings
    reports them)."""
    return [
        (name.lower(), DEFAULT_DONORS[slot].lower())
        for name, slot in missing_wheel_icons(meta, aliased)
        if slot in DEFAULT_DONORS and name.upper() != DEFAULT_DONORS[slot]
    ]


def icon_warnings(meta: str | bytes, aliased: Iterable[str] = ()) -> list[str]:
    """One line per add-on CWeaponInfo that the weapon wheel will show without an icon.

    `aliased`: weapon names that a ``wicon`` row gives a donor's icon (case-insensitive).
    """
    covered = {name.upper() for name in aliased}
    warnings = []
    for name, slot in weapon_wheel_slots(meta):
        if has_wheel_icon(name, slot):
            continue
        if name.upper() in covered:
            if slot not in WHEEL_SLOTS:
                warnings.append(f"{name}: WheelSlot {slot or 'missing'} is not one of {', '.join(WHEEL_SLOTS)}")
            continue
        home = [s for s, names in BORROWABLE.items() if name.upper() in names]
        if home:
            warnings.append(f"{name}: its wheel icon is in {home[0]}, but WheelSlot is {slot or 'missing'}")
            continue
        if slot not in WHEEL_SLOTS:
            warnings.append(f"{name}: WheelSlot {slot or 'missing'} is not one of {', '.join(WHEEL_SLOTS)}")
            continue
        free = ", ".join(BORROWABLE.get(slot, ())) or "none"
        warnings.append(
            f"{name}: no weapon wheel icon (hud.gfx SLOT_WEAPONS_{WHEEL_SLOTS.index(slot)} has no frame "
            f"INT{signed_hash(name)}); names with an icon in {slot}: {free}; or alias a retail "
            f"{slot} weapon's icon with a wicon row (--wheel-icon)"
        )
    return warnings
