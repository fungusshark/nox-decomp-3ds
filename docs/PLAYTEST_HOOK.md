# Playtest game-state hook (dev only, issue #36)

A small UDP bridge that lets a host-side bot read the local player's state and
the client's visible objects, and send pad-equivalent commands. It replaces
screenshot perception for automated playtesting. It is **not** part of release
builds.

Files:
- `port/3ds/playtest_hook_3ds.c` / `.h`: the hook (wire structs live in the header).
- `scripts/playtest_hook_client.py`: stdlib host client (`listen`, `send`, `--selftest`).
- Call sites: `src/win.c` `sub_4453A0` (right after `nox_gamepad_update()`),
  `src/GAME2.c` opcode `0xB0` handler (`nox_pt_note_server_name`),
  `src/GAME2.c` client message dispatcher, top of the opcode switch (`nox_pt_note_msg`, v6 events),
  `port/3ds/net_3ds.c` `nox_3ds_net_shutdown()` (closes the socket before `socExit`).
- `port/3ds/net_sockets_3ds.c`: `nox_3ds_soc_ensure_ready_any_mode()` (hook builds only).

## 1. Build flag

```bash
cmake -S port/3ds -B build-3ds-hook -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
      -DNOX_PLAYTEST_HOOK=ON -DNOX_3DS_STAGE_WIN_DEV=OFF
cmake --build build-3ds-hook -j
```

`NOX_PLAYTEST_HOOK` defaults to **OFF**. When OFF, `playtest_hook_3ds.c` is not
compiled and `nox_pt_frame()` / `nox_pt_shutdown()` are empty `static inline`
functions, so the release binary has no hook code and no per-frame cost.
Use `-DNOX_3DS_STAGE_WIN_DEV=OFF` so a hook build never overwrites the staged
`/mnt/c/3ds-dev/nox.3dsx`.

## 2. Runtime enable: `sdmc:/3ds/nox/playtest.cfg`

The hook reads the file once, on the first event pump. If it is missing, the
hook stays inert (one predictable branch per frame, no socket, no `socInit`).

```ini
# where state packets go (required)
host=127.0.0.1
port=47800          # host UDP port for state (default 47800)
listen_port=47801   # 3DS UDP port for commands (default 47801)
publish_every_n_frames=1  # publish every N event pumps (1..60, default 1; alias: every=)
max_objects=32      # 0..32 (default 32)
commands=1          # 0 = state-only (default 1)
clip_viewport=1     # only objects inside the camera rect (default 1)
accept_any_src=0    # 1 = accept commands from any IP, not only host= (default 0)
mp_server_allow=giraffepus   # dev-only MP allowlist (comma-separated, up to 8, <=15 chars each; "*" = any server)
```

**LAN risk:** with `accept_any_src=1`, any device on the network that can reach
`listen_port` can drive your character. Use it only on an isolated or trusted
network. Commands are unauthenticated in both modes; the source-IP check is
only a basic filter.

Under Azahar, the guest's HLE sockets map to host sockets, so `host=127.0.0.1`
reaches WSL (verified in Azahar 2127.0.1, 2026-11-27: state packets arrive at
`127.0.0.1:47800` and commands sent to `127.0.0.1:47801` are accepted with
`accept_any_src=0`). On hardware, use the PC's LAN IP.

If `socInit`, `socket`, `O_NONBLOCK` or `bind` fails, the hook logs one line
via `nox_3ds_net_log` (`[pt] ...`), stays inert, and retries silently every 10 s. `sendto` errors are
logged once and otherwise ignored. In Solo, the hook pays the same 1 MB soc:u
buffer the Multiplayer path uses. `socExit` still happens only at shutdown.

## 3. Wire schema (state v6 / command v1, little-endian, packed)

The C `_Static_assert`s and the Python `*_SIZE` constants must agree. Run
`python3 -I scripts/playtest_hook_client.py --selftest` after any change.

### State packet (3DS to host): header (96 B) + `n_obj` x object (20 B)

| Off | Type | Field | Source |
|---|---|---|---|
| 0 | u32 | magic `"NXPT"` | |
| 4 | u16 | version = 6 (1 to 5 from older builds) | |
| 6 | u16 | header_size = 96 | |
| 8 | u32 | seq | |
| 12 | u32 | game_frame | `byte_5D4594[2598000]` |
| 16 | u32 | hook_tick | `nox_pt_frame` call count |
| 20 | u16 | flags | see below |
| 22 | u8 | player_class | playerInfo+2251, 0xFF = no playerInfo |
| 23 | u8 | n_obj | |
| 24 | u32 | net_id | `byte_5D4594[2616328]` |
| 28 | u16 x4 | hp, hp_max, mp, mp_max | `[1093040/44]`, `[1093060/64]` |
| 36 | i32 x2 | x, y | local drawable +12/+16 |
| 44 | u8 | facing | drawable +297 |
| 45 | u8 | anim | drawable +276 (low byte) |
| 46 | u16 | obj_size = 20 | |
| 48 | i32 x2 | cam_x, cam_y | `[811084/811088]` |
| 56 | 5 x 8 B | abilities: `u8 id, u8 ready, u8 active, u8 level, u16 cd_remaining, u16 cd_total` | record `1047764 + 24*a` (a = 1..5): id +0, ready +8, active mask +12, level +16, start frame +20; total = `sub_4252D0(id)` |

`cd_remaining = cd_total - (game_frame - start)` while not ready, else 0.

`level` is record +16. It is written only by opcode `0xCD` (`sub_4611E0`,
GAME2.c:15307, which also mirrors it to playerInfo `+3696 + 4*id`). Warrior
class init (GAME2.c:13509) copies the default table at `byte_587000[133536]`,
which has level 0 for all five abilities and ready 1. So `level == 0` means not
learned. In that case the hook reports `ready = 0` and `cd_remaining = 0`.
(Runtime-unverified: check that a learned ability shows `level >= 1`.)

When `player_valid` is 0, hp, hp_max, mp and mp_max are `0xFFFF`, facing and
anim are `0xFF`, and x and y are 0. The Python client decodes these as `null`.

Flags: `0x1` menu_shell, `0x2` map_loaded, `0x4` in_world
(`nox_3ds_input_is_world_mode()`: map loaded, no menu, modal or chat; **and**,
hook-only, the NPC `Dialog.wnd` at `byte_5D4594[1123524]` is absent or hidden
(`sub_46ACC0(w) == 1`)),
`0x8` is_mp (net mode WIFI and not Solo, `!sub_40A5C0(0x800)`), `0x10` is_host (`sub_40A5C0(1)`: a local server is running.
It is set in the menu shell and in Solo too, so it is **not** an MP signal. The
MP gate uses only `is_mp`), `0x20` player_valid (local drawable found; required for
`cmds_active`), `0x40` observer (playerInfo+3680 bit0), `0x80` hp_zero, `0x100`
bots_ok, `0x200` cmds_active, `0x400` obj_truncated.

Object (20 B): `u16 net_id (+128), u16 type_id (+108), u32 class_flags (+112),
i32 x (+12), i32 y (+16), u8 facing (+297), u8 hp_pct, u8 class_id, u8 hp_flags`.
In v1/v2 the last two bytes were a reserved `u16 0`, and `hp_pct` was always 0xFF.

**Object health and class (v3).** The hook exports only values the client
receives in order to draw them. It never reads server state:

- `hp_pct` (0..100, 0xFF = the client has no health value for this object):
  - **Players in Quest** (`sub_40A5C0(0x1000)`): playerInfo+2282. This is the
    percent `sub_4B8EB0` draws as the 48 px bar beside another player's name
    tag (call at GAME3.c:16243, Quest only). The server sends
    `100*hp/hp_max` (integer, floor) in opcode `0xC4` sub 12 (`sub_4D8670`,
    GAME3.c:41455; client store GAME2.c:52755) on join and on HP change.
    It is the server's exact HP **rounded down to 1%**, not exact HP.
    `hp_flags 0x01 quest_tag`.
  - **Players outside Quest** (deathmatch, CTF, and other modes): the server
    never sends other players' HP, and the client draws no bar, so `hp_pct`
    is 0xFF.
  - **Owned units** (summons and pets the server tracks for this client):
    the client table `byte_5D4594[1200916]` (32 x 16 B, `sub_495020`:
    +0 net code, +4 poison colour, +6 hp, +8 hp_max, +12 active). Opcode `0xDB`
    (`sub_4D9250`, sent only to the owner) adds an entry, `0xDC` removes it,
    `0xDD` sets hp/max and `0x41` sets hp as `2*byte`. The renderer draws
    `cur/max` with `sub_4BC080` (GAME3.c:16855, 19103). `hp_pct` is
    `100*cur/max`, floor. `0x41` updates are 2 HP coarse.
    `hp_flags 0x02 unit_bar`. Other monsters get 0xFF.
  - `hp_flags 0x04 poisoned`: the bar uses the alternate colour (playerInfo
    +3680 bit 10 for player tags, entry +4 for units; read from the renderer,
    meaning UNVERIFIED).
  - The HUD toggle (`sub_470A90`) only hides the tag bar. The hook exports the
    value whether or not the HUD shows it, since the client has it either way.
  - On a listen-server host, the host's playerInfo+2282 is only set if the
    host's own client receives `0xC4/12` (UNVERIFIED).
- `class_id` (players only, else 0xFF): playerInfo+2251 of the client player
  table entry for the object's net code. The client gets it in the opcode
  `0x2D` player record (GAME2.c:50294, `qmemcpy(+2185, pkt+3, 0x61)`, so it is
  packet byte 69). The scoreboard also uses it (GAME1.c:34002). Values: 0 Warrior
  (verified), 1 Wizard, 2 Conjurer. 1 and 2 are from the decomp (class 2 gates
  the creature-book tab, GAME2.c:11437/11856) and the OpenNox enum. They are
  not yet checked live.

Objects come **only** from client-side drawables, which are the set the server
chose to send. The hook does not walk the full list `byte_5D4594[1046572]`.
Instead it queries the client's spatial grid: 128 px buckets in
`byte_5D4594[1301784]` (`grid[x][y]` heads, chain next +400, filled by
`sub_49AA00`). The query goes through `sub_49AB00()`, the read-only walker the
renderer uses to build its per-frame draw lists (`sub_4754F0`/`sub_475560`,
GAME2.c:30805). It uses the same rect: camera rect plus 128 px below for tall
sprites. With `clip_viewport=0` (Solo only), the rect grows by one view on
each side. The cost is bounded by the drawables in the about 6x4 buckets
under the view, not by map size. At most 1024 drawables are visited per
publish (`PT_MAX_VISITS`); past that, `obj_truncated` is set. Entries with class
`& 0x20400000` (non-network, as skipped by `sub_45A6F0`) and the local player
are excluded. Objects with the invisibility enchant (drawable +124 bit 0, the
check the renderer makes at GAME2.c:30355) are dropped unless the local player
has enchant bit 21 (infravision, which reveals them at GAME2.c:32368). Invisible
allies are dropped too, because the ally test needs `sub_418C80`. With clipping
on, an object must also be inside the camera rect **and** pass `sub_4984B0()`.
That is the client's own draw-visibility test (view polygon / line of sight,
read-only), and the renderer calls it before drawing each drawable
(GAME2.c:30306). It uses the polygon from the last rendered frame. Clipping is
**forced on in MP**; `clip_viewport=0` only works in Solo. The list is sorted
(v5) **missiles within 320 px of the local player** first, then players, then
monsters, then other missiles, then others, nearest first inside each group,
and capped at `max_objects` (`pt_obj_sort_key`, `port/3ds/playtest_missile.h`).
Builds before v5 sorted all missiles after monsters. `obj_truncated` is set when
more candidates passed the filters than `max_objects`, or the visit budget ran
out. Past the budget, missiles are still considered (the class test is cheap)
up to twice the budget, so a crowded bucket cannot hide an incoming missile.

### Names block (state v2 and v3)

The 96 B header and 20 B objects keep the v1 layout. When the header's
`version` is 2 or more, the objects are followed by:

`u8 n_names, u8 entry_size (=18)`, then `n_names` x `{u16 net_id, u8 len,
char name[15]}`.

Each name is UTF-8 converted from the client player table's UTF-16 name
(`-fshort-wchar`). The table is `byte_5D4594[371780]`, 32 x 4828 B, net code
at +2060, active at +2092, name at +4704; it is the table `sub_417040` and the
scoreboard and name tags use. The hook reads it with its own lookup, because
`sub_417040` writes a slot byte. Names are cut to 15 bytes on a character
boundary and NUL-padded. Surrogate pairs become `?`. The local player comes
first (if known), then each exported player-class object. This means no other
player's name is sent when object export is gated off. The Python client
accepts v1, v2 and v3, and adds `name` to `player` and to player objects. For v3
it also adds `class_id`/`class` and `hp_src` (the names of the `hp_flags` bits) to objects. For
v1/v2 these are `None`, because a reserved 0 must not read as Warrior. The
v3 bump exists for this reason only: the byte layout is the same as v2. A
decoder that only knows v1 can parse the base layout by ignoring `version` and
any trailing bytes.

### Ext block (state v4)

When `version` is 4 or more, the names block is followed by:

`u8 n_ext, u8 entry_size (=20)`, then `n_ext` x
`{u16 net_id, u8 anim, u8 ext_flags, u32 enchant, u32 weapon_flags,
u32 armor_flags, i16 vx, i16 vy}`.

The first entry is the local player when `player_valid` is set, then one
entry per exported object, in object order. Like the names block, it is
attached only to objects that are already exported, so a closed MP gate sends
only the local entry. Every field is client-received display state:

- `enchant`: drawable +124, the enchant mask the renderer tests with
  `sub_4356C0(d, bit)` to draw auras and status effects. The client writes it
  only in opcode `0x5A` (GAME2.c:51124-51138, `u16 net code, u32 mask`).
  Bit 0 invisible and bit 21 infravision are confirmed by renderer use
  (GAME2.c:30355, 32368). The Python client names the other bits in the
  OpenNox `ENCHANT_*` order (3 confused, 4 slowed, 5 held, 9 hasted,
  17 protect_from_fire, 20 protect_from_electricity, 26 shield (Force Field),
  27 reflective_shield, ...). Those names are UNVERIFIED live. Poison is not an
  enchant: use `hp_flags 0x04` (v3) where the client has a bar.
- `weapon_flags` / `armor_flags` (players only, `ext_flags 0x08 equip`):
  client player table +4 / +0. Opcodes `0x50`/`0x51` OR a weapon bit in,
  `0x4F`/`0x52` an armor bit (`sub_417AA0`, GAME1.c:21632); `0x54`/`0x53`
  clear them (`sub_417B80`, GAME1.c:21700). These are the masks the client
  uses to draw equipment on the player sprite. The mask holds every equipped
  weapon-class item (for example a bow plus its quiver), not only the one in
  hand. Bit meanings follow the thing.bin weapon/armor subclass flags.
- `anim`: drawable +276 low byte for every object (the header already has the
  local one). Semantics UNVERIFIED (3 jump, 27 melee swing seen).
- `vx`/`vy` (`ext_flags 0x01 vel`): world px per 16 game frames (Q4
  px/frame), computed by the hook from the client position of the same net id
  in the previous publish, divided by the `game_frame` delta (skipped when
  the delta is 0 or 64+ frames). Not a server velocity. From v5, missiles
  use the per-pump tracker instead (v5 block, `tracked`), so they usually have
  a velocity in their first exported packet.
- `ext_flags 0x02 owned`: the object is in this client's owned-unit table
  (`byte_5D4594[1200916]`, own summons and pets, opcode `0xDB` sent only to
  the owner). The client does not know the owner of other players' creatures
  or their orders, so neither is exported.
- `ext_flags 0x04 missile`: `class_flags & 1`. Network missile objects
  (Fireball, Magic Missile, Force of Nature ball, arrows, bolts and similar)
  are ordinary drawables, so they are in the object list with their
  `type_id`, position and the hook velocity. From v5, near missiles sort
  first and their velocity comes from the per-pump tracker (see the v5
  block). Beams and
  instant effects (Death Ray, lightning, Fist of Vengeance marker) arrive
  as effect packets, not drawables, so the hook does not see them (TODO).

The Python client accepts v1 to v6. For v4 it adds `ext` to `player` and to
each object (`enchants` names, `enchant_mask`, `weapon_flags`,
`armor_flags`, `vel` in px/frame, `is_missile`, `owned_by_me`). Older
decoders reject newer versions by version; `hookio.py` loads the client from
`NOX_HOOK_CLIENT` or the first checkout it finds, so point it at a v5 client
before running a v5 build.

### v5 block (state v5): spell set and missiles

When `version` is 5 or more, the ext block is followed by:

`u8 spell_set, u8 v5_flags (=0), u8 n_mis, u8 entry_size (=12)`, then `n_mis` x
`{u16 net_id, u8 mis_flags, u8 reserved (=0), u32 subclass, u16 miss_px, u16 tca_f}`.

A v5 packet ends after this block; v6 appends the v6 block (below).
The v4 layout before it is unchanged, so a v4 decoder that skips the version
check and stops at the ext block still reads a v5 packet.

- `spell_set` (0..4, 0xFF = unknown): the active spell set, the "Spell Set #N"
  on the bottom screen minus 1. It is client GUI state: the spell-bar struct
  `*(byte_587000[133480])` (= `&byte_5D4594[1049220]`, GAME1.c:4592), byte
  +200, read by `sub_45E180` and written by `sub_45E110` (Next/Previous set
  `sub_4604F0`/`sub_460540`, the direct SelectSpellSetN control 44). Warriors
  have the same bar (abilities), so the value is there for every class.
- One entry per **exported** missile object (`class_flags & 1`), in object
  order, at most 16. A closed MP gate exports no objects, so no entries.
  - `subclass`: drawable +116, the thing.bin SUBCLASS mask the client copies
    from its own thing table (`sub_44D0C0`, GAME2.c:310). For pure missiles,
    bit 0 is MISSILE_COUNTERSPELL and bit 1 is MAGIC (the subclass Inversion
    reflects; the server tests `+12 & 2` in `sub_52BE40`, GAME4.c:45711).
    Examples: Magic (955), MagicMissile (704) and Pixie (672) are 0x3;
    Fireball (693-697) and DeathBall (706) are 0x1. Arrows and bolts
    (MISSILE+WEAPON) use the weapon subclass table. The Python client names
    bits 0/1 `counterspell`/`magic`.
  - `mis_flags`: `0x01 incoming` (approaching, and the straight-line closest
    approach is within 64 px and 90 game frames), `0x02 near` (within 320 px),
    `0x04 tracked` (the velocity below and the ext `vel` come from the tracker),
    `0x08 approaching` (the distance is shrinking).
  - `miss_px`, `tca_f`: closest approach distance (px) and the game frames
    until it, from the tracker velocity with the local player assumed still.
    0xFFFF when there is no velocity or the missile is moving away. Homing
    missiles curve, so treat these as a hint.
- **Tracker.** Every event pump (not only every `every`-th, which publishes),
  the hook samples the missiles that pass the export filters (camera rect,
  `sub_4984B0`, invisibility) into a 32-slot table, and derives the velocity
  from the last two samples in different game frames. This is why a missile
  has a velocity in its first exported packet. The drawable's own previous
  position (+32/+36) is not used, because the client smoothing pass
  (GAME2.c:9860) rewrites it and resets it on moves over 10 px.
- **Owner: not exported.** The client drawable has no owner field. The server
  object keeps owner and target in its missile data (`+748`, used by
  `sub_52BE40`), which the hook must never read. A bot can still infer its own
  missiles (they appear next to it right after a cast).

Why v5 (issue #36, missiles "rare"): live v4 recordings showed missiles in the
stream, but bots publish with `every=2` (about 4 game frames apart) and v4
velocity needed the same net id in the previous *publish*. Crossbow bolts
(about 44 px/frame) were exported a median of once, and 25 % of all missile
samples (every first sighting) had no velocity. Without velocity the bots
cannot tell an incoming missile, so Inversion and Counterspell never fired.

### v6 block (state v6): session, event ring, scoreboard, obelisks

When `version` is 6 or more, the v5 block is followed by one 52 B session
record, then `n_ev` events (20 B), `n_score` scores (12 B) and `n_obl`
obelisks (4 B). Sizes are in the record, so a later version may grow entries.
A v6 packet ends after the obelisks.

Session (`nox_pt_session_t`):
`char map[24], u32 map_seq, u32 game_flags, u32 timer_ms, u16 frag_limit,
u8 time_limit_min, u8 s_flags, u32 ev_next_seq, u8 n_ev, u8 ev_size (=20),
u8 n_score, u8 score_size (=12), u8 n_obl, u8 obl_size (=4), u16 reserved`.

| Field | Source |
|---|---|
| `map` | `sub_409B40()` = `byte_5D4594[2649740]`, the map name without path or extension, set by `sub_409D70` from MSG_USE_MAP `0x2B` (GAME2.c:50239) |
| `map_seq` | hook counter, +1 for each accepted `0x2B` (same test as the client: s32 +37 > `[1200804]`). A change means a new map or new game: reset per-game bot state |
| `game_flags` | `byte_5D4594[3600]` (`sub_40A5C0` tests it). Mode bits `& 0x17F0` (0x400 elimination/"lessons", 0x1000 Quest, 0x2000 MP kill messages, ...) |
| `timer_ms` | `sub_40A230()` remaining time when `sub_40A220()` (timer on, MSG_TIMER_STATUS `0xD3`) is set, else 0xFFFFFFFF. Unit is the `sub_416BB0` clock (ms, UNVERIFIED live) |
| `frag_limit`, `time_limit_min` | `sub_416590(0)` +54 / +56, written from MSG_GAME_SETTINGS `0xAF` bytes +18 / +19 on a client (GAME2.c:52270). On the host they are its own settings (UNVERIFIED) |
| `s_flags` | `0x01 game_over`: `sub_40A5C0(8)`, set by the winner reports `0x56..0x59` (also on the host, where the server sets it), cleared by `0x2B`. `0x02 timer_on`. `0x04 events_gated`: MP gate closed, events about others are not recorded |
| `ev_next_seq` | seq the next event gets |

**Respawn timer:** not shown by the client. In deathmatch the dead player
gets MSG_INFORM 12 ("InObservationMode" / "PressJump") and respawns on jump,
so there is no countdown to export. Use the `respawn` event and `observer`.

**Event ring.** `nox_pt_note_msg` sees every server message in the client
dispatcher before its handler runs and parses it with
`port/3ds/playtest_events.h` (pure C, host-tested). Events go into a 256-entry
ring with `seq` 1, 2, ... Every packet carries the **newest 40** events
(oldest first), so each event repeats until 40 newer ones push it out. The
host dedups by `seq` (`EventDedup` in the Python client); a gap in `seq`
means events were lost. Events about other objects are recorded only while the
object export is allowed (Solo, or MP with the gate open); `map` and
`game_over` events are always recorded.

Event (`pt_event_t`, 20 B): `u32 seq, u32 frame, u8 type, u8 b, u16 a, u16 c,
u16 d, u16 e, i16 f`. Net ids are masked with `0x7FFF` like `sub_578B30`.

| type | name | fields | Source message (opcode = MSG index + 0x27) |
|---|---|---|---|
| 1 | dmg | a target, f delta (amount = -f) | `0x42` MSG_REPORT_HEALTH_DELTA `{u16 net, i16 delta}`, the floating damage digits (`sub_49A650` list, drawn by `sub_49A6A0`) |
| 2 | kill | a victim, c killer player (0 none), d assist player, e code, b kind | `0xA9` MSG_INFORM sub 14 (11 B), the kill-feed line (`sub_495210`/`sub_4952E0`, "%s %s") |
| 3 | died | a player | `0xE8` MSG_PLAYER_DIED |
| 4 | respawn | a player, b flags, c byte +7 | `0xE9` MSG_PLAYER_RESPAWN |
| 5 | chant | b phoneme 0..8, c female, d volume, f pan | `0xA6`/`0xA7` MSG_AUDIO_EVENT with sound 186..201 |
| 6 | cast | c spell id, b female, d volume, e phonemes, f pan, a = 0 | hook-decoded chant (below) |
| 7 | dspell | a caster, d target, c spell id (0 = Harpoon), b sub, e level, f 1 start / 0 stop | `0x9E` MSG_FX_DURATION_SPELL |
| 8 | summon | a summon id, c x, d y, e creature thing type, b caster dir, f frames | `0x7E` MSG_FX_SUMMON (`sub_5236F0`) |
| 9 | summon_cancel | a summon id | `0x7F` |
| 10 | obelisk | a obelisk net id, b charge 0..100 | `0xB3` MSG_OBELISK_CHARGE |
| 11 | map | | `0x2B` MSG_USE_MAP (accepted) |
| 12 | game_over | b 1 flagball / 2 flag / 3 deathmatch / 4 team DM, a winner (player net id or team), c byte +3 (1 = time limit, UNVERIFIED) | `0x56..0x59` MSG_REPORT_*_WINNER (accepted) |

Client-visible evidence and limits, per item:

- **Damage (`dmg`).** The server sends `0x42` from the per-client object
  update (`sub_5188A0`/`sub_518960`/`sub_518C30`, GAME4.c:29711-29941) when an
  object's HP dropped since the HP that client last saw (`sub_4D8760`, only
  for `delta < 0`). It goes to every client the object is visible to, so it
  covers **all** damage to visible objects (ours, other players', monsters'),
  not only hits we land. **The message has no source**: attribute it on the
  bot side (our missile/melee just hit, see `kill`). Players also get one for
  themselves.
- **Kills (`kill`).** `sub_54D2B0` (GAME5.c:6092) builds the record when a
  player dies with game flag 0x2000 and broadcasts it (`sub_4DA180(14)`, to
  all players): +2 killer (the owner of the killing object if it is a player),
  +4 assist (the last other player that hurt the victim within 10 s), +6
  victim, +8 code, +10 kind (1 = killed by a monster, code = its thing type;
  otherwise the victim's last damage kind/code, playerInfo +304/+300;
  meaning UNVERIFIED). Use `scores` for exact totals.
- **Scoreboard.** The rank screen (`guirank.c`, GAME2.c:25430-25850) shows the
  client player table: +2136 score ("lessons": +1 per kill, -1 for a suicide
  or team kill, `sub_4D8E90`/`sub_4D8EC0`) and +2140 deaths (`sub_4D8D40`).
  MSG_REPORT_LESSON `0x4E` broadcasts both to every client (`sub_4D8EF0`).
  Each packet has one 12 B entry per active player: `u16 net_id, u8 class_id,
  u8 pflags (+3680 low byte, bit 0 observer), i32 score, i32 deaths`. With a
  closed MP gate, only the local player.
- **Others' spell casts.** There is **no** cast message with a caster and
  spell id for other players. What the client gets:
  - **Chant sounds.** The server plays every phoneme of a spell to every other
    player (`sub_4FC960` -> `sub_501960(sound, caster, 2, listener)`,
    GAME4.c), as `0xA6`/`0xA7` `{i8 pan, u16 sound | volume << 10}`. The sound
    gives the phoneme and the caster's gender (`sub_4FE1C0`: male 186..193,
    female 194..201), not the caster. The hook walks the client's own spell
    trie (`*(byte_587000[66120])`, node +0 spell, +4+4p children,
    `sub_424830`) per chant stream (gender, pan within 48, gap <= 30 frames)
    and emits `cast` with the spell id when the path ends in a leaf, or 10
    frames after the last phoneme when a longer spell shares the prefix.
    The caster is unknown: match `pan` (stereo side, sign UNVERIFIED) and
    `volume` (distance) against visible enemy players. Spells with no chant
    (wands, staffs, scrolls) produce nothing. The local player's own chant is
    not sent this way (it uses MSG_REPORT_SPELL_START `0x70`).
  - **Duration rays (`dspell`)** carry caster and target: Lightning (24),
    Chain Lightning bolt (7), Drain Mana (22), Charm (9), Greater Heal (35),
    Plasma (59) and Harpoon (sub 7/14).
  - **Summons (`summon`)**: point, creature thing type and duration, no
    caster (`sub_5236F0`).
  - Missiles (Fireball, Magic Missile, Force of Nature, Pixie, ...) are
    drawables with their `type_id` (v5). Force Field and other buffs are
    enchant bits (v4). Toxic Cloud, Meteor and similar are ordinary drawables
    with their thing type. Instant effects (Fist, Death Ray, explosions)
    arrive as FX packets with only a point; they are not exported yet.
- **Obelisk mana.** MSG_OBELISK_CHARGE `0xB3 {u16 net, u8 charge}` sets the
  obelisk's light (`16*c/10`) and animation frame (`8*c/50`). The hook keeps
  the last charge per obelisk (16 slots, cleared on map change) and sends
  them as `{u16 net_id, u8 charge, u8 0}`; the `obelisk` event marks each
  change. Only obelisks the server updated for this client are known.
- **Creature owner.** Not known for other players' creatures: the client
  drawable has no owner field. The client does know (v4 `owned`) its own
  creatures and, from v6, `ext_flags 0x10 friend`: the object is in the
  client friend list, filled by MSG_OBJECT_FRIEND_ADD/REMOVE/RESET
  (`0x34`/`0x35`/`0x36`, list `byte_5D4594[1203864]`, `sub_495A80`). The
  server adds every object that is not hostile to this player (`sub_4E7F10`,
  `sub_4E7F90` = !`sub_5330C0`): teammates and their creatures. So
  `friend && !owned` creature = a teammate's, `!friend` = an enemy's.

All v6 fields are client-received display state. The parser runs only on
messages the client already handles, and the hook still never reads server
objects (on the host, `0x42`/`0xA9` are the messages the host's own client
receives).

### Command packet (host to 3DS): 20 B

`u32 magic "NXPC", u8 version=1 (command version, independent of state v2), u8 op, u16 ttl_ms, u32 seq, i32 a, i32 b`.

| op | name | a / b | Applied via |
|---|---|---|---|
| 0 | NOP | | |
| 1 | MOVE | a = orientation 0..255 (0 E, 64 S, 128 W, 192 N), b = 0 stop / 1 walk / 3 run | `nox_ctrl_set_thumbstick_move` (latched, ttl) |
| 2 | AIM_WORLD | world x, y | `nox_3ds_input_set_hook_world_aim` + `nox_ctrl_set_radial_aim` + `nox_ctrl_inject_mouse_abs` (latched, ttl) |
| 3 | AIM_DIR | a = orientation; point 64 px from the player | same |
| 4 | ATTACK | a = hold frames (0 = 2) | LMB tap (`nox_ctrl_inject_mouse_button(0)`) |
| 5 | SLOT | a = 1..5, b != 0 = self-cast | key A/S/D/F/G tap; self-cast = LSHIFT held one frame earlier |
| 6 | HEAL | | key X tap (QuickHealth) |
| 7 | MANA | | key C tap (QuickMana) |
| 8 | JUMP | | MMB tap |
| 9 | STOP | | drop latches, release held inputs |
| 10 | SPELLSET | a = spell set 0..4 | W (previous) / E (next) key taps, the keys ZL / ZR send, the shorter way round the 5-set wrap; at most 2 taps 3 pumps apart. Needs v5 `spell_set` known; ignored while an earlier SPELLSET is still tapping |

MOVE and AIM latch for `ttl_ms` (0 = 250 ms, max 2000 ms) after the last
packet. If the host process dies, input stops within the TTL, and the human pad
takes over again on the next pump. Taps are one-shot and released after their
hold frames. Up to 4 command datagrams are drained per pump, and they are
accepted only from `host=` unless `accept_any_src=1`.

The hook runs after `nox_gamepad_update()`, which includes
`nox_3ds_input_process_hybrid()`. It overrides the latched move and aim values,
and its `mouse_abs` is queued after `input_3ds`'s own. The engine then
translates everything exactly as it would a human pad (`sub_42D6B0` sends
opcode `0x3F` to the server). While moving, the cursor and facing follow the
move direction, as they do for a human. Aim fully applies when the bot is
standing still.

**Aim drives the world-aim point, not only the mouse.** The 3DS client
hovers and picks objects from `input_3ds`'s world-aim point
(`nox_3ds_input_get_world_aim`, read by `sub_476FA0` hover in `src/GAME2.c`
and the world cursor in `src/GAME1.c`), which for a human sits 24 px from the
hero along the last C-Stick / Circle Pad direction. While an AIM_WORLD /
AIM_DIR latch is on, the hook installs an override
(`nox_3ds_input_set_hook_world_aim`, `port/3ds/playtest_aim.h`) so
`nox_3ds_input_get_world_aim` returns the hook's screen aim point. Hover,
pickup (ATTACK click on a hovered item within 75 px), melee aim and skill
facing then follow the hook's aim. The override is released on aim timeout,
STOP, gate close (`cmds_active` false) and shutdown, so the human aim is back
on the next pump. Only builds with `NOX_PLAYTEST_HOOK` have the override.
Older hook builds moved only the mouse, so hook aim plus ATTACK hovered
nothing. The host unit test (`scripts/test_playtest_gate.c`, run by
`--selftest`) covers the override and world-to-screen clamp.

## 4. Gating and cheat surface

**Dev allowlist (`mp_server_allow`).** MP commands and the export of other
objects and names are also enabled when the current server name equals an
allowlist entry: case-insensitive, **exact match**, not prefix, since Nox server
names are 15 chars max. It uses the same fail-closed session-name state
machine as the tag check (see below); with no known session name, the
allowlist does not apply. Flag `0x800 allowlisted` is set on a
match; `0x100 bots_ok` still reports only the tag.

**Dev wildcard (`mp_server_allow=*`).** If any entry is `*`, MP commands and
the export of other objects and names are allowed on **every** server. This
holds even before the server name is known; for example, a Direct-IP join is
allowed. The packet flags then carry `0x800 allowlisted` and `0x1000
wildcard`, so logs show that the wildcard opened the gate. It exists for the
developer's own test server, where checking the name adds nothing. Never use
it on a bot that may join other people's servers. Like the allowlist, it lives
only in hook builds, in the bot's own `playtest.cfg`. The decision is
`pt_gate_mp_allow()` in `port/3ds/playtest_gate.h`, which the host gate test
covers. Rationale: this exists for the
developer's own LAN games (for example `giraffepus`), where the host agrees
but renaming the server to fit `[bots-ok]` into 15 chars is impractical. It
lives only in hook builds (`NOX_PLAYTEST_HOOK=ON`) and in a cfg file on the
bot's own SD card. Release builds are unaffected. It is a dev convenience, not
an opt-in from the other players: use it only for servers whose players know
a bot is present.

- Commands are accepted only when `commands=1` and `in_world` is set.
- **Multiplayer** (net mode WIFI): commands also need the server name to
  contain `[bots-ok]` (case-insensitive) or match `mp_server_allow` (see above). The host checks its own name
  (`sub_40A4C0()`, `byte_5D4594[1324]`). A client checks the name received in
  opcode `0xB0` (`sub_416590(0)+9` = `byte_5D4594[371389]`, GAME2.c:52317).
  The client never reads the engine copy at `byte_5D4594[371389]` for the gate.
  That copy persists across sessions, so a stale `[bots-ok]` name could leak
  into the next server. Instead, `GAME2.c` forwards each fresh `0xB0` to
  `nox_pt_note_server_name()`. A hook-owned state machine
  (`port/3ds/playtest_gate.h`) decides which name counts.

  The live bug was that `0xB0` arrives while **browsing** the server list,
  before the session counts as MP. It is not resent after the join, so the
  old "clear when not MP" rule wiped the name and the gate never opened.

  The new rules:
  - While no map is loaded, the hook captures the name of the server-list row
    selected as the join target. That row is
    `*(byte_5D4594[814624]) + 120` (14 chars, valid when `[815056]` is
    set); it is the row `sub_43B300` / `sub_438A90` actually join.
  - On the first map-loaded frame in MP, that row name becomes the session
    name. If the row name is 14 chars and the last browsed `0xB0` name
    extends it, the full 15-char name is used instead.
  - `0xB0` names seen while browsing never open the gate by themselves. So
    browsing several servers and then joining a non-allowlisted one stays
    closed.
  - A Direct-IP join has no row, so it stays closed until an in-session `0xB0`
    arrives (TODO: capture the Direct-IP target).
  - The session name is cleared on return to the menu shell after a map, or
    when the game leaves MP. Map rotation within a session keeps it.
  - `scripts/test_playtest_gate.c` unit-tests these cases on the host. It is
    run by `playtest_hook_client.py --selftest` when a C compiler is present.
  Nox server names are at most 15 chars, so the tag uses 9 of them.
- Object `hp_pct`/`class_id` (v3) come only from client-received display data
  (Quest name-tag bar percent, owned-unit bars, player-table class). They are
  attached only to objects that are already exported, so a closed MP gate
  sends none of them.
- Without the tag, MP state-out sends the header and local player only
  (`n_obj = 0`). If the gate closes, all overrides drop
  and held keys are released.
- HP, mana and position are server-authoritative. The client only stores what
  it receives, and the hook never writes game state. Commands are the same
  control codes a pad produces, so the bot has a human's control power. Its
  edges are information (exact coordinates and cooldowns) and reaction time.
  Viewport clipping and the client-only object list limit the information edge.
- On the host, the server object list lives in the same process. The hook must
  never call `sub_4DA7xx`, `sub_4ECCB0`, `sub_518EE0` or the server branch of
  `sub_418C80`. Reviewers should grep for these.

## 5. Field verification (Azahar, 2026-11-27, issue #36)

Verified in an Azahar field-dump session (Xvfb, OpenGL/llvmpipe, Solo Warrior
and Solo Quest Warrior). Evidence: `playtest-captures/raw/hook_verify/`
(`state_127.jsonl` plus screenshots).

Verified:
- Transport: guest `host=127.0.0.1` reaches WSL; host-to-guest commands on
  `127.0.0.1:47801` are accepted.
- No `playtest.cfg`: no packets and no `[pt]` log line (inert).
- Menus: `flags = 0x11` (`menu_shell | is_host`), `player_class = 0xFF`,
  `n_obj = 0`. `is_host` (`sub_40A5C0(1)`) is also set in the menu shell and in
  Solo, so it is not an MP signal on its own. Use `is_mp`.
- Modals: the chapter scroll, the Start menu and Quit confirm clear `in_world`
  and `cmds_active` (`flags = 0x32`). MOVE sent during the Start menu had no effect.
  `game_frame` stays frozen while the chapter scroll is up.
- `player_class`: Warrior = **0** (Solo and Solo Quest). Wizard and Conjurer are not checked yet.
- HP: a fresh Solo Warrior reads `hp = 20/20` and the HUD vial is full. A Solo Quest
  Warrior reads `450/450` (HUD 100%). Mana reads `0/0` for a Warrior.
- `facing` (drawable +297) is the 3x3 keypad direction code, not 0..7:
  `0 NW, 1 N, 2 NE, 3 W, 5 E, 6 SW, 7 S, 8 SE` (4 is unused). Observed: N=1,
  W=3, E=5, S=7, SE=8. When a MOVE latch expires, facing snaps back to 7.
- `anim` (drawable +276): `0` idle, `4` walk (MOVE b=1), `6` run (MOVE b=3),
  `3` jump (JUMP), `27` melee swing (ATTACK, seen for one or two packets).
- `cam_x/cam_y` is the top-left of a **640x384** world view (player - cam =
  (320,192)). The 3DS draws that view scaled to 400x240, so `clip_viewport`
  with the `sub_437250()` rect matches what is on screen. Objects up to |dx| 317 and
  |dy| 192 from the player were reported, and none were outside the rect.
- Objects: NPC (Airship Captain) `class_flags 0x200002` at the on-screen
  position, and static props `0x100008`. In a black line-of-sight area next to a
  cliff, no objects were reported.
- Commands: MOVE (all 8 directions, walk and run) moves the hero. Input stops
  within the TTL after the last packet. AIM_DIR turns the hero to face that way.
  ATTACK swings and JUMP jumps. HEAL is accepted (no potion to drink).
- Ability records: ids 1..5 with `cd_total` 300/300/150/30/600 frames.
  Unlearned abilities report `level 0, ready 0` (fixed in 72ed96f and
  re-verified). Before the fix they reported `ready=1`.

- v4 ext block (Solo Warrior, airship spawn, Azahar): the local entry carried
  `ext_flags vel|equip`, `weapon_flags 0x100`, `armor_flags 0x405`, enchant 0
  and velocity 0 at rest; NPC and props had `vel` with 0 and no equip flags.
  Enchant bits, missiles and velocity while moving are not yet checked live.

Still unverified:
- The `cd_remaining` countdown (no learned ability was available). SLOT/self-cast
  is not exercised yet.
- Death (`hp_zero`, death `anim`) and `observer`: not reached.
- Wizard/Conjurer class values (decomp says 1/2, see section 3). `class_flags` bit names other than 4 (player), 2 (monster/NPC) and
  `0x100000` (static prop, observed).
- Invisibility (bit 0) and infravision (bit 21).
- Real-hardware frame cost. In the emulator, the FPS counter shows no
  measurable difference (Solo Quest 21 FPS without cfg vs. 25-26 FPS with the hook, on llvmpipe;
  the noise is larger than any hook cost).

Round 2 re-verification (72ed96f; evidence in `hook_verify/round2/`):
- **PASS: Solo Quest player lookup.** On Quest stage 1 (Dismal Swamp),
  `player_valid = 1`, the position is real and tracks MOVE, and `cmds_active` is set.
  `hp = 450/450` with the HUD at "Health 100%" and a full vial, so 450 is the
  real Quest Warrior HP. 150/150 appears only for the first 1-2 packets after
  spawn. In menus HP/MP/pos/facing/anim decode as null (0xFFFF/0xFF).
- **PASS: NPC dialog gate.** While the Airship Captain dialog is open,
  `flags = 0x32` (no `in_world` or `cmds_active`), and MOVE is refused (no position
  change). After GoodBye, `0x236` returns and MOVE works again.
- **PASS: abilities.** A fresh Solo and Quest Warrior reports all 5 records as
  `level 0, ready 0`. A learned ability and its cooldown countdown are still not checked.
- **Open issue (fixed after this run, re-verify): the Quest object list is incomplete.**
  The export now uses the spatial-grid query described in section 3 instead of
  the capped list walk. Original finding: `PT_MAX_WALK` (2048) capped the exported object walk. On Quest stage 1, `obj_truncated` is set in
  every packet. Only NPCs (`0x200002`) and one `0x180000` were ever reported,
  and an item lying at the player's feet (on-screen "USE" marker) was not
  listed. The hook is blind to objects beyond the first 2048 list nodes.
- Death and observer: still not reached (no monsters met in about 3 minutes of
  Dismal Swamp wandering).

Round 3 re-verification (4df4691, spatial-grid export; evidence in
`hook_verify/round3/`). Quest stage 1 rolled "The Underworld", so the round-2
swamp item case could not be reproduced on the same map.
- **PASS: no truncation.** `obj_truncated` was clear in all 1006 map-loaded
  packets. At most 4 objects per packet.
- **PASS: objects match the screen at the spots checked** (start room plus about 10 spots):
  the NPC (`0x200002`, type 1378) and a prop (`0x100008`) in the start room, and
  rubble (`0x100000`, type 60) south-east of the exit at the matching screen
  offset (world delta x 0.625 = screen). Nothing was listed while the player
  stood in black LOS areas (spot_2, spot_8, run_S).
- **NOT CHECKED: item at feet.** No floor item or monster was met on this map,
  so the exact round-2 case is still open.
- FPS in the emulator (llvmpipe): start room 18 FPS with the hook vs. 20 FPS
  without cfg (different random map). The difference is within noise.

## 6. TODO

- `[bot] ` player-name prefix: not done. The name lives in playerInfo+4704
  (wchar, GAME2.c:19544) and is chosen before join (character creation and
  the `.plr` load). The injection point in the join packet is still to be located.
- Verify v3 object `hp_pct` live: Quest co-op tag bar (`quest_tag`) and a
  Conjurer summon (`unit_bar`); confirm Wizard = 1 and Conjurer = 2 for `class_id`.
- Invisibility bit 0 / infravision bit 21 are read from renderer code; confirm them in a dump. Allow invisible allies once a client-only team test exists.
- Session-name capture from the selected server-list row: confirm it in a live LAN join, and confirm that the row's +120 name matches the `0xB0` name.
- Gear and inventory fields (playerInfo +4 / +2328).
