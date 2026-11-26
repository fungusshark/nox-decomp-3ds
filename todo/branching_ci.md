# Git & Release Workflow (Trunk-Based GitHub Flow)

We follow a **trunk-based workflow** inspired by `google3`/Perforce CLs, adapted for GitHub. `main` is our single source of truth and must remain buildable and playable at all times.

---

### 1. Branching & Feature Development
* **Short-Lived Feature Branches:** Create focused branches off `main` for single, atomic tasks (e.g., `feat/3ds-software-blitter`, `fix/wnd-ui-parser`). Keep branches small (1–3 days of work max).
* **Feature Flags over Long-Lived Branches:** For large, multi-stage features (e.g., new platform backends or renderers), merge incremental progress into `main` behind compile-time `#ifdef` flags or runtime config toggles rather than maintaining divergent branches.
* **No Retail Assets in Git:** Never commit original *Nox* proprietary files (`.bag`, `.map`, `.mp3`, etc.). Keep all retail data in `data/` (gitignored) and commit only source code, build scripts, and extraction/conversion tools.

---

### 2. Pre-Merge Verification (PRs)
* **Build & Test on the Feature Branch First:** Compile and verify the feature branch locally (or via the PR CI build artifact) before merging into `main`.
* **Sync with `main` Before Merging:** If other PRs merged while you were working, rebase onto `main` (`git fetch origin && git rebase origin/main`) and run a quick smoke test to verify there are no integration regressions.
* **Squash and Merge:** Merge PRs into `main` via **Squash and Merge** with a clear, descriptive commit message so `main` maintains a clean, linear history of atomic changes.

---

### 3. CI & Automated Releases
* **PR Builds (Feature Verification):** Every Pull Request triggers CI to compile the project and attach temporary test artifacts (`.3dsx`, `.nro`, `.exe`, etc.) for isolated feature testing.
* **`main` Auto-Release Builder (Rolling Dev Build):** Every merge into `main` automatically triggers CI to build the integrated project and update a rolling GitHub Pre-Release (`latest-dev` / `nightly`) with fresh binaries. Testers should always pull integrated builds from `main` / `latest-dev`, never from feature branches.
* **Milestone Releases:** Formal versioned releases (e.g., `v0.1.0`) are cut strictly from tagged commits on `main`.

---

### 4. Concrete GitHub Actions Roadmap (`.github/workflows/build.yml`)

#### Current State & Gaps
The inherited workflow in [`.github/workflows/build.yml`](../.github/workflows/build.yml) comes from upstream `sookyboo/nox-decomp`:
1. **No `pull_request` trigger:** Currently runs only on `workflow_dispatch`, `push` to `main`, and `push` tags `v*` — PR authors do not get automated build verification or test artifacts before merging.
2. **No Nintendo 3DS job:** Builds Linux `armhf`, Linux `i386`, Win32, Flatpak, and PortMaster packages via Docker, plus GHCR server images, but does not cross-compile `port/3ds` into `nox.3dsx`.
3. **No rolling `latest-dev` pre-release:** The `release` job runs strictly on `refs/tags/v*`; merges to `main` only retain workflow run artifacts without updating a canonical tester download link.

#### Planned Extensions
1. **PR Verification Gate (`on: pull_request: branches: [main]`):**
   * Enable `pull_request` builds targeting `main` across targets, while keeping `publish-server` and release-publishing jobs gated on `github.event_name == 'push'` (and never exposing secrets on forked PRs).
2. **Fast 3DS Build Job (`build-3ds`):**
   * Run in the `devkitpro/devkitarm:latest` container using Ninja + `ccache` cached via `actions/cache`:
     ```bash
     cmake -S port/3ds -B build-3ds -G Ninja \
       -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
       -DCMAKE_BUILD_TYPE=Release
     ninja -C build-3ds
     ```
   * Upload `build-3ds/nox.3dsx` and `build-3ds/nox.smdh` (or `nox-3ds-${RELEASE_SUFFIX}.zip`) via `actions/upload-artifact` on every PR and `main` push (~1–2 min turnaround).
3. **Rolling `latest-dev` Pre-Release Job (`release-dev`):**
   * Triggered on `github.event_name == 'push' && github.ref == 'refs/heads/main'` after `build` and `build-3ds` succeed (`permissions: contents: write`).
   * Create or update a single rolling GitHub Pre-Release (`latest-dev`):
     * Move/update the `latest-dev` tag to the current `main` commit and publish with `gh release create latest-dev --prerelease ... || gh release upload latest-dev --clobber ...`.
     * Stamp release notes with the short commit SHA, timestamp, and recent commit summary, bundling `nox.3dsx` alongside Desktop/PortMaster builds.
4. **Tagged Milestone Releases (`v*`):**
   * Extend the existing `release` job to download the `build-3ds` artifact and attach `nox-3ds-${RELEASE_SUFFIX}.zip` (`nox.3dsx` + SD card placement notes `/3ds/nox/nox.3dsx`) alongside the Win32, Linux, Flatpak, PortMaster, and `SHA256SUMS` assets.
