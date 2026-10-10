#include "gtavmenu/feature_catalog.h"

// Vehicle spawn list. Generated from a datamined candidate superset and
// (optionally) trimmed to models this build can spawn; see
// devtools/generators/generate_vehicle_registry.py and data/vehicles/gtav-vehicle-names.txt.
// The generated header is an initializer list only, so this extern symbol and
// its ABI stay stable. Models are stock GTA V names with precomputed joaat
// hashes; spawn_vehicle() validates each with IS_MODEL_* before use.
const GtavVehicleEntry gtav_vehicle_catalog[] = {
#include "gtavmenu/vehicle_catalog_generated.h"
};

const uint32_t gtav_vehicle_catalog_count =
    (uint32_t)(sizeof(gtav_vehicle_catalog) / sizeof(gtav_vehicle_catalog[0]));

// Weapon pack granted by the "Give Weapon Pack" action. The hash column is
// joaat(name) precomputed with devtools/generators/generate_vehicle_registry.py's joaat(), so
// give_weapons() can pass weapon hashes straight to GIVE_WEAPON_TO_PED without calling
// GET_HASH_KEY on the game thread (matches how the vehicle catalog precomputes
// model_hash). Update the hash whenever a name changes.
const GtavWeaponEntry gtav_weapon_catalog[] = {
    // Pistols
    {"WEAPON_PISTOL", "Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x1b06d571u, 250},
    {"WEAPON_PISTOL_MK2", "Pistol Mk II", GTAV_WEAPON_CATEGORY_PISTOLS, 0xbfe256d4u, 250},
    {"WEAPON_COMBATPISTOL", "Combat Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x5ef9fec4u, 250},
    {"WEAPON_APPISTOL", "AP Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x22d8fe39u, 250},
    {"WEAPON_PISTOL50", "Pistol .50", GTAV_WEAPON_CATEGORY_PISTOLS, 0x99aeeb3bu, 250},
    {"WEAPON_HEAVYPISTOL", "Heavy Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0xd205520eu, 250},
    {"WEAPON_VINTAGEPISTOL", "Vintage Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x083839c4u, 250},
    {"WEAPON_SNSPISTOL", "SNS Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0xbfd21232u, 250},
    {"WEAPON_SNSPISTOL_MK2", "SNS Pistol Mk II", GTAV_WEAPON_CATEGORY_PISTOLS, 0x88374054u, 250},
    {"WEAPON_MARKSMANPISTOL", "Marksman Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0xdc4db296u, 36},
    {"WEAPON_REVOLVER", "Heavy Revolver", GTAV_WEAPON_CATEGORY_PISTOLS, 0xc1b3c3d1u, 100},
    {"WEAPON_REVOLVER_MK2", "Heavy Revolver Mk II", GTAV_WEAPON_CATEGORY_PISTOLS, 0xcb96392fu, 100},
    {"WEAPON_DOUBLEACTION", "Double-Action Revolver", GTAV_WEAPON_CATEGORY_PISTOLS, 0x97ea20b8u,
     100},
    {"WEAPON_NAVYREVOLVER", "Navy Revolver", GTAV_WEAPON_CATEGORY_PISTOLS, 0x917f6c8cu, 100},
    {"WEAPON_CERAMICPISTOL", "Ceramic Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x2b5ef5ecu, 250},
    {"WEAPON_PISTOLXM3", "WM 29 Pistol", GTAV_WEAPON_CATEGORY_PISTOLS, 0x1bc4fdb9u, 250},
    {"WEAPON_RAYPISTOL", "Up-n-Atomizer", GTAV_WEAPON_CATEGORY_PISTOLS, 0xaf3696a1u, 100},
    {"WEAPON_STUNGUN", "Stun Gun", GTAV_WEAPON_CATEGORY_PISTOLS, 0x3656c8c1u, 1},
    {"WEAPON_FLAREGUN", "Flare Gun", GTAV_WEAPON_CATEGORY_PISTOLS, 0x47757124u, 20},
    // SMG / machine guns
    {"WEAPON_MICROSMG", "Micro SMG", GTAV_WEAPON_CATEGORY_SMG_MG, 0x13532244u, 500},
    {"WEAPON_SMG", "SMG", GTAV_WEAPON_CATEGORY_SMG_MG, 0x2be6766bu, 500},
    {"WEAPON_SMG_MK2", "SMG Mk II", GTAV_WEAPON_CATEGORY_SMG_MG, 0x78a97cd0u, 500},
    {"WEAPON_ASSAULTSMG", "Assault SMG", GTAV_WEAPON_CATEGORY_SMG_MG, 0xefe7e2dfu, 500},
    {"WEAPON_COMBATPDW", "Combat PDW", GTAV_WEAPON_CATEGORY_SMG_MG, 0x0a3d4d34u, 500},
    {"WEAPON_MACHINEPISTOL", "Machine Pistol", GTAV_WEAPON_CATEGORY_SMG_MG, 0xdb1aa450u, 500},
    {"WEAPON_MINISMG", "Mini SMG", GTAV_WEAPON_CATEGORY_SMG_MG, 0xbd248b55u, 500},
    {"WEAPON_GUSENBERG", "Gusenberg Sweeper", GTAV_WEAPON_CATEGORY_SMG_MG, 0x61012683u, 500},
    {"WEAPON_TACTICALSMG", "Tactical SMG", GTAV_WEAPON_CATEGORY_SMG_MG, 0x38b1edf0u, 500},
    {"WEAPON_COMBATMG", "Combat MG", GTAV_WEAPON_CATEGORY_SMG_MG, 0x7fd62962u, 600},
    {"WEAPON_COMBATMG_MK2", "Combat MG Mk II", GTAV_WEAPON_CATEGORY_SMG_MG, 0xdbbd7280u, 600},
    {"WEAPON_MG", "MG", GTAV_WEAPON_CATEGORY_SMG_MG, 0x9d07f764u, 600},
    {"WEAPON_MINIGUN", "Minigun", GTAV_WEAPON_CATEGORY_SMG_MG, 0x42bf8a85u, 2000},
    // Rifles
    {"WEAPON_ASSAULTRIFLE", "Assault Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0xbfefff6du, 500},
    {"WEAPON_ASSAULTRIFLE_MK2", "Assault Rifle Mk II", GTAV_WEAPON_CATEGORY_RIFLES, 0x394f415cu,
     500},
    {"WEAPON_CARBINERIFLE", "Carbine Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0x83bf0278u, 500},
    {"WEAPON_CARBINERIFLE_MK2", "Carbine Rifle Mk II", GTAV_WEAPON_CATEGORY_RIFLES, 0xfad1f1c9u,
     500},
    {"WEAPON_SPECIALCARBINE", "Special Carbine", GTAV_WEAPON_CATEGORY_RIFLES, 0xc0a3098du, 500},
    {"WEAPON_SPECIALCARBINE_MK2", "Special Carbine Mk II", GTAV_WEAPON_CATEGORY_RIFLES, 0x969c3d67u,
     500},
    {"WEAPON_ADVANCEDRIFLE", "Advanced Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0xaf113f99u, 500},
    {"WEAPON_BULLPUPRIFLE", "Bullpup Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0x7f229f94u, 500},
    {"WEAPON_BULLPUPRIFLE_MK2", "Bullpup Rifle Mk II", GTAV_WEAPON_CATEGORY_RIFLES, 0x84d6fafdu,
     500},
    {"WEAPON_COMPACTRIFLE", "Compact Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0x624fe830u, 250},
    {"WEAPON_MILITARYRIFLE", "Military Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0x9d1f17e6u, 500},
    {"WEAPON_HEAVYRIFLE", "Heavy Rifle", GTAV_WEAPON_CATEGORY_RIFLES, 0xc78d71b4u, 500},
    {"WEAPON_TACTICALRIFLE", "Service Carbine", GTAV_WEAPON_CATEGORY_RIFLES, 0xd1d5f52bu, 500},
    {"WEAPON_RAYCARBINE", "Unholy Hellbringer", GTAV_WEAPON_CATEGORY_RIFLES, 0x476bf155u, 500},
    // Snipers
    {"WEAPON_SNIPERRIFLE", "Sniper Rifle", GTAV_WEAPON_CATEGORY_SNIPERS, 0x05fc3c11u, 100},
    {"WEAPON_HEAVYSNIPER", "Heavy Sniper", GTAV_WEAPON_CATEGORY_SNIPERS, 0x0c472fe2u, 100},
    {"WEAPON_HEAVYSNIPER_MK2", "Heavy Sniper Mk II", GTAV_WEAPON_CATEGORY_SNIPERS, 0x0a914799u,
     100},
    {"WEAPON_MARKSMANRIFLE", "Marksman Rifle", GTAV_WEAPON_CATEGORY_SNIPERS, 0xc734385au, 100},
    {"WEAPON_MARKSMANRIFLE_MK2", "Marksman Rifle Mk II", GTAV_WEAPON_CATEGORY_SNIPERS, 0x6a6c02e0u,
     100},
    {"WEAPON_PRECISIONRIFLE", "Precision Rifle", GTAV_WEAPON_CATEGORY_SNIPERS, 0x6e7dddecu, 100},
    // Shotguns
    {"WEAPON_PUMPSHOTGUN", "Pump Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x1d073a89u, 100},
    {"WEAPON_PUMPSHOTGUN_MK2", "Pump Shotgun Mk II", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x555af99au,
     100},
    {"WEAPON_ASSAULTSHOTGUN", "Assault Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0xe284c527u, 120},
    {"WEAPON_SAWNOFFSHOTGUN", "Sawed-Off Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x7846a318u, 100},
    {"WEAPON_BULLPUPSHOTGUN", "Bullpup Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x9d61e50fu, 100},
    {"WEAPON_HEAVYSHOTGUN", "Heavy Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x3aabbbaau, 100},
    {"WEAPON_DBSHOTGUN", "Double-Barrel Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0xef951fbbu, 100},
    {"WEAPON_AUTOSHOTGUN", "Sweeper Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x12e82d3du, 100},
    {"WEAPON_COMBATSHOTGUN", "Combat Shotgun", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0x05a96ba4u, 100},
    {"WEAPON_MUSKET", "Musket", GTAV_WEAPON_CATEGORY_SHOTGUNS, 0xa89cb99eu, 100},
    // Heavy / explosive
    {"WEAPON_GRENADELAUNCHER", "Grenade Launcher", GTAV_WEAPON_CATEGORY_HEAVY, 0xa284510bu, 50},
    {"WEAPON_GRENADELAUNCHER_SMOKE", "Smoke Grenade Launcher", GTAV_WEAPON_CATEGORY_HEAVY,
     0x4dd2dc56u, 10},
    {"WEAPON_RPG", "RPG", GTAV_WEAPON_CATEGORY_HEAVY, 0xb1ca77b1u, 25},
    {"WEAPON_HOMINGLAUNCHER", "Homing Launcher", GTAV_WEAPON_CATEGORY_HEAVY, 0x63ab0442u, 10},
    {"WEAPON_COMPACTLAUNCHER", "Compact Grenade Launcher", GTAV_WEAPON_CATEGORY_HEAVY, 0x0781fe4au,
     25},
    {"WEAPON_RAILGUN", "Railgun", GTAV_WEAPON_CATEGORY_HEAVY, 0x6d544c99u, 20},
    {"WEAPON_FIREWORK", "Firework Launcher", GTAV_WEAPON_CATEGORY_HEAVY, 0x7f7497e5u, 20},
    {"WEAPON_EMPLAUNCHER", "Compact EMP Launcher", GTAV_WEAPON_CATEGORY_HEAVY, 0xdb26713au, 20},
    {"WEAPON_RAYMINIGUN", "Widowmaker", GTAV_WEAPON_CATEGORY_HEAVY, 0xb62d1f67u, 2000},
    // Throwables
    {"WEAPON_GRENADE", "Grenade", GTAV_WEAPON_CATEGORY_THROWABLES, 0x93e220bdu, 25},
    {"WEAPON_STICKYBOMB", "Sticky Bomb", GTAV_WEAPON_CATEGORY_THROWABLES, 0x2c3731d9u, 25},
    {"WEAPON_PROXMINE", "Proximity Mine", GTAV_WEAPON_CATEGORY_THROWABLES, 0xab564b93u, 10},
    {"WEAPON_PIPEBOMB", "Pipe Bomb", GTAV_WEAPON_CATEGORY_THROWABLES, 0xba45e8b8u, 10},
    {"WEAPON_MOLOTOV", "Molotov", GTAV_WEAPON_CATEGORY_THROWABLES, 0x24b17070u, 25},
    {"WEAPON_SMOKEGRENADE", "Tear Gas", GTAV_WEAPON_CATEGORY_THROWABLES, 0xfdbc8a50u, 25},
    {"WEAPON_BZGAS", "BZ Gas", GTAV_WEAPON_CATEGORY_THROWABLES, 0xa0973d5eu, 25},
    {"WEAPON_FLARE", "Flare", GTAV_WEAPON_CATEGORY_THROWABLES, 0x497facc3u, 25},
    {"WEAPON_SNOWBALL", "Snowball", GTAV_WEAPON_CATEGORY_THROWABLES, 0x0787f0bbu, 10},
    {"WEAPON_BALL", "Ball", GTAV_WEAPON_CATEGORY_THROWABLES, 0x23c9f95cu, 10},
    // Melee
    {"WEAPON_KNIFE", "Knife", GTAV_WEAPON_CATEGORY_MELEE, 0x99b507eau, 1},
    {"WEAPON_BAT", "Baseball Bat", GTAV_WEAPON_CATEGORY_MELEE, 0x958a4a8fu, 1},
    {"WEAPON_MACHETE", "Machete", GTAV_WEAPON_CATEGORY_MELEE, 0xdd5df8d9u, 1},
    {"WEAPON_HATCHET", "Hatchet", GTAV_WEAPON_CATEGORY_MELEE, 0xf9dcbf2du, 1},
    {"WEAPON_SWITCHBLADE", "Switchblade", GTAV_WEAPON_CATEGORY_MELEE, 0xdfe37640u, 1},
    {"WEAPON_NIGHTSTICK", "Nightstick", GTAV_WEAPON_CATEGORY_MELEE, 0x678b81b1u, 1},
    {"WEAPON_HAMMER", "Hammer", GTAV_WEAPON_CATEGORY_MELEE, 0x4e875f73u, 1},
    {"WEAPON_CROWBAR", "Crowbar", GTAV_WEAPON_CATEGORY_MELEE, 0x84bd7bfdu, 1},
    {"WEAPON_GOLFCLUB", "Golf Club", GTAV_WEAPON_CATEGORY_MELEE, 0x440e4788u, 1},
    {"WEAPON_BOTTLE", "Broken Bottle", GTAV_WEAPON_CATEGORY_MELEE, 0xf9e6aa4bu, 1},
    {"WEAPON_DAGGER", "Antique Cavalry Dagger", GTAV_WEAPON_CATEGORY_MELEE, 0x92a27487u, 1},
    {"WEAPON_KNUCKLE", "Brass Knuckles", GTAV_WEAPON_CATEGORY_MELEE, 0xd8df3c3cu, 1},
    {"WEAPON_FLASHLIGHT", "Flashlight", GTAV_WEAPON_CATEGORY_MELEE, 0x8bb05fd7u, 1},
    {"WEAPON_POOLCUE", "Pool Cue", GTAV_WEAPON_CATEGORY_MELEE, 0x94117305u, 1},
    {"WEAPON_BATTLEAXE", "Battle Axe", GTAV_WEAPON_CATEGORY_MELEE, 0xcd274149u, 1},
    {"WEAPON_STONE_HATCHET", "Stone Hatchet", GTAV_WEAPON_CATEGORY_MELEE, 0x3813fc08u, 1},
    {"WEAPON_WRENCH", "Pipe Wrench", GTAV_WEAPON_CATEGORY_MELEE, 0x19044ee0u, 1},
    {"WEAPON_PIPE", "Metal Pipe", GTAV_WEAPON_CATEGORY_MELEE, 0x8cbd3112u, 1},
    {"WEAPON_CANDYCANE", "Candy Cane", GTAV_WEAPON_CATEGORY_MELEE, 0x6589186au, 1},
};

const uint32_t gtav_weapon_catalog_count =
    (uint32_t)(sizeof(gtav_weapon_catalog) / sizeof(gtav_weapon_catalog[0]));

// Ped model list (skin changer / spawn ped / bodyguard). Generated from the Menyoo
// community PedList dump and grouped by category; see devtools/generators/generate_ped_registry.py
// and data/peds/gtav-ped-names.txt. The three protagonists are fully stable in story mode; the
// other models load and look correct in free-roam but may lose special abilities and the
// game can revert them on a mission load or character switch. Cutscene/story-only models
// are the most likely to not stream -- the skin/spawn job validates each with IS_MODEL_*
// before use, so an unspawnable name degrades to "model not valid" rather than crashing.
// The generated header is an initializer list only, so this extern symbol and its ABI stay
// stable. Pass an on-target --validation file to trim it to confirmed-spawnable models.
const GtavPedModelEntry gtav_ped_model_catalog[] = {
#include "gtavmenu/ped_catalog_generated.h"
};

const uint32_t gtav_ped_model_catalog_count =
    (uint32_t)(sizeof(gtav_ped_model_catalog) / sizeof(gtav_ped_model_catalog[0]));

// Object spawner prop list. Generated from a curated, categorized prop list (see
// devtools/generators/generate_object_registry.py and data/objects/gtav-object-names.txt). Like the
// vehicle catalog, the generated header is an initializer-list only, so this extern symbol and its
// ABI stay stable. Models are stock GTA V prop names with precomputed joaat hashes;
// spawn_object_model_hash() validates each with IS_MODEL_* before CREATE_OBJECT, so an unknown
// name degrades to "model not valid".
const GtavObjectEntry gtav_object_catalog[] = {
#include "gtavmenu/object_catalog_generated.h"
};

const uint32_t gtav_object_catalog_count =
    (uint32_t)(sizeof(gtav_object_catalog) / sizeof(gtav_object_catalog[0]));

// Vehicle image-preview registry: {model, joaat(model), dict, texture, caption} rows mapping a
// highlighted vehicle to the streamed texture dict + texture name to DRAW_SPRITE. Generated from
// data/vehicles/vehicle-preview-textures.tsv (see
// devtools/generators/generate_vehicle_preview_catalog.py); the header is an initializer list only
// so this extern symbol/ABI stays stable. Only vehicles with a verified mapping are listed -- the
// render path shows a placeholder for anything not found here.
const GtavVehiclePreviewEntry gtav_vehicle_preview_catalog[] = {
#include "gtavmenu/vehicle_preview_catalog_generated.h"
};

const uint32_t gtav_vehicle_preview_catalog_count =
    (uint32_t)(sizeof(gtav_vehicle_preview_catalog) / sizeof(gtav_vehicle_preview_catalog[0]));

// Self > Scenarios browser list. Generated from a curated, categorized scenario-name list (see
// devtools/generators/generate_scenario_registry.py and data/scenarios/gtav-scenario-names.txt).
// Like the other catalogs, the generated header is an initializer-list only, so this extern symbol
// and its ABI stay stable. There is no joaat column: the scenario travels to the game-thread
// handler as its catalog index, which passes `name` straight to TASK_START_SCENARIO_IN_PLACE.
const GtavScenarioEntry gtav_scenario_catalog[] = {
#include "gtavmenu/scenario_catalog_generated.h"
};

const uint32_t gtav_scenario_catalog_count =
    (uint32_t)(sizeof(gtav_scenario_catalog) / sizeof(gtav_scenario_catalog[0]));

const GtavAnimEntry gtav_anim_catalog[] = {
#include "gtavmenu/anim_catalog_generated.h"
};

const uint32_t gtav_anim_catalog_count =
    (uint32_t)(sizeof(gtav_anim_catalog) / sizeof(gtav_anim_catalog[0]));
