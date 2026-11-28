#!/usr/bin/env python3
"""Host-side client for the Nox 3DS playtest hook (issue #36).

Stdlib only. Wire layout must match port/3ds/playtest_hook_3ds.h exactly.

  # print decoded state packets as JSON lines
  python3 scripts/playtest_hook_client.py listen --port 47800

  # send commands to the 3DS (cmd port = listen_port in playtest.cfg)
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 move --dx 1 --dy 0 --run
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 aim --x 1200 --y 800
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 slot 1
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 attack
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 spellset 2   # set 0..4

  python3 -I scripts/playtest_hook_client.py --selftest
"""
import argparse
import json
import math
import socket
import struct
import sys
import time

MAGIC_STATE = 0x5450584E  # b"NXPT"
MAGIC_CMD = 0x4350584E    # b"NXPC"
VERSION = 6          # state packet (v2 = v1 + names block; v3 = v2 + obj class_id/hp_flags; v4 = v3 + ext block;
                     # v5 = v4 + spell set / missile block; v6 = v5 + session / events / scoreboard / obelisks)
CMD_VERSION = 1      # command packet
MAX_OBJECTS = 32
NUM_ABILITIES = 5

# Struct formats ('<' = little-endian, no padding: matches __attribute__((packed))).
ABILITY_FMT = "<BBBBHH"
HDR_FMT_PREFIX = "<IHHIIIHBBIHHHHiiBBHii"
HDR_FMT = HDR_FMT_PREFIX + ABILITY_FMT[1:] * NUM_ABILITIES
OBJ_FMT = "<HHIiiBBBB"
CMD_FMT = "<IBBHIii"
NAME_FMT = "<HB15s"
NAME_BYTES = 15
EXT_FMT = "<HBBIIIhh"
MIS_FMT = "<HBBIHH"
MAX_MIS = 16
SESSION_FMT = "<24sIIIHBBIBBBBBBH"
EVENT_FMT = "<IIBBHHHHh"
SCORE_FMT = "<HBBii"
OBL_FMT = "<HBB"
MAX_EV_PKT = 40
RECV_BUF = 8192     # max v6 packet is about 3.5 KB

# Must equal the C _Static_asserts in playtest_hook_3ds.h.
ABILITY_SIZE = 8
HDR_SIZE = 96
OBJ_SIZE = 20
CMD_SIZE = 20
NAME_SIZE = 18
EXT_SIZE = 20
MIS_SIZE = 12
SESSION_SIZE = 52
EVENT_SIZE = 20
SCORE_SIZE = 12
OBL_SIZE = 4

assert struct.calcsize(ABILITY_FMT) == ABILITY_SIZE
assert struct.calcsize(HDR_FMT) == HDR_SIZE
assert struct.calcsize(OBJ_FMT) == OBJ_SIZE
assert struct.calcsize(CMD_FMT) == CMD_SIZE
assert struct.calcsize(NAME_FMT) == NAME_SIZE
assert struct.calcsize(EXT_FMT) == EXT_SIZE
assert struct.calcsize(MIS_FMT) == MIS_SIZE
assert struct.calcsize(SESSION_FMT) == SESSION_SIZE
assert struct.calcsize(EVENT_FMT) == EVENT_SIZE
assert struct.calcsize(SCORE_FMT) == SCORE_SIZE
assert struct.calcsize(OBL_FMT) == OBL_SIZE

FLAGS = {
    0x0001: "menu_shell",
    0x0002: "map_loaded",
    0x0004: "in_world",
    0x0008: "is_mp",
    0x0010: "is_host",
    0x0020: "player_valid",
    0x0040: "observer",
    0x0080: "hp_zero",
    0x0100: "bots_ok",
    0x0200: "cmds_active",
    0x0400: "obj_truncated",
    0x0800: "allowlisted",
    0x1000: "wildcard",
}

OPS = {
    "nop": 0, "move": 1, "aim": 2, "aimdir": 3, "attack": 4,
    "slot": 5, "heal": 6, "mana": 7, "jump": 8, "stop": 9,
    "spellset": 10,   # a = spell set 0..4 (W/E taps like ZL/ZR)
}

# playerInfo+2251. Warrior = 0 verified in Azahar; 1/2 from the decomp (class 2
# gates the creature book, GAME2.c:11437) and the OpenNox enum (UNVERIFIED live).
CLASSES = {0: "warrior", 1: "wizard", 2: "conjurer"}
# v3 object hp_flags: where hp_pct came from (both are bars the client draws).
HP_FLAGS = {0x01: "quest_tag", 0x02: "unit_bar", 0x04: "poisoned"}
# v4 ext_flags.
EXT_FLAGS = {0x01: "vel", 0x02: "owned", 0x04: "missile", 0x08: "equip", 0x10: "friend"}
# v4 enchant mask bits (drawable +124, opcode 0x5A). Bit 0 invisible and 21
# infravision are used by the renderer (GAME2.c:30355, 32368); the other names
# follow the OpenNox ENCHANT_* enum order (UNVERIFIED live per bit).
ENCHANTS = {
    0: "invisible", 1: "moonglow", 2: "blinded", 3: "confused", 4: "slowed",
    5: "held", 6: "detecting", 7: "ethereal", 8: "run", 9: "hasted",
    10: "villain", 11: "afraid", 12: "burning", 13: "vampirism", 14: "anchored",
    15: "light", 16: "death", 17: "protect_from_fire", 18: "protect_from_poison",
    19: "protect_from_magic", 20: "protect_from_electricity", 21: "infravision",
    22: "shock", 23: "invulnerable", 24: "telekinesis", 25: "freeze",
    26: "shield", 27: "reflective_shield", 28: "charming", 29: "antimagic",
    30: "crown", 31: "sneak",
}
# v5 missile flags (port/3ds/playtest_missile.h PT_MISF_*).
MIS_FLAGS = {0x01: "incoming", 0x02: "near", 0x04: "tracked", 0x08: "approaching"}
# v5 missile subclass bits (drawable +116 = thing.bin SUBCLASS; the server's
# Counterspell / Inversion use bit 0 / bit 1, GAME4.c:45711). Arrows and bolts
# are MISSILE+WEAPON things whose subclass uses the weapon table instead.
MIS_SUBCLASS = {0x1: "counterspell", 0x2: "magic"}
# v6 (port/3ds/playtest_events.h PT_EV_*).
EV_TYPES = {1: "dmg", 2: "kill", 3: "died", 4: "respawn", 5: "chant", 6: "cast", 7: "dspell",
            8: "summon", 9: "summon_cancel", 10: "obelisk", 11: "map", 12: "game_over"}
SESSION_FLAGS = {0x01: "game_over", 0x02: "timer_on", 0x04: "events_gated"}
GAME_OVER_KIND = {1: "flagball", 2: "flag", 3: "deathmatch", 4: "team_deathmatch"}
# Spell ids: the client spell name table (byte_587000[65448], SPELL_* strings).
SPELLS = dict(enumerate((
    "invalid anchor arachnaphobia blind blink burn cancel chain_lightning_bolt channel_life charm "
    "cleansing_flame cleansing_mana_flame confuse counterspell cure_poison death death_ray detect_magic "
    "detonate detonate_glyphs disenchant_all turn_undead drain_mana earthquake lightning explosion fear "
    "fireball firewalk fist force_field force_of_nature freeze fumble glyph greater_heal haste infravision "
    "inversion invisibility invulnerability lesser_heal light chain_lightning lock mark mark_1 mark_2 mark_3 "
    "mark_4 magic_missile shield meteor meteor_shower moonglow nullify mana_bomb phantom pixie_swarm plasma "
    "poison protection_from_electricity protection_from_fire protection_from_magic protection_from_poison "
    "pull push oval_shield restore_health restore_mana run shock slow small_zap stun summon_bat "
    "summon_black_bear summon_bear summon_beholder summon_bomber summon_carnivorous_plant "
    "summon_albino_spider summon_small_albino_spider summon_evil_cherub summon_ember_demon summon_ghost "
    "summon_giant_leech summon_imp summon_mechanical_flyer summon_mechanical_golem summon_mimic summon_ogre "
    "summon_ogre_brute summon_ogre_warlord summon_scorpion summon_shade summon_skeleton summon_skeleton_lord "
    "summon_spider summon_small_spider summon_spitting_spider summon_stone_golem summon_troll summon_urchin "
    "summon_wasp summon_willowisp summon_wolf summon_black_wolf summon_white_wolf summon_zombie "
    "summon_vile_zombie summon_demon summon_lich summon_dryad summon_urchin_shaman swap tag "
    "teleport_other_to_mark_1 teleport_other_to_mark_2 teleport_other_to_mark_3 teleport_other_to_mark_4 "
    "teleport_pop teleport_to_mark_1 teleport_to_mark_2 teleport_to_mark_3 teleport_to_mark_4 "
    "teleport_to_target telekinesis toxic_cloud trigger_glyph vampirism villain wall wink summon_creature "
    "mark_location teleport_to_marker").split()))
assert len(SPELLS) == 137 and SPELLS[27] == "fireball" and SPELLS[128] == "toxic_cloud"

ABILITY_NAMES = {1: "berserker_charge", 2: "war_cry", 3: "harpoon",
                 4: "tread_lightly", 5: "eye_of_the_wolf"}


def orient_from_vec(dx, dy):
    """Same convention as src/gamepad.c update_left_analog_absolute()."""
    theta = math.atan2(-dy, -dx) / math.pi
    return int((theta + 1.0) * 128.0 + 0.5) & 255


def vec_from_orient(o):
    ang = ((o / 128.0) - 1.0) * math.pi
    return -math.cos(ang), -math.sin(ang)


def derive_mode(flags):
    if flags & 0x0001 or not flags & 0x0002:
        return "menu"
    if flags & 0x0040:
        return "observer"
    if flags & 0x0080:
        return "dead?"
    if flags & 0x0004:
        return "in_world"
    return "in_world_modal"


def decode_event(seq, frame, typ, b, a, c, d, e, f):
    """One v6 event as a dict (field meaning per type, see docs/PLAYTEST_HOOK.md)."""
    t = EV_TYPES.get(typ, "type%d" % typ)
    ev = {"seq": seq, "frame": frame, "type": t}
    if t == "dmg":
        ev.update(target_net_id=a, amount=-f, delta=f)
    elif t == "kill":
        ev.update(victim_net_id=a, killer_net_id=c or None, assist_net_id=d or None, kind=b, code=e)
    elif t in ("died", "respawn"):
        ev.update(net_id=a)
        if t == "respawn":
            ev.update(flags=b, arg=c)
    elif t == "chant":
        ev.update(phoneme=b, female=bool(c), volume=d, pan=f)
    elif t == "cast":
        ev.update(caster_net_id=None, spell_id=c, spell=SPELLS.get(c), female=bool(b), volume=d, pan=f,
                  phonemes=e)
    elif t == "dspell":
        ev.update(caster_net_id=a, target_net_id=d, spell_id=c or None, spell=SPELLS.get(c) if c else "harpoon",
                  sub=b, level=e, start=bool(f))
    elif t == "summon":
        ev.update(summon_id=a, x=c, y=d, thing_type=e, dir=b, frames=f)
    elif t == "summon_cancel":
        ev.update(summon_id=a)
    elif t == "obelisk":
        ev.update(net_id=a, charge=b)
    elif t == "game_over":
        ev.update(kind=GAME_OVER_KIND.get(b, b), winner_id=a, arg=c)
    else:
        ev.update(a=a, b=b, c=c, d=d, e=e, f=f)
    return ev


def decode_v6(data, off):
    """v6 block at off -> (dict, end)."""
    if len(data) < off + SESSION_SIZE:
        raise ValueError("v6 packet missing session block")
    (mraw, map_seq, gflags, timer_ms, frag_limit, tlim, sflags, ev_next,
     n_ev, ev_size, n_sc, sc_size, n_ob, ob_size, _r) = struct.unpack_from(SESSION_FMT, data, off)
    if ev_size < EVENT_SIZE or sc_size < SCORE_SIZE or ob_size < OBL_SIZE:
        raise ValueError("bad v6 entry sizes %d/%d/%d" % (ev_size, sc_size, ob_size))
    p = off + SESSION_SIZE
    end = p + n_ev * ev_size + n_sc * sc_size + n_ob * ob_size
    if len(data) < end:
        raise ValueError("short v6 block len=%d need=%d" % (len(data), end))
    events = []
    for i in range(n_ev):
        events.append(decode_event(*struct.unpack_from(EVENT_FMT, data, p + i * ev_size)))
    p += n_ev * ev_size
    scores = []
    for i in range(n_sc):
        nid, cls, pf, score, deaths = struct.unpack_from(SCORE_FMT, data, p + i * sc_size)
        scores.append({"net_id": nid, "class_id": None if cls == 0xFF else cls, "class": CLASSES.get(cls),
                       "observer": bool(pf & 1), "score": score, "deaths": deaths})
    p += n_sc * sc_size
    obl = {}
    for i in range(n_ob):
        nid, charge, _ = struct.unpack_from(OBL_FMT, data, p + i * ob_size)
        obl[nid] = charge
    return {
        "map": mraw.split(b"\0", 1)[0].decode("latin-1"),
        "map_seq": map_seq, "game_flags": gflags,
        "timer_ms": None if timer_ms == 0xFFFFFFFF else timer_ms,
        "frag_limit": frag_limit, "time_limit_min": tlim,
        "flags": [n for b, n in SESSION_FLAGS.items() if sflags & b],
        "game_over": bool(sflags & 1), "events_gated": bool(sflags & 4),
        "ev_next_seq": ev_next, "events": events, "scores": scores, "obelisks": obl,
    }, end


class EventDedup:
    """Host-side v6 event stream: feed decoded states, get each event once, in
    order. `lost` counts seq gaps (events that fell out of the 40-event window
    before a packet carried them)."""

    def __init__(self):
        self.last = 0
        self.lost = 0

    def feed(self, st):
        sess = st.get("session")
        if not sess:
            return []
        out = []
        for ev in sess["events"]:
            if ev["seq"] <= self.last:
                continue
            if self.last and ev["seq"] > self.last + 1:
                self.lost += ev["seq"] - self.last - 1
            self.last = ev["seq"]
            out.append(ev)
        return out


def decode_state(data):
    if len(data) < HDR_SIZE:
        raise ValueError("short packet: %d" % len(data))
    f = struct.unpack_from(HDR_FMT, data, 0)
    (magic, version, header_size, seq, game_frame, hook_tick, flags, pclass,
     n_obj, net_id, hp, hp_max, mp, mp_max, x, y, facing, anim, obj_size,
     cam_x, cam_y) = f[:21]
    if magic != MAGIC_STATE:
        raise ValueError("bad magic 0x%08X" % magic)
    if version not in (1, 2, 3, 4, 5, 6) or header_size != HDR_SIZE or obj_size != OBJ_SIZE:
        raise ValueError("version/size mismatch v=%d hdr=%d obj=%d" % (version, header_size, obj_size))
    base_len = HDR_SIZE + n_obj * OBJ_SIZE
    names = {}
    exts = []
    mis = {}
    spell_set = None
    session = None
    if version == 1:
        if len(data) != base_len:
            raise ValueError("length %d != %d" % (len(data), base_len))
    else:
        if len(data) < base_len + 2:
            raise ValueError("v%d packet missing names block" % version)
        n_names, entry_size = data[base_len], data[base_len + 1]
        names_end = base_len + 2 + n_names * NAME_SIZE
        if entry_size != NAME_SIZE or len(data) < names_end:
            raise ValueError("bad names block n=%d size=%d len=%d" % (n_names, entry_size, len(data)))
        for i in range(n_names):
            nid, nlen, raw = struct.unpack_from(NAME_FMT, data, base_len + 2 + i * NAME_SIZE)
            names[nid] = raw[:min(nlen, NAME_BYTES)].decode("utf-8", "replace")
        if version < 4:
            if len(data) != names_end:
                raise ValueError("length %d != %d" % (len(data), names_end))
        else:
            if len(data) < names_end + 2:
                raise ValueError("v4 packet missing ext block")
            n_ext, ext_size = data[names_end], data[names_end + 1]
            ext_end = names_end + 2 + n_ext * ext_size
            if ext_size < EXT_SIZE or len(data) < ext_end or (version == 4 and len(data) != ext_end):
                raise ValueError("bad ext block n=%d size=%d len=%d" % (n_ext, ext_size, len(data)))
            if version >= 5:
                if len(data) < ext_end + 4:
                    raise ValueError("v5 packet missing v5 block")
                ss, _v5f, n_mis, mis_size = data[ext_end:ext_end + 4]
                mis_end = ext_end + 4 + n_mis * mis_size
                # v6+ may append after the v5 block; v5 itself must end here.
                if mis_size < MIS_SIZE or len(data) < mis_end or (version == 5 and len(data) != mis_end):
                    raise ValueError("bad v5 block n=%d size=%d len=%d" % (n_mis, mis_size, len(data)))
                if version >= 6:
                    session, v6_end = decode_v6(data, mis_end)
                    if version == 6 and len(data) != v6_end:
                        raise ValueError("length %d != %d" % (len(data), v6_end))
                spell_set = None if ss == 0xFF else ss
                for i in range(n_mis):
                    m_net, m_f, _r, m_sub, m_miss, m_tca = struct.unpack_from(
                        MIS_FMT, data, ext_end + 4 + i * mis_size)
                    mis[m_net] = {
                        "flags": [n for b, n in MIS_FLAGS.items() if m_f & b],
                        "incoming": bool(m_f & 0x01), "near": bool(m_f & 0x02),
                        "tracked": bool(m_f & 0x04), "approaching": bool(m_f & 0x08),
                        "subclass": m_sub,
                        "subclass_names": [n for b, n in MIS_SUBCLASS.items() if m_sub & b],
                        "miss_px": None if m_miss == 0xFFFF else m_miss,
                        "tca_f": None if m_tca == 0xFFFF else m_tca,
                    }
            for i in range(n_ext):
                (e_net, e_anim, e_xf, e_ench, e_wpn, e_arm, e_vx, e_vy) = struct.unpack_from(
                    EXT_FMT, data, names_end + 2 + i * ext_size)
                exts.append({
                    "net_id": e_net, "anim": e_anim,
                    "ext_flags": [n for b, n in EXT_FLAGS.items() if e_xf & b],
                    "enchant_mask": e_ench,
                    "enchants": [n for b, n in ENCHANTS.items() if e_ench & (1 << b)],
                    "weapon_flags": e_wpn if e_xf & 0x08 else None,
                    "armor_flags": e_arm if e_xf & 0x08 else None,
                    "is_missile": bool(e_xf & 0x04), "owned_by_me": bool(e_xf & 0x02),
                    "vel": (e_vx / 16.0, e_vy / 16.0) if e_xf & 0x01 else None,
                })
    abilities = []
    ab = f[21:]
    for i in range(NUM_ABILITIES):
        aid, ready, active, level, cd_rem, cd_total = ab[i * 6:(i + 1) * 6]
        abilities.append({
            "slot": i + 1, "id": aid, "name": ABILITY_NAMES.get(aid),
            "level": level, "learned": level > 0,
            "ready": bool(ready), "active": bool(active),
            "cd_remaining": cd_rem, "cd_total": cd_total,
        })
    objects = []
    for i in range(n_obj):
        o_net, o_type, o_cls, o_x, o_y, o_face, o_hp, o_class, o_hpf = struct.unpack_from(
            OBJ_FMT, data, HDR_SIZE + i * OBJ_SIZE)
        if version < 3:  # v1/v2: these bytes were reserved (0), not class 0
            o_class, o_hpf = 0xFF, None
        objects.append({
            "net_id": o_net, "type_id": o_type, "class_flags": o_cls,
            "is_player": bool(o_cls & 4), "is_monster": bool(o_cls & 2),
            "x": o_x, "y": o_y, "facing": o_face,
            "hp_pct": None if o_hp == 0xFF else o_hp,
            "hp_src": None if o_hpf is None else
                      [n for b, n in HP_FLAGS.items() if o_hpf & b],
            "class_id": None if o_class == 0xFF else o_class,
            "class": CLASSES.get(o_class),
            "name": names.get(o_net) if o_cls & 4 else None,
        })
    valid = bool(flags & 0x0020)
    # v4: attach ext entries (local player first if valid, then objects in order).
    self_ext = None
    if exts:
        k = 0
        if valid and exts[0]["net_id"] == (net_id & 0xFFFF):
            self_ext, k = exts[0], 1
        for o, e in zip(objects, exts[k:]):
            if e["net_id"] == o["net_id"]:
                o["ext"] = e
    # v5: missile info by net id (exported missiles only).
    for o in objects:
        if o["class_flags"] & 1 and o["net_id"] in mis:
            o["missile"] = mis[o["net_id"]]

    def opt(v, bad):
        return v if valid and v != bad else None

    return {
        "seq": seq, "game_frame": game_frame, "hook_tick": hook_tick,
        "flags": flags, "flag_names": [n for b, n in FLAGS.items() if flags & b],
        "mode": derive_mode(flags),
        "player": {
            "net_id": net_id,
            "name": names.get(net_id & 0xFFFF),
            "class_id": None if pclass == 0xFF else pclass,
            "class": CLASSES.get(pclass),
            "valid": valid,
            "hp": opt(hp, 0xFFFF), "hp_max": opt(hp_max, 0xFFFF),
            "mp": opt(mp, 0xFFFF), "mp_max": opt(mp_max, 0xFFFF),
            "x": x if valid else None, "y": y if valid else None,
            "facing": opt(facing, 0xFF), "anim": opt(anim, 0xFF),
            "abilities": abilities,
            "ext": self_ext,
            "spell_set": spell_set,   # v5: active spell set 0..4 (None: unknown / older build)
        },
        "camera": {"x": cam_x, "y": cam_y},
        "objects": objects,
        "version": version,
        "spell_set": spell_set,
        "session": session,   # v6: map/timer/limits/game over, events, scoreboard, obelisks (None before v6)
    }


def encode_state(seq=1, game_frame=0, hook_tick=0, flags=0, pclass=0xFF, net_id=0,
                 hp=0, hp_max=0, mp=0, mp_max=0, x=0, y=0, facing=0, anim=0,
                 cam_x=0, cam_y=0, abilities=(), objects=(), names=(), exts=(), version=VERSION,
                 spell_set=0xFF, missiles=(), session=None):
    """Encoder mirroring publish_state() (used by --selftest and fakes)."""
    ab = list(abilities) + [(0, 0, 0, 0, 0, 0)] * (NUM_ABILITIES - len(abilities))
    ab_flat = []
    for aid, ready, active, level, cd_rem, cd_total in ab[:NUM_ABILITIES]:
        ab_flat += [aid, ready, active, level, cd_rem, cd_total]
    objs = list(objects)[:MAX_OBJECTS]
    hdr = struct.pack(HDR_FMT, MAGIC_STATE, version, HDR_SIZE, seq, game_frame, hook_tick,
                      flags, pclass, len(objs), net_id, hp, hp_max, mp, mp_max, x, y,
                      facing, anim, OBJ_SIZE, cam_x, cam_y, *ab_flat)
    def obj_tail(o):  # (class_id, hp_flags); v1/v2 wrote 0, 0
        if version < 3:
            return (0, 0)
        return (o[7] if len(o) > 7 else 0xFF, o[8] if len(o) > 8 else 0)
    body = b"".join(struct.pack(OBJ_FMT, o[0], o[1], o[2], o[3], o[4], o[5], o[6], *obj_tail(o))
                    for o in objs)
    if version == 1:
        return hdr + body
    entries = []
    for nid, name in names:
        raw = name.encode("utf-8")[:NAME_BYTES]
        while raw:  # truncate on a UTF-8 char boundary, like the C side
            try:
                raw.decode("utf-8")
                break
            except UnicodeDecodeError:
                raw = raw[:-1]
        entries.append(struct.pack(NAME_FMT, nid, len(raw), raw))
    out = hdr + body + bytes([len(entries), NAME_SIZE]) + b"".join(entries)
    if version < 4:
        return out
    # exts: (net_id, anim, ext_flags, enchant, weapon_flags, armor_flags, vx_q4, vy_q4)
    out += bytes([len(exts), EXT_SIZE]) + b"".join(struct.pack(EXT_FMT, *e) for e in exts)
    if version < 5:
        return out
    # missiles: (net_id, mis_flags, subclass, miss_px, tca_f)
    mis = list(missiles)[:MAX_MIS]
    out += bytes([spell_set, 0, len(mis), MIS_SIZE]) + b"".join(
        struct.pack(MIS_FMT, m[0], m[1], 0, m[2], m[3], m[4]) for m in mis)
    if version < 6:
        return out
    # session: dict(map, map_seq, game_flags, timer_ms, frag_limit, time_limit_min, s_flags, ev_next_seq,
    #   events=[(seq, frame, type, b, a, c, d, e, f)], scores=[(net, class, pflags, score, deaths)],
    #   obelisks=[(net, charge)])
    ss = dict(session or {})
    evs = list(ss.get("events", ()))[:MAX_EV_PKT]
    scs = list(ss.get("scores", ()))
    obs = list(ss.get("obelisks", ()))
    out += struct.pack(SESSION_FMT, ss.get("map", "").encode("latin-1")[:23], ss.get("map_seq", 0),
                       ss.get("game_flags", 0), ss.get("timer_ms", 0xFFFFFFFF), ss.get("frag_limit", 0),
                       ss.get("time_limit_min", 0), ss.get("s_flags", 0),
                       ss.get("ev_next_seq", (evs[-1][0] + 1) if evs else 1),
                       len(evs), EVENT_SIZE, len(scs), SCORE_SIZE, len(obs), OBL_SIZE, 0)
    out += b"".join(struct.pack(EVENT_FMT, *e) for e in evs)
    out += b"".join(struct.pack(SCORE_FMT, *c) for c in scs)
    return out + b"".join(struct.pack(OBL_FMT, o[0], o[1], 0) for o in obs)


def encode_cmd(op, a=0, b=0, ttl_ms=0, seq=0):
    if isinstance(op, str):
        op = OPS[op]
    return struct.pack(CMD_FMT, MAGIC_CMD, CMD_VERSION, op, ttl_ms, seq, a, b)


def decode_cmd(data):
    if len(data) != CMD_SIZE:
        raise ValueError("cmd size %d" % len(data))
    magic, version, op, ttl_ms, seq, a, b = struct.unpack(CMD_FMT, data)
    if magic != MAGIC_CMD or version != CMD_VERSION:
        raise ValueError("bad cmd header")
    return {"op": op, "ttl_ms": ttl_ms, "seq": seq, "a": a, "b": b}


class HookClient:
    """Minimal programmatic API: recv_state() and send(op, a, b)."""

    def __init__(self, listen_port=47800, target=None, bind="0.0.0.0"):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, listen_port))
        self.target = target  # (ip, port); learned from first state packet if None
        self.cmd_port = target[1] if target else 47801
        self.seq = 0

    def recv_state(self, timeout=None):
        self.sock.settimeout(timeout)
        data, addr = self.sock.recvfrom(RECV_BUF)
        if self.target is None:
            self.target = (addr[0], self.cmd_port)
        return decode_state(data)

    def send(self, op, a=0, b=0, ttl_ms=0):
        if self.target is None:
            raise RuntimeError("no target yet; pass --to or receive a state packet first")
        self.seq += 1
        self.sock.sendto(encode_cmd(op, a, b, ttl_ms, self.seq), self.target)


def parse_hostport(s):
    host, _, port = s.rpartition(":")
    return host, int(port)


def build_cmd(args):
    op = args.op
    v = args.args
    a = b = 0
    if op == "move":
        if args.orient is not None:
            a = args.orient
        else:
            a = orient_from_vec(args.dx, args.dy)
        b = 0 if args.stop else (3 if args.run else 1)
    elif op == "aim":
        a, b = args.x, args.y
    elif op == "aimdir":
        a = args.orient if args.orient is not None else orient_from_vec(args.dx, args.dy)
    elif op == "attack":
        a = int(v[0]) if v else 0
    elif op == "slot":
        if not v:
            raise SystemExit("slot needs a number 1..5")
        a = int(v[0])
        b = 1 if args.self_cast else 0
    elif op == "spellset":
        if not v or not 0 <= int(v[0]) <= 4:
            raise SystemExit("spellset needs a set 0..4")
        a = int(v[0])
    return encode_cmd(op, a, b, args.ttl, 0)


def gate_selftest():
    """Compile and run the host unit test for port/3ds/playtest_gate.h if a C compiler exists."""
    import os
    import shutil
    import subprocess
    import tempfile
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "scripts", "test_playtest_gate.c")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or not os.path.exists(src):
        print("gate test SKIPPED (no C compiler)")
        return
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_playtest_gate")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I", os.path.join(root, "port", "3ds"), src, "-o", exe], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.strip()
        print(out)


def selftest():
    abilities = [(1, 1, 0, 1, 0, 300), (2, 0, 1, 2, 120, 300), (3, 0, 0, 1, 75, 150),
                 (4, 1, 0, 3, 0, 30), (5, 0, 0, 0, 0, 600)]
    objects = [(17, 1234, 4, 100, -20, 3, 0xFF, 1, 0),
               (900, 77, 0x2000000, 5000, 6000, 0, 50, 0xFF, 0x02),
               (18, 1234, 4, 0, 0, 7, 37, 2, 0x05)]
    pkt = encode_state(seq=7, game_frame=123456, hook_tick=99, flags=0x0226, pclass=0,
                       net_id=42, hp=150, hp_max=150, mp=0, mp_max=0, x=2048, y=-1,
                       facing=6, anim=1, cam_x=1900, cam_y=-120, abilities=abilities,
                       objects=objects, names=[(42, "Jack 3"), (17, "Jäck-Wizard-Long-Name")],
                       exts=[(42, 1, 0x09, (1 << 26) | (1 << 9), 0x2, 0x41, 32, -8),
                             (17, 3, 0x08, 1 << 27, 0x80, 0, 0, 0),
                             (900, 0, 0x03, 0, 0, 0, -16, 0),
                             (18, 7, 0x00, 0, 0, 0, 0, 0)], version=4)
    assert len(pkt) == HDR_SIZE + 3 * OBJ_SIZE + 2 + 2 * NAME_SIZE + 2 + 4 * EXT_SIZE, len(pkt)
    assert pkt[6:8] == struct.pack("<H", HDR_SIZE) and pkt[4] == 4
    st = decode_state(pkt)
    assert st["seq"] == 7 and st["game_frame"] == 123456 and st["hook_tick"] == 99
    assert st["mode"] == "in_world" and "cmds_active" in st["flag_names"]
    p = st["player"]
    assert (p["net_id"], p["hp"], p["hp_max"], p["x"], p["y"], p["facing"], p["anim"]) == \
        (42, 150, 150, 2048, -1, 6, 1)
    assert p["class"] == "warrior"
    assert [(x["id"], x["ready"], x["cd_remaining"], x["cd_total"]) for x in p["abilities"]] == \
        [(1, True, 0, 300), (2, False, 120, 300), (3, False, 75, 150), (4, True, 0, 30),
         (5, False, 0, 600)]
    assert [x["learned"] for x in p["abilities"]] == [True, True, True, True, False]
    assert p["abilities"][1]["level"] == 2 and pkt[56 + 8 + 3] == 2  # level byte offset
    assert st["camera"] == {"x": 1900, "y": -120}
    o = st["objects"]
    assert (o[0]["net_id"], o[0]["type_id"], o[0]["is_player"], o[0]["x"], o[0]["y"],
            o[0]["hp_pct"]) == (17, 1234, True, 100, -20, None)
    assert (o[1]["class_flags"], o[1]["hp_pct"]) == (0x2000000, 50)
    # v3 object tail: class_id at +18, hp_flags at +19 (hp_pct stays at +17).
    assert (o[0]["class"], o[0]["class_id"], o[0]["hp_src"]) == ("wizard", 1, [])
    assert (o[1]["class_id"], o[1]["hp_src"]) == (None, ["unit_bar"])
    assert (o[2]["class"], o[2]["hp_pct"], o[2]["hp_src"]) == ("conjurer", 37,
                                                               ["quest_tag", "poisoned"])
    o2 = HDR_SIZE + 2 * OBJ_SIZE
    assert (pkt[o2 + 17], pkt[o2 + 18], pkt[o2 + 19]) == (37, 2, 0x05)
    # v4 ext block: local player first, then objects in order.
    pe = p["ext"]
    assert pe["enchants"] == ["hasted", "shield"] and pe["vel"] == (2.0, -0.5)
    assert (pe["weapon_flags"], pe["armor_flags"], pe["anim"]) == (0x2, 0x41, 1)
    assert o[0]["ext"]["enchants"] == ["reflective_shield"] and o[0]["ext"]["vel"] is None
    assert o[1]["ext"]["owned_by_me"] and o[1]["ext"]["vel"] == (-1.0, 0.0)
    assert o[1]["ext"]["weapon_flags"] is None and o[2]["ext"]["ext_flags"] == []
    x0 = HDR_SIZE + 3 * OBJ_SIZE + 2 + 2 * NAME_SIZE
    assert pkt[x0:x0 + 2] == bytes([4, EXT_SIZE])
    assert struct.unpack_from("<I", pkt, x0 + 2 + 4)[0] == (1 << 26) | (1 << 9)  # enchant offset
    assert st["spell_set"] is None and p["spell_set"] is None and "missile" not in o[0]
    # v4 packet with trailing bytes is rejected (v4 ends at the ext block).
    try:
        decode_state(pkt + b"\0\0\0\0")
    except ValueError:
        pass
    else:
        raise AssertionError("v4 trailing bytes accepted")
    # v5: spell set + missile block (missile objects only, by net id).
    mobjs = objects + [(700, 704, 0x180001, 2148, -1, 0, 0xFF, 0xFF, 0),
                       (701, 531, 0x5200001, 1900, 0, 0, 0xFF, 0xFF, 0)]
    mexts = [(42, 1, 0x09, 0, 0x2, 0x41, 0, 0), (17, 3, 0x08, 0, 0x80, 0, 0, 0),
             (900, 0, 0x03, 0, 0, 0, 0, 0), (18, 7, 0, 0, 0, 0, 0, 0),
             (700, 0, 0x05, 0, 0, 0, -160, 0), (701, 0, 0x04, 0, 0, 0, 0, 0)]
    p5 = encode_state(seq=8, flags=0x0226, net_id=42, x=2048, y=-1, objects=mobjs,
                      names=[(42, "Jack 3")], exts=mexts, spell_set=3,
                      missiles=[(700, 0x0F, 0x3, 0, 10), (701, 0x02, 0x10, 0xFFFF, 0xFFFF)], version=5)
    assert p5[4] == 5 and len(p5) == (HDR_SIZE + 5 * OBJ_SIZE + 2 + NAME_SIZE + 2 + 6 * EXT_SIZE
                                      + 4 + 2 * MIS_SIZE), len(p5)
    s5 = decode_state(p5)
    assert s5["version"] == 5 and s5["spell_set"] == 3 and s5["player"]["spell_set"] == 3
    m0 = s5["objects"][3]["missile"]
    assert m0["incoming"] and m0["tracked"] and m0["near"] and m0["approaching"]
    assert m0["subclass_names"] == ["counterspell", "magic"] and m0["miss_px"] == 0 and m0["tca_f"] == 10
    assert s5["objects"][3]["ext"]["vel"] == (-10.0, 0.0)
    m1 = s5["objects"][4]["missile"]
    assert m1["flags"] == ["near"] and m1["miss_px"] is None and m1["tca_f"] is None
    assert "missile" not in s5["objects"][0]
    x5 = len(p5) - 4 - 2 * MIS_SIZE
    assert p5[x5:x5 + 4] == bytes([3, 0, 2, MIS_SIZE])
    assert struct.unpack_from("<I", p5, x5 + 4 + 4)[0] == 0x3        # subclass offset
    assert decode_state(encode_state(flags=0x0006, version=5))["spell_set"] is None   # 0xFF = unknown
    for bad in (p5[:-1], p5 + b"\0"):
        try:
            decode_state(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("bad v5 packet accepted")
    # v6: session block, event window, scoreboard, obelisks.
    sess = {"map": "Estate", "map_seq": 3, "game_flags": 0x2000 | 0x400, "timer_ms": 61000,
            "frag_limit": 15, "time_limit_min": 10, "s_flags": 0x03,
            "events": [(5, 900, 1, 0, 17, 0, 0, 0, -12),           # dmg 12 on 17
                       (6, 901, 2, 2, 17, 42, 0, 2, 0),            # 42 killed 17
                       (7, 902, 6, 1, 0, 27, 90, 3, -40),          # chant decoded: fireball
                       (8, 903, 7, 11, 18, 24, 42, 2, 1),          # lightning 18 -> 42
                       (9, 904, 10, 60, 0x55, 0, 0, 0, 0),         # obelisk 0x55 at 60
                       (10, 905, 12, 3, 42, 1, 0, 0, 0)],          # DM winner 42
            "scores": [(42, 1, 0, 3, 1), (17, 2, 1, -1, 4)], "obelisks": [(0x55, 60)]}
    p6 = encode_state(seq=9, flags=0x0226, net_id=42, objects=mobjs[:1], names=[(42, "Jack 3")],
                      exts=[(42, 1, 0x19, 0, 0, 0, 0, 0), (17, 0, 0x10, 0, 0, 0, 0, 0)],
                      spell_set=0, session=sess)
    assert p6[4] == 6
    s6 = decode_state(p6)
    se = s6["session"]
    assert se["map"] == "Estate" and se["map_seq"] == 3 and se["timer_ms"] == 61000
    assert se["frag_limit"] == 15 and se["time_limit_min"] == 10 and se["game_over"]
    assert se["flags"] == ["game_over", "timer_on"] and se["ev_next_seq"] == 11
    ev = se["events"]
    assert ev[0] == {"seq": 5, "frame": 900, "type": "dmg", "target_net_id": 17, "amount": 12, "delta": -12}
    assert ev[1]["killer_net_id"] == 42 and ev[1]["victim_net_id"] == 17 and ev[1]["assist_net_id"] is None
    assert ev[2]["spell"] == "fireball" and ev[2]["pan"] == -40 and ev[2]["caster_net_id"] is None
    assert ev[3]["spell"] == "lightning" and ev[3]["caster_net_id"] == 18 and ev[3]["start"]
    assert ev[4]["charge"] == 60 and ev[5]["kind"] == "deathmatch" and ev[5]["winner_id"] == 42
    assert se["scores"][0] == {"net_id": 42, "class_id": 1, "class": "wizard", "observer": False,
                               "score": 3, "deaths": 1}
    assert se["scores"][1]["score"] == -1 and se["scores"][1]["observer"]
    assert se["obelisks"] == {0x55: 60}
    assert "friend" in s6["player"]["ext"]["ext_flags"] and s6["objects"][0]["ext"]["ext_flags"] == ["friend"]
    v6o = len(p6) - SESSION_SIZE - 6 * EVENT_SIZE - 2 * SCORE_SIZE - OBL_SIZE
    assert p6[v6o:v6o + 6] == b"Estate" and struct.unpack_from("<I", p6, v6o + 24)[0] == 3
    assert struct.unpack_from("<h", p6, v6o + SESSION_SIZE + 18)[0] == -12     # event f offset
    for bad in (p6[:-1], p6 + b"\0"):
        try:
            decode_state(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("bad v6 packet accepted")
    assert decode_state(p5)["session"] is None
    dd = EventDedup()
    assert [e["seq"] for e in dd.feed(s6)] == [5, 6, 7, 8, 9, 10] and dd.feed(s6) == []
    later = dict(sess, events=[(10, 905, 12, 3, 42, 1, 0, 0, 0), (13, 950, 3, 0, 17, 0, 0, 0, 0)])
    assert [e["seq"] for e in dd.feed(decode_state(encode_state(session=later)))] == [13] and dd.lost == 2
    assert decode_cmd(encode_cmd("spellset", 4)) == {"op": 10, "ttl_ms": 0, "seq": 0, "a": 4, "b": 0}
    # v3 packets (no ext block) still decode.
    v3 = decode_state(encode_state(flags=0x0026, objects=objects[:1], names=[(17, "x")],
                                   version=3))
    assert v3["version"] == 3 and "ext" not in v3["objects"][0] and v3["player"]["ext"] is None
    # v2 packets: reserved bytes (0) must not decode as Warrior.
    v2 = decode_state(encode_state(flags=0x0026, objects=objects[:1], names=[(17, "x")],
                                   version=2))
    assert v2["version"] == 2 and v2["objects"][0]["class_id"] is None
    assert v2["objects"][0]["hp_src"] is None and v2["objects"][0]["name"] == "x"
    assert p["name"] == "Jack 3" and o[1]["name"] is None
    assert o[0]["name"] == "Jäck-Wizard-Lo"  # 15-byte UTF-8 cut ("ä" is 2 bytes)
    assert len(o[0]["name"].encode("utf-8")) <= NAME_BYTES
    # v1 packets (no names block) still decode.
    v1 = decode_state(encode_state(flags=0x0026, objects=objects[:1], version=1))
    assert v1["version"] == 1 and v1["objects"][0]["name"] is None
    inv = decode_state(encode_state(flags=0x0006, hp=0xFFFF, hp_max=0xFFFF, mp=0xFFFF,
                                    mp_max=0xFFFF, facing=0xFF, anim=0xFF))
    assert not inv["player"]["valid"] and inv["player"]["hp"] is None and inv["player"]["x"] is None
    # Byte-level spot checks of the C layout.
    assert pkt[0:4] == b"NXPT"
    assert struct.unpack_from("<I", pkt, 24)[0] == 42          # net_id offset
    assert struct.unpack_from("<i", pkt, 36)[0] == 2048        # x offset
    assert struct.unpack_from("<i", pkt, 48)[0] == 1900        # cam_x offset
    assert pkt[56] == 1                                        # abilities[0].id
    assert struct.unpack_from("<H", pkt, 56 + 8 + 4)[0] == 120  # abilities[1].cd_remaining
    for bad in (pkt[:-1], b"XXXX" + pkt[4:]):
        try:
            decode_state(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("bad packet accepted")
    c = encode_cmd("move", orient_from_vec(1, 0), 3, ttl_ms=300, seq=5)
    assert len(c) == CMD_SIZE and c[0:4] == b"NXPC"
    assert decode_cmd(c) == {"op": 1, "ttl_ms": 300, "seq": 5, "a": 0, "b": 3}
    assert decode_cmd(encode_cmd("aim", -5, 70000)) == {"op": 2, "ttl_ms": 0, "seq": 0,
                                                        "a": -5, "b": 70000}
    # Orientation convention round-trips (east=0, south=64, west=128, north=192).
    assert [orient_from_vec(*d) for d in ((1, 0), (0, 1), (-1, 0), (0, -1))] == [0, 64, 128, 192]
    for o_ in range(0, 256, 7):
        assert orient_from_vec(*vec_from_orient(o_)) == o_
    gate_selftest()
    print("selftest OK (hdr=%d obj=%d cmd=%d)" % (HDR_SIZE, OBJ_SIZE, CMD_SIZE))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="round-trip encode/decode and exit")
    sub = ap.add_subparsers(dest="cmd")

    ls = sub.add_parser("listen", help="print state packets as JSON lines")
    ls.add_argument("--port", type=int, default=47800)
    ls.add_argument("--bind", default="0.0.0.0")
    ls.add_argument("--count", type=int, default=0, help="exit after N packets (0 = forever)")
    ls.add_argument("--every", type=int, default=1, help="print every Nth packet")

    sd = sub.add_parser("send", help="send one command")
    sd.add_argument("--to", required=True, help="3DS IP:listen_port, e.g. 127.0.0.1:47801")
    sd.add_argument("--ttl", type=int, default=0, help="latch ms for move/aim (0 -> 250)")
    sd.add_argument("--repeat", type=int, default=1, help="send N times")
    sd.add_argument("--interval", type=float, default=0.1, help="seconds between repeats")
    sd.add_argument("--dx", type=float, default=1.0)
    sd.add_argument("--dy", type=float, default=0.0)
    sd.add_argument("--orient", type=int, default=None, help="0..255 (0=east, 64=south)")
    sd.add_argument("--run", action="store_true")
    sd.add_argument("--stop", action="store_true", help="move: hold still (override stick)")
    sd.add_argument("--x", type=int, default=0)
    sd.add_argument("--y", type=int, default=0)
    sd.add_argument("--self", dest="self_cast", action="store_true", help="slot: cast on self")
    sd.add_argument("op", choices=sorted(OPS))
    sd.add_argument("args", nargs="*")

    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.cmd == "listen":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((args.bind, args.port))
        n = 0
        while True:
            data, addr = s.recvfrom(RECV_BUF)
            try:
                st = decode_state(data)
            except ValueError as e:
                st = {"error": str(e), "len": len(data)}
            st["from"] = "%s:%d" % addr
            n += 1
            if n % max(1, args.every) == 0:
                sys.stdout.write(json.dumps(st, separators=(",", ":")) + "\n")
                sys.stdout.flush()
            if args.count and n >= args.count:
                return 0
    if args.cmd == "send":
        pkt = build_cmd(args)
        target = parse_hostport(args.to)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for i in range(max(1, args.repeat)):
            s.sendto(pkt, target)
            if i + 1 < args.repeat:
                time.sleep(args.interval)
        return 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
