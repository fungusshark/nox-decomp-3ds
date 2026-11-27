#!/usr/bin/env bash
set -euo pipefail

# Tiered verification runner for nox-decomp-3ds:
#   ./scripts/verify-3ds.sh quick                      -> Compile 3DS (+ host Linux build if configured)
#   ./scripts/verify-3ds.sh ui [wait_sec=8] [out.png]  -> Compile + WSL Azahar boot & screenshot capture
#   ./scripts/verify-3ds.sh prebuilt <tag> [--publish] -> Build versioned .3dsx + stage/publish pre-release

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-quick}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

build_3ds_and_host() {
    if [[ ! -f "${REPO_ROOT}/build-3ds/CMakeCache.txt" ]]; then
        if [[ ! -f "/opt/devkitpro/cmake/3DS.cmake" ]]; then
            echo "ERROR: devkitPro 3DS toolchain not found at /opt/devkitpro/cmake/3DS.cmake" >&2
            exit 1
        fi
        cmake -S "${REPO_ROOT}/port/3ds" -B "${REPO_ROOT}/build-3ds" \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake
    else
        # Clear any sticky release override so git describe reflects the active branch
        cmake -S "${REPO_ROOT}/port/3ds" -B "${REPO_ROOT}/build-3ds" -UNOX_3DS_VERSION >/dev/null
    fi
    echo "[verify:quick] Building 3DS target (build-3ds/nox.3dsx)..."
    cmake --build "${REPO_ROOT}/build-3ds" -j"${JOBS}"

    for host_dir in build-linux build; do
        if [[ -f "${REPO_ROOT}/${host_dir}/CMakeCache.txt" ]]; then
            echo "[verify:quick] Building host target (${host_dir})..."
            cmake --build "${REPO_ROOT}/${host_dir}" -j"${JOBS}"
            break
        fi
    done
    ls -lh "${REPO_ROOT}/build-3ds/nox.3dsx"
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
        echo "[verify:ui] Launching build-3ds/nox.3dsx in Azahar for ${WAIT_SEC}s..."

        run_capture() {
            "$AZAHAR_BIN" "${REPO_ROOT}/build-3ds/nox.3dsx" >/tmp/nox_azahar.log 2>&1 &
            local pid=$!
            trap 'kill "$pid" >/dev/null 2>&1 || true' RETURN
            sleep "$WAIT_SEC"
            if ! kill -0 "$pid" >/dev/null 2>&1; then
                echo "ERROR: Azahar exited early! Last log lines:" >&2
                tail -n 40 /tmp/nox_azahar.log >&2 || true
                return 1
            fi
            if command -v scrot >/dev/null 2>&1; then
                scrot -o "$OUT_PNG"
            elif command -v import >/dev/null 2>&1; then
                import -window root "$OUT_PNG"
            else
                echo "[verify:ui] WARN: install 'scrot' to save PNG screenshots; boot smoke OK." >&2
            fi
            kill "$pid" >/dev/null 2>&1 || true
            wait "$pid" >/dev/null 2>&1 || true
            trap - RETURN
        }

        if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
            xvfb-run -a -s "-screen 0 1024x768x24" bash -c "$(declare -f run_capture); AZAHAR_BIN='$AZAHAR_BIN' REPO_ROOT='$REPO_ROOT' WAIT_SEC='$WAIT_SEC' OUT_PNG='$OUT_PNG' run_capture"
        else
            run_capture
        fi
        echo "[verify:ui] Completed -> ${OUT_PNG}"
        ;;
    prebuilt|release)
        shift || true
        exec "${REPO_ROOT}/scripts/publish-3ds-prebuilt.sh" "$@"
        ;;
    *)
        echo "Usage: $0 {quick|ui [wait_sec] [out.png]|prebuilt <vX.Y.Z-tag> [--publish]}" >&2
        exit 1
        ;;
esac
