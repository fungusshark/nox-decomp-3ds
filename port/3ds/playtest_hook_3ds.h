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
#define NOX_PT_VERSION          6u  /* state packet: v2 = v1 + names block; v3 = v2 + obj hp_pct/class_id/hp_flags; v4 = v3 + ext block; v5 = v4 + spell set / missile block; v6 = v5 + session / event ring / scoreboard / obelisks */
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
#define NOX_PT_F_ALLOWLISTED    0x0800u /* server name matches cfg mp_server_allow (or wildcard) */
#define NOX_PT_F_WILDCARD       0x1000u /* cfg mp_server_allow=* : any MP server allowed (dev-only) */

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
    NOX_PT_OP_STOP      = 9,  /* drop all overrides, release held inputs */
    NOX_PT_OP_SPELLSET  = 10  /* a = spell set 0..4: W/E (ZL/ZR) taps, shorter way round the wrap */
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
    uint8_t  hp_pct;        /* v3: 0..100 health bar the client draws, 0xFF = client has none */
    uint8_t  class_id;      /* v3: players only, playerInfo+2251 (0 Warrior, 1 Wizard, 2 Conjurer), else 0xFF */
    uint8_t  hp_flags;      /* v3: NOX_PT_HPF_* (v1/v2: reserved, always 0) */
} nox_pt_obj_t;

/* nox_pt_obj_t.hp_flags (v3). hp_pct comes only from values the client
 * receives to draw a health bar; see docs/PLAYTEST_HOOK.md section 3. */
#define NOX_PT_HPF_QUEST_TAG    0x01u /* player name-tag bar: playerInfo+2282 (opcode 0xC4/12, Quest only) */
#define NOX_PT_HPF_UNIT_BAR     0x02u /* owned-unit bar: table byte_5D4594[1200916] (opcodes 0xDB/0xDD/0x41) */
#define NOX_PT_HPF_POISONED     0x04u /* bar drawn in the poison colour */

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

/* v4 ext block, after the names block: u8 n_ext, u8 entry_size (=20), then
 * n_ext entries: the local player first (if valid), then one per exported
 * object in object order. Client-received display state only (see
 * docs/PLAYTEST_HOOK.md section 3, "Ext block"). */
typedef struct __attribute__((packed)) nox_pt_ext {
    uint16_t net_id;
    uint8_t  anim;          /* drawable +276 low byte (UNVERIFIED semantics) */
    uint8_t  ext_flags;     /* NOX_PT_XF_* */
    uint32_t enchant;       /* drawable +124 enchant mask, opcode 0x5A (GAME2.c:51124) */
    uint32_t weapon_flags;  /* players: playerInfo+4, opcodes 0x50/0x51 set, 0x54 clear (sub_417AA0/sub_417B80) */
    uint32_t armor_flags;   /* players: playerInfo+0, opcodes 0x4F/0x52 set, 0x53 clear */
    int16_t  vx, vy;        /* world px per 16 game frames (Q4 px/frame), hook-side delta; valid if XF_VEL */
} nox_pt_ext_t;

#define NOX_PT_XF_VEL           0x01u /* vx/vy valid (seen in the previous publish) */
#define NOX_PT_XF_OWNED         0x02u /* in this client's owned-unit table [1200916] (own summon/pet) */
#define NOX_PT_XF_MISSILE       0x04u /* class_flags & 1 (missile/projectile object) */
#define NOX_PT_XF_EQUIP         0x08u /* player: playerInfo entry found, weapon/armor flags valid */
#define NOX_PT_XF_FRIEND        0x10u /* v6: in the client friend list (MSG_OBJECT_FRIEND_ADD 0x34, byte_5D4594[1203864]) */

_Static_assert(sizeof(nox_pt_ext_t)       == 20, "nox_pt_ext_t wire size");

/* v5 block, after the ext block: u8 spell_set (0..4, 0xFF unknown), u8 v5_flags
 * (0, reserved), u8 n_mis, u8 entry_size (=12), then n_mis entries, one per
 * exported missile object (class_flags & 1) in object order. See
 * docs/PLAYTEST_HOOK.md section 3, "v5 block". */
#define NOX_PT_MAX_MIS          16u
typedef struct __attribute__((packed)) nox_pt_mis {
    uint16_t net_id;
    uint8_t  mis_flags;     /* PT_MISF_* (playtest_missile.h): incoming, near, tracked, approaching */
    uint8_t  reserved;      /* 0 */
    uint32_t subclass;      /* drawable +116, thing.bin SUBCLASS (missiles: bit0 MISSILE_COUNTERSPELL, bit1 MAGIC) */
    uint16_t miss_px;       /* closest approach to the local player, px; 0xFFFF unknown / moving away */
    uint16_t tca_f;         /* game frames to the closest approach; 0xFFFF unknown / moving away */
} nox_pt_mis_t;
_Static_assert(sizeof(nox_pt_mis_t)       == 12, "nox_pt_mis_t wire size");
_Static_assert(sizeof(nox_pt_cmd_t)       == 20, "nox_pt_cmd_t wire size");

/* v6 block, after the v5 block: one nox_pt_session_t, then n_ev x event
 * (pt_event_t, playtest_events.h, 20 B), n_score x nox_pt_score_t, n_obl x
 * nox_pt_obl_t. See docs/PLAYTEST_HOOK.md section 3, "v6 block". */
#define NOX_PT_MAX_EV_PKT       40u /* newest events per packet (sliding window, host dedups by seq) */
#define NOX_PT_MAX_SCORES       32u
#define NOX_PT_MAX_OBL          16u
#define NOX_PT_MAP_BYTES        24u
#define NOX_PT_SF_GAME_OVER     0x01u /* sub_40A5C0(8): winner banner up (set by 0x56..0x59, cleared by 0x2B) */
#define NOX_PT_SF_TIMER_ON      0x02u /* sub_40A220(): game timer running, timer_ms valid */
#define NOX_PT_SF_EV_GATED      0x04u /* MP gate closed: events about other objects are not recorded */
typedef struct __attribute__((packed)) nox_pt_session {
    char     map[NOX_PT_MAP_BYTES]; /* sub_409B40() = byte_5D4594[2649740], NUL padded */
    uint32_t map_seq;       /* hook counter: +1 per accepted MSG_USE_MAP (0x2B) */
    uint32_t game_flags;    /* sub_40A5B0() = byte_5D4594[3600]: mode bits (0x17F0 mask), 8 game over, 0x1000 quest ... */
    uint32_t timer_ms;      /* sub_40A230() remaining, 0xFFFFFFFF if no timer */
    uint16_t frag_limit;    /* sub_416590(0)+54 (0xAF byte +18): kill/lesson limit */
    uint8_t  time_limit_min;/* sub_416590(0)+56 (0xAF byte +19) */
    uint8_t  s_flags;       /* NOX_PT_SF_* */
    uint32_t ev_next_seq;   /* seq the next event will get */
    uint8_t  n_ev, ev_size; /* events (oldest first) */
    uint8_t  n_score, score_size;
    uint8_t  n_obl, obl_size;
    uint16_t reserved;
} nox_pt_session_t;
_Static_assert(sizeof(nox_pt_session_t)   == 52, "nox_pt_session_t wire size");

typedef struct __attribute__((packed)) nox_pt_score {
    uint16_t net_id;        /* playerInfo +2060 */
    uint8_t  class_id;      /* +2251 */
    uint8_t  pflags;        /* +3680 low byte (bit0 observer) */
    int32_t  score;         /* +2136 "lessons" (kills; -1 for suicide / team kill), MSG_REPORT_LESSON 0x4E */
    int32_t  deaths;        /* +2140, MSG_REPORT_LESSON 0x4E */
} nox_pt_score_t;
_Static_assert(sizeof(nox_pt_score_t)     == 12, "nox_pt_score_t wire size");

typedef struct __attribute__((packed)) nox_pt_obl {
    uint16_t net_id;
    uint8_t  charge;        /* 0..100, last MSG_OBELISK_CHARGE (0xB3) */
    uint8_t  reserved;
} nox_pt_obl_t;
_Static_assert(sizeof(nox_pt_obl_t)       == 4,  "nox_pt_obl_t wire size");

_Static_assert(sizeof(nox_pt_name_t)      == 18, "nox_pt_name_t wire size");

/* Call once per event pump, right after nox_gamepad_update(). */
void nox_pt_frame(void);
/* Close the hook socket; must run before soc:u socExit(). */
void nox_pt_shutdown(void);
/* GAME2.c opcode 0xB0 (client): server name just received from the server. */
void nox_pt_note_server_name(const char *name);
/* GAME2.c client dispatcher: every server message, before its handler runs
 * (p = message start, avail = bytes left in the frame). Read-only. */
void nox_pt_note_msg(const unsigned char *p, int avail);
/* net_sockets_3ds.c: socInit regardless of the current net mode. */
bool nox_3ds_soc_ensure_ready_any_mode(void);

#else /* !NOX_PLAYTEST_HOOK */

static inline void nox_pt_frame(void) {}
static inline void nox_pt_shutdown(void) {}
static inline void nox_pt_note_server_name(const char *name) { (void)name; }
static inline void nox_pt_note_msg(const unsigned char *p, int avail) { (void)p; (void)avail; }

#endif /* NOX_PLAYTEST_HOOK */

#ifdef __cplusplus
}
#endif

#endif /* NOX_PLAYTEST_HOOK_3DS_H */
