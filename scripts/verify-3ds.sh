#!/usr/bin/env bash
set -euo pipefail

# Tiered verification runner for nox-decomp-3ds:
#   ./scripts/verify-3ds.sh quick                        -> Compile 3DS + ELF invariant gate (+ host build if present)
#   ./scripts/verify-3ds.sh ui [wait_sec=8] [out.png]    -> Compile + ELF gate + WSL Azahar boot & screenshot capture
#   ./scripts/verify-3ds.sh play <subcommand...>         -> Directed Azahar X11/XTest play/screenshot driver
#   ./scripts/verify-3ds.sh prebuilt <tag> [--publish]   -> Build versioned .3dsx + .elf(.gz) + stage/publish pre-release

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-quick}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-3ds}"

build_3ds_and_host() {
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        if [[ ! -f "/opt/devkitpro/cmake/3DS.cmake" ]]; then
            echo "ERROR: devkitPro 3DS toolchain not found at /opt/devkitpro/cmake/3DS.cmake" >&2
            exit 1
        fi
        cmake -S "${REPO_ROOT}/port/3ds" -B "${BUILD_DIR}" \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
            ${CMAKE_EXTRA_ARGS:-}
    else
        # Clear any sticky release override so git describe reflects the active branch
        cmake -S "${REPO_ROOT}/port/3ds" -B "${BUILD_DIR}" -UNOX_3DS_VERSION ${CMAKE_EXTRA_ARGS:-} >/dev/null
    fi
    echo "[verify:quick] Building 3DS target (${BUILD_DIR}/nox.3dsx)..."
    cmake --build "${BUILD_DIR}" -j"${JOBS}"

    # Run fast static ELF/3DSX/SMDH post-build invariant check (<0.5s)
    python3 "${REPO_ROOT}/scripts/check-3ds-elf.py" --build-dir "${BUILD_DIR}"

    for host_dir in build-linux build; do
        if [[ -f "${REPO_ROOT}/${host_dir}/CMakeCache.txt" ]]; then
            echo "[verify:quick] Building host target (${host_dir})..."
            cmake --build "${REPO_ROOT}/${host_dir}" -j"${JOBS}"
            break
        fi
    done
    ls -lh "${BUILD_DIR}/nox.3dsx" "${BUILD_DIR}/nox.elf"
}

case "$MODE" in
    quick)
        build_3ds_and_host
        ;;
    ui|emu)
        WAIT_SEC="${2:-8}"
        OUT_PNG="${3:-/tmp/nox_3ds_ui.png}"
        build_3ds_and_host

        AZAHAR_BIN="${AZAHAR_BIN:-$(command -v azahar || true)}"
        if [[ -z "$AZAHAR_BIN" ]]; then
            echo "[verify:ui] SKIP emulator run: 'azahar' not in PATH (run ./scripts/setup-azahar-dev.sh to install)." >&2
            exit 0
        fi

        mkdir -p "$(dirname "$OUT_PNG")"
        SESSION_ID="ui_$$"
        echo "[verify:ui] Launching ${BUILD_DIR}/nox.3dsx in isolated Azahar session (${WAIT_SEC}s)..."

        run_isolated_ui() {
            trap 'python3 "${REPO_ROOT}/scripts/azahar-play.py" --session "${SESSION_ID}" stop >/dev/null 2>&1 || true' EXIT RETURN
            python3 "${REPO_ROOT}/scripts/azahar-play.py" --session "${SESSION_ID}" --timeout "$((WAIT_SEC + 25))" \
                start "${BUILD_DIR}/nox.3dsx" --wait-boot "${WAIT_SEC}" --out "${OUT_PNG}" --isolated-sdmc
            python3 "${REPO_ROOT}/scripts/azahar-play.py" --session "${SESSION_ID}" stop >/dev/null 2>&1 || true
            trap - EXIT RETURN
        }

        if [[ "${USE_XVFB:-0}" == "1" || -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
            xvfb-run -a -s "-screen 0 1024x768x24" bash -c "$(declare -f run_isolated_ui); REPO_ROOT='$REPO_ROOT' BUILD_DIR='$BUILD_DIR' WAIT_SEC='$WAIT_SEC' OUT_PNG='$OUT_PNG' SESSION_ID='$SESSION_ID' run_isolated_ui"
        else
            run_isolated_ui
        fi
        echo "[verify:ui] Completed -> ${OUT_PNG}"
        ;;
    play)
        shift || true
        exec python3 "${REPO_ROOT}/scripts/azahar-play.py" "$@"
        ;;
    prebuilt|release)
        shift || true
        exec "${REPO_ROOT}/scripts/publish-3ds-prebuilt.sh" "$@"
        ;;
    *)
        echo "Usage: $0 {quick|ui [wait_sec] [out.png]|play <args...>|prebuilt <vX.Y.Z-tag> [--publish]}" >&2
        exit 1
        ;;
esac
