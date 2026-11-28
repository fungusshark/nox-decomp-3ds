#ifndef NOX_PLAYTEST_HOOK_3DS_H
#define NOX_PLAYTEST_HOOK_3DS_H

/*
 * Gated game-state hook for automated playtesting (GitHub issue #36).
 *
 * Compiled in ONLY when the CMake option NOX_PLAYTEST_HOOK=ON defines
 * NOX_PLAYTEST_HOOK. Otherwise every entry point below is an empty static
 * inline, so release builds carry zero code and zero per-frame cost.
 *
 * Runtime: inert unless sdmc:/3ds/nox/playtest.cfg exists. See
 * docs/PLAYTEST_HOOK.md for the cfg format and the UDP wire schema.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NOX_PLAYTEST_HOOK

/* Wire constants. Keep in sync with scripts/playtest_hook_client.py. */
#define NOX_PT_MAGIC_STATE      0x5450584Eu /* bytes "NXPT" (little-endian u32) */
#define NOX_PT_MAGIC_CMD        0x4350584Eu /* bytes "NXPC" */
#define NOX_PT_VERSION          2u  /* state packet: v2 = v1 + trailing names block */
#define NOX_PT_CMD_VERSION      1u  /* command packet */
#define NOX_PT_MAX_NAMES        (NOX_PT_MAX_OBJECTS + 1u) /* exported players + local */
#define NOX_PT_NAME_BYTES       15u /* UTF-8, NUL-padded, truncated on a char boundary */
#define NOX_PT_MAX_OBJECTS      32u
#define NOX_PT_NUM_ABILITIES    5u

/* State header flags (u16). */
#define NOX_PT_F_MENU_SHELL     0x0001u /* main menu shell active */
#define NOX_PT_F_MAP_LOADED     0x0002u /* sub_43C700() */
#define NOX_PT_F_IN_WORLD       0x0004u /* map loaded, no modal/menu/chat/NPC dialog */
#define NOX_PT_F_IS_MP          0x0008u /* network mode WIFI (multiplayer session/menus) */
#define NOX_PT_F_IS_HOST        0x0010u /* sub_40A5C0(1): local server running (also in menus/Solo; not an MP signal) */
#define NOX_PT_F_PLAYER_VALID   0x0020u /* local player drawable found in client list */
#define NOX_PT_F_OBSERVER       0x0040u /* playerInfo+3680 bit0 */
#define NOX_PT_F_HP_ZERO        0x0080u /* hp==0 while player valid (death heuristic, UNVERIFIED) */
#define NOX_PT_F_BOTS_OK        0x0100u /* server name contains "[bots-ok]" */
#define NOX_PT_F_CMDS_ACTIVE    0x0200u /* commands are being accepted this frame */
#define NOX_PT_F_OBJ_TRUNCATED  0x0400u /* more candidate objects than n_obj */
#define NOX_PT_F_ALLOWLISTED    0x0800u /* server name matches cfg mp_server_allow */

/* Command opcodes. */
enum {
    NOX_PT_OP_NOP       = 0,  /* keepalive, refreshes nothing */
    NOX_PT_OP_MOVE      = 1,  /* a = orientation 0..255, b = 0 stop / 1 walk / 3 run */
    NOX_PT_OP_AIM_WORLD = 2,  /* a = world x, b = world y */
    NOX_PT_OP_AIM_DIR   = 3,  /* a = orientation 0..255 (aim point 64 px from player) */
    NOX_PT_OP_ATTACK    = 4,  /* LMB tap at the current aim; a = hold frames (0 -> 2) */
    NOX_PT_OP_SLOT      = 5,  /* a = slot 1..5, b != 0 -> self-cast (LSHIFT) */
    NOX_PT_OP_HEAL      = 6,  /* quick health potion (X) */
    NOX_PT_OP_MANA      = 7,  /* quick mana potion (C) */
    NOX_PT_OP_JUMP      = 8,  /* MMB tap */
    NOX_PT_OP_STOP      = 9   /* drop all overrides, release held inputs */
};

typedef struct __attribute__((packed)) nox_pt_ability {
    uint8_t  id;            /* ability id (1 Charge, 2 War Cry, 3 Harpoon, 4 Tread, 5 Eye) */
    uint8_t  ready;         /* client copy of the server ready flag */
    uint8_t  active;        /* nonzero if the active mask is set */
    uint8_t  level;         /* record +16, opcode 0xCD; 0 = not learned (ready forced 0) */
    uint16_t cd_remaining;  /* frames, 0 when ready (derived; UNVERIFIED) */
    uint16_t cd_total;      /* frames, sub_4252D0(id) */
} nox_pt_ability_t;

typedef struct __attribute__((packed)) nox_pt_state_hdr {
    uint32_t magic;         /* NOX_PT_MAGIC_STATE */
    uint16_t version;       /* NOX_PT_VERSION */
    uint16_t header_size;   /* sizeof(nox_pt_state_hdr_t) */
    uint32_t seq;           /* packet sequence number */
    uint32_t game_frame;    /* byte_5D4594[2598000] client frame */
    uint32_t hook_tick;     /* nox_pt_frame() call counter */
    uint16_t flags;         /* NOX_PT_F_* */
    uint8_t  player_class;  /* playerInfo+2251, 0xFF unknown */
    uint8_t  n_obj;         /* objects following the header */
    uint32_t net_id;        /* byte_5D4594[2616328] */
    uint16_t hp, hp_max;    /* byte_5D4594[1093040/1093044]; 0xFFFF if !PLAYER_VALID */
    uint16_t mp, mp_max;    /* byte_5D4594[1093060/1093064]; 0xFFFF if !PLAYER_VALID */
    int32_t  x, y;          /* drawable +12/+16 world px */
    uint8_t  facing;        /* drawable +297 */
    uint8_t  anim;          /* drawable +276 low byte (UNVERIFIED semantics) */
    uint16_t obj_size;      /* sizeof(nox_pt_obj_t) */
    int32_t  cam_x, cam_y;  /* byte_5D4594[811084/811088] camera world origin */
    nox_pt_ability_t abilities[NOX_PT_NUM_ABILITIES];
} nox_pt_state_hdr_t;

typedef struct __attribute__((packed)) nox_pt_obj {
    uint16_t net_id;        /* drawable +128 */
    uint16_t type_id;       /* drawable +108 thing type */
    uint32_t class_flags;   /* drawable +112 (4 player, 2 monster, 1 missile ...) */
    int32_t  x, y;          /* drawable +12/+16 world px */
    uint8_t  facing;        /* drawable +297 */
    uint8_t  hp_pct;        /* 0xFF = unknown (TODO: ally HP% via sub_495150) */
    uint16_t reserved;
} nox_pt_obj_t;

typedef struct __attribute__((packed)) nox_pt_cmd {
    uint32_t magic;         /* NOX_PT_MAGIC_CMD */
    uint8_t  version;       /* NOX_PT_CMD_VERSION */
    uint8_t  op;            /* NOX_PT_OP_* */
    uint16_t ttl_ms;        /* latch lifetime for MOVE/AIM (0 -> 250, max 2000) */
    uint32_t seq;
    int32_t  a, b;
} nox_pt_cmd_t;

_Static_assert(sizeof(nox_pt_ability_t)   == 8,  "nox_pt_ability_t wire size");
_Static_assert(sizeof(nox_pt_state_hdr_t) == 96, "nox_pt_state_hdr_t wire size");
_Static_assert(sizeof(nox_pt_obj_t)       == 20, "nox_pt_obj_t wire size");
/* v2 names block, after the objects: u8 n_names, u8 entry_size (=18), then
 * n_names entries. Players only; local player first if known. */
typedef struct __attribute__((packed)) nox_pt_name {
    uint16_t net_id;
    uint8_t  len;                       /* bytes used in name[] */
    char     name[NOX_PT_NAME_BYTES];   /* UTF-8 from playerInfo+4704 (UTF-16) */
} nox_pt_name_t;

_Static_assert(sizeof(nox_pt_cmd_t)       == 20, "nox_pt_cmd_t wire size");
_Static_assert(sizeof(nox_pt_name_t)      == 18, "nox_pt_name_t wire size");

/* Call once per event pump, right after nox_gamepad_update(). */
void nox_pt_frame(void);
/* Close the hook socket; must run before soc:u socExit(). */
void nox_pt_shutdown(void);
/* GAME2.c opcode 0xB0 (client): server name just received from the server. */
void nox_pt_note_server_name(const char *name);
/* net_sockets_3ds.c: socInit regardless of the current net mode. */
bool nox_3ds_soc_ensure_ready_any_mode(void);

#else /* !NOX_PLAYTEST_HOOK */

static inline void nox_pt_frame(void) {}
static inline void nox_pt_shutdown(void) {}
static inline void nox_pt_note_server_name(const char *name) { (void)name; }

#endif /* NOX_PLAYTEST_HOOK */

#ifdef __cplusplus
}
#endif

#endif /* NOX_PLAYTEST_HOOK_3DS_H */
