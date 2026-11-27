# Playtest game-state hook (dev only, issue #36)

A small UDP bridge that lets a host-side bot read the local player's state and
the client's visible objects, and send pad-equivalent commands. It replaces
screenshot perception for automated playtesting. It is **not** part of release
builds.

Files:
- `port/3ds/playtest_hook_3ds.c` / `.h`: the hook (wire structs live in the header).
- `scripts/playtest_hook_client.py`: stdlib host client (`listen`, `send`, `--selftest`).
- Call sites: `src/win.c` `sub_4453A0` (right after `nox_gamepad_update()`),
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

Under Azahar, the guest's HLE sockets map to host sockets, so `host=127.0.0.1`
should reach WSL (**UNVERIFIED**: smoke-test this). On hardware, use the PC's LAN IP.

If `socInit`, `socket`, `O_NONBLOCK` or `bind` fails, the hook logs one line
via `nox_3ds_net_log` (`[pt] ...`) and disables itself. `sendto` errors are
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
| 56 | 5 x 8 B | abilities: `u8 id, u8 ready, u8 active, u8 0, u16 cd_remaining, u16 cd_total` | record `1047764 + 24*a` (a = 1..5): id +0, ready +8, active mask +12, start frame +20; total = `sub_4252D0(id)` |

`cd_remaining = cd_total - (game_frame - start)` while not ready, else 0.

Flags: `0x1` menu_shell, `0x2` map_loaded, `0x4` in_world
(`nox_3ds_input_is_world_mode()`: map loaded, no menu, modal, chat or dialog),
`0x8` is_mp (net mode WIFI), `0x10` is_host (`sub_40A5C0(1)`), `0x20`
player_valid, `0x40` observer (playerInfo+3680 bit0), `0x80` hp_zero, `0x100`
bots_ok, `0x200` cmds_active, `0x400` obj_truncated.

Object (20 B): `u16 net_id (+128), u16 type_id (+108), u32 class_flags (+112),
i32 x (+12), i32 y (+16), u8 facing (+297), u8 hp_pct (0xFF = unknown), u16 0`.

Objects come **only** from the client drawable list `byte_5D4594[1046572]`
(next = +368). That is the set the server chose to send. Entries with class
`& 0x20400000` (non-network, as skipped by `sub_45A6F0`) and the local player
are excluded. With `clip_viewport=1`, only objects inside the camera rect are
kept. The list is sorted players, then monsters, then missiles, then others,
nearest first, and capped at `max_objects`. The walk is bounded at 2048 nodes.

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
  Nox server names are at most 15 chars, so the tag uses 9 of them.
  State-out still runs without the tag. If the gate closes, all overrides drop
  and held keys are released.
- HP, mana and position are server-authoritative. The client only stores what
  it receives, and the hook never writes game state. Commands are the same
  control codes a pad produces, so the bot has a human's control power. Its
  edges are information (exact coordinates and cooldowns) and reaction time.
  Viewport clipping and the client-only object list limit the information edge.
- On the host, the server object list lives in the same process. The hook must
  never call `sub_4DA7xx`, `sub_4ECCB0`, `sub_518EE0` or the server branch of
  `sub_418C80`. Reviewers should grep for these.

## 5. UNVERIFIED fields (confirm in an Azahar field-dump session)

- `player_class` enum: 0 Warrior / 1 Wizard / 2 Conjurer (OpenNox convention).
- `anim` (drawable +276) values: 6 = running is seen in code; death and other values are unknown.
- `hp_zero` as the death signal: there is no explicit death bit. Opcode `0xE8` clears gear, and HP may read 0 before the first `0xDD`.
- `observer` = playerInfo+3680 bit0 (it gates the minimap show-all; the exact semantics are not yet confirmed).
- Ability records `1047764 + 24*a`: whether `a` is the slot or the ability id (the same for a fresh Warrior), and the `cd_remaining` derivation. That derivation assumes `game_frame` and `sub_4252D0` use the same 30 fps frame units.
- `class_flags` bit names other than `4` = player.
- Viewport rect from `sub_437250()` and the camera origin `[811084/811088]` under the 3DS 400x240 viewport.
- Guest-to-host `127.0.0.1` reachability under Azahar HLE soc; real-hardware frame cost (measure with `perf_3ds.h`).

## 6. TODO

- `[bot] ` player-name prefix: not done. The name lives in playerInfo+4704
  (wchar, GAME2.c:19544) and is chosen before join (character creation and
  the `.plr` load). The injection point in the join packet is still to be located.
- Ally HP% for objects (`0x41/0x42` to `sub_495150`/`sub_49A650`); `hp_pct` is always 0xFF for now.
- Optional invisibility-enchant filtering (drawable +124); the bit is unconfirmed.
- Gear and inventory fields (playerInfo +4 / +2328).
