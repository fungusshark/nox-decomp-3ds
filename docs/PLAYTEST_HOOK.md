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
every=2             # publish every N event pumps (1..60, default 2)
max_objects=32      # 0..32 (default 32)
commands=1          # 0 = state-only (default 1)
clip_viewport=1     # only objects inside the camera rect (default 1)
accept_any_src=0    # 1 = accept commands from any IP, not only host= (default 0)
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

## 3. Wire schema (v1, little-endian, packed)

The C `_Static_assert`s and the Python `*_SIZE` constants must agree. Run
`python3 -I scripts/playtest_hook_client.py --selftest` after any change.

### State packet (3DS to host): header (96 B) + `n_obj` x object (20 B)

| Off | Type | Field | Source |
|---|---|---|---|
| 0 | u32 | magic `"NXPT"` | |
| 4 | u16 | version = 1 | |
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
i32 x (+12), i32 y (+16), u8 facing (+297), u8 hp_pct (0xFF = unknown), u16 0`.

Objects come **only** from the client drawable list `byte_5D4594[1046572]`
(next = +368). That is the set the server chose to send. Entries with class
`& 0x20400000` (non-network, as skipped by `sub_45A6F0`) and the local player
are excluded. Objects with the invisibility enchant (drawable +124 bit 0, the
check the renderer makes at GAME2.c:30355) are dropped unless the local player
has enchant bit 21 (infravision, which reveals them at GAME2.c:32368). Invisible
allies are dropped too, because the ally test needs `sub_418C80`. With clipping
on, an object must also be inside the camera rect **and** pass `sub_4984B0()`.
That is the client's own draw-visibility test (view polygon / line of sight,
read-only), and the renderer calls it before drawing each drawable
(GAME2.c:30306). It uses the polygon from the last rendered frame. Clipping is
**forced on in MP**; `clip_viewport=0` only works in Solo. The list is sorted players, then monsters, then missiles, then others,
nearest first, and capped at `max_objects`. The walk is bounded at 2048 nodes;
hitting the cap sets `obj_truncated`.

### Command packet (host to 3DS): 20 B

`u32 magic "NXPC", u8 version=1, u8 op, u16 ttl_ms, u32 seq, i32 a, i32 b`.

| op | name | a / b | Applied via |
|---|---|---|---|
| 0 | NOP | | |
| 1 | MOVE | a = orientation 0..255 (0 E, 64 S, 128 W, 192 N), b = 0 stop / 1 walk / 3 run | `nox_ctrl_set_thumbstick_move` (latched, ttl) |
| 2 | AIM_WORLD | world x, y | `nox_ctrl_set_radial_aim` + `nox_ctrl_inject_mouse_abs` (latched, ttl) |
| 3 | AIM_DIR | a = orientation; point 64 px from the player | same |
| 4 | ATTACK | a = hold frames (0 = 2) | LMB tap (`nox_ctrl_inject_mouse_button(0)`) |
| 5 | SLOT | a = 1..5, b != 0 = self-cast | key A/S/D/F/G tap; self-cast = LSHIFT held one frame earlier |
| 6 | HEAL | | key X tap (QuickHealth) |
| 7 | MANA | | key C tap (QuickMana) |
| 8 | JUMP | | MMB tap |
| 9 | STOP | | drop latches, release held inputs |

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

## 4. Gating and cheat surface

- Commands are accepted only when `commands=1` and `in_world` is set.
- **Multiplayer** (net mode WIFI): commands also need the server name to
  contain `[bots-ok]` (case-insensitive). The host checks its own name
  (`sub_40A4C0()`, `byte_5D4594[1324]`). A client checks the name received in
  opcode `0xB0` (`sub_416590(0)+9` = `byte_5D4594[371389]`, GAME2.c:52317).
  The client never reads the engine copy at `byte_5D4594[371389]` for the gate.
  That copy persists across sessions, so a stale `[bots-ok]` name could leak
  into the next server. Instead, `GAME2.c` forwards each fresh `0xB0` to
  `nox_pt_note_server_name()`, which keeps a hook-owned snapshot. The snapshot
  is cleared when the player returns to the menu shell from a map, or when the
  game leaves MP. The gate fails closed until a new `0xB0` arrives.
  Nox server names are at most 15 chars, so the tag uses 9 of them.
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

Still unverified:
- The `cd_remaining` countdown (no learned ability was available). SLOT/self-cast
  is not exercised yet.
- Death (`hp_zero`, death `anim`) and `observer`: not reached.
- Wizard/Conjurer class values. `class_flags` bit names other than 4 (player), 2 (monster/NPC) and
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
- **Open issue: the Quest object list is incomplete.** `PT_MAX_WALK` (2048) still
  caps the exported object walk. On Quest stage 1, `obj_truncated` is set in
  every packet. Only NPCs (`0x200002`) and one `0x180000` were ever reported,
  and an item lying at the player's feet (on-screen "USE" marker) was not
  listed. The hook is blind to objects beyond the first 2048 list nodes.
- Death and observer: still not reached (no monsters met in about 3 minutes of
  Dismal Swamp wandering).

## 6. TODO

- `[bot] ` player-name prefix: not done. The name lives in playerInfo+4704
  (wchar, GAME2.c:19544) and is chosen before join (character creation and
  the `.plr` load). The injection point in the join packet is still to be located.
- Ally HP% for objects (`0x41/0x42` to `sub_495150`/`sub_49A650`); `hp_pct` is always 0xFF for now.
- Invisibility bit 0 / infravision bit 21 are read from renderer code; confirm them in a dump. Allow invisible allies once a client-only team test exists.
- Session-boundary heuristic for clearing the server-name snapshot (menu shell after a map): confirm that `0xB0` is resent after each join.
- Gear and inventory fields (playerInfo +4 / +2328).
