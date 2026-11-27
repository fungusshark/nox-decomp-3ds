# Agent & Contributor Guidelines (`nox-decomp-3ds`)

## 1. Real 3DS Hardware First vs. Emulator Role

- **Real Hardware = Source of Truth:** All frame-pacing (`PDC0`/`PDC1` VBlank), CPU/cache optimization (`Old 3DS 268 MHz` no-L2 & `New 3DS 804 MHz` 2 MB L2), unaligned memory access (`ARM11` `Data Abort`), SDMC (`fs:USER`), and `soc:u`/`http:C` Wi-Fi changes **must** target real 3DS hardware. Read [`docs/3DS_HARDWARE_LEARNINGS.md`](docs/3DS_HARDWARE_LEARNINGS.md) and [`docs/CONSOLE_DECOMP_PORTING_GUIDE.md`](docs/CONSOLE_DECOMP_PORTING_GUIDE.md) before modifying `port/3ds/` or hot loops in `src/`.
- **WSL / Windows Azahar Emulator = Fast Smoke & UI Layout Checks:** Emulators use HLE sockets/TLS/filesystems and forgiving host CPUs. Use Azahar for automated boot checks, overnight scripted runs, and `400×240` / `320×240` UI layout & screenshot inspection (`./scripts/verify-3ds.sh ui`). **Never close a hardware, FPS, or `soc:u` networking issue based solely on emulator behavior.**
- **Mandatory ARM11 Safety Invariants:**
  - Keep `-fsigned-char -fno-tree-vectorize`, `__stacksize__ = 1 MB`, and `memcpy`-based `nox_read_*` / `nox_write_*` helpers on byte-packed structs and network packets.
  - Keep `citro3d` at `60.0f` with Top-screen `VBlank0` pacing (`C3D_FrameCounter(0)` + `C3D_FrameBegin(0)`), and flush caches (`GSPGPU_FlushDataCache` / `DSP_FlushDataCache`) before DMA.

---

## 2. Tiered Build & Verification (`./scripts/verify-3ds.sh`)

Pick the lightest tier that matches your change to keep iteration fast:

1. **Tier 1 — `quick` (Default iterative compile gate):**
   ```bash
   ./scripts/verify-3ds.sh quick
   ```
   Compiles `build-3ds/nox.3dsx` (and host `build/` if configured). Run before every commit.
2. **Tier 2 — `ui` (UI, font, menu, or boot flow changes):**
   ```bash
   ./scripts/verify-3ds.sh ui [wait_seconds=8] [out_png=/tmp/nox_3ds_ui.png]
   ```
   Builds `nox.3dsx`, launches it in WSL Azahar (if installed; see `./scripts/setup-azahar-dev.sh`), captures a screenshot, and exits cleanly for visual inspection.
3. **Tier 3 — `prebuilt` (Hardware test handoff & shared pre-releases):**
   ```bash
   ./scripts/publish-3ds-prebuilt.sh <v0.2.X-branch-slug> [--publish] [--notes "..."]
   ```
   Stamps `NOX_3DS_VERSION` into `nox.smdh` + UI banner, stages `nox-<tag>.3dsx` locally (`~/nox_prebuilts/` & `/mnt/c/3ds-dev/` when present), and with `--publish` uploads a GitHub Pre-release `.3dsx` asset.

---

## 3. Branching, Stacked Changes & Prebuilt Collaboration

- **Feature & Fix Branches:** Branch `feat/<topic>` or `fix/<topic>` off `main` (or stack cleanly onto an active `feat/*` branch when iterating on dependent changes).
- **Prebuilt -> Hardware Verify -> Merge / Fast-Forward:** Push a tagged pre-release `.3dsx` from the `feat/*` or `fix/*` branch so collaborators can verify on real 3DS hardware, then merge or fast-forward into `main` (and fast-forward downstream stacked branches).
- **Decoupled Author -> Reviewer -> Scripted Verifier Pipeline:**
  - Keep **Author** subagents (`Workspace: "share"`) lean (code + `./scripts/verify-3ds.sh quick` only — never dump raw `objdump -d` or debug X11 sessions inside the Author).
  - Run an **Independent Reviewer** subagent (fresh context inside the local repo) on `git diff origin/main...feat/<topic>` to audit `#ifdef __3DS__` vs. `#else` math equivalence (fuzzing LUT/SWAR/fixed-point rewrites against the `#else` reference), ARM11 alignment safety, and compact `./scripts/audit-3ds-asm.py build-3ds/nox.elf <sym...>` output before publishing prebuilts.
  - Run Azahar screenshot recipes (`./scripts/azahar-play.py`), inline `_screenshots` uploads (`./scripts/upload-gh-screenshots.py`), and `.3dsx`+`.elf.gz` staging (`./scripts/publish-3ds-prebuilt.sh`) via scripts or a dedicated lightweight verifier step.

---

## 4. GitHub Issues & Investigation Tags

- Use GitHub Issues to centralize parallel investigations and design Q&A across sessions and collaborators.
- Standard title prefixes for investigations:
  - `[Investigate: HW-3DS]`, `[Investigate: O3DS-268MHz]`, `[Investigate: Perf]`, `[Investigate: Net/SOCU]`, `[Investigate: UI/PICA200]`, `[Investigate: Port-HAL/Switch]`
- Always pass explicit `--json` fields to `gh` CLI queries to avoid deprecated GraphQL `projectCards` errors:
  ```bash
  gh issue list --state all --limit 30 --json number,title,state
  gh issue view <num> --json number,title,state,body,comments
  ```
- **Large Inline Screenshots on Issues/PRs (Never Force Downloads or Tiny Multi-Column Tables):**
  - **Never** embed `https://github.com/.../releases/download/.../*.png` URLs for screenshots (`Content-Disposition: attachment` forces a file download when clicked instead of opening GitHub's zoomable lightbox).
  - Instead, push screenshots to the isolated orphan `_screenshots` branch via `./scripts/upload-gh-screenshots.py issue-<num> <file1.png> ...` (zero `main` bloat) and embed them **stacked vertically at full width (`width="720"`)** (or at most a 2-row stacked **Before** above **After** comparison — never cramped 3–4 column tables) so `400×240` / `320×240` UI typography is immediately readable inline.
