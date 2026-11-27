#!/usr/bin/env bash
set -euo pipefail

# Build a version-stamped nox.3dsx prebuilt, stage locally, and optionally publish a GitHub Pre-release.
# Usage: ./scripts/publish-3ds-prebuilt.sh <v0.2.X-slug> [--publish] [--notes "Release summary"]

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_TAG="${1:-}"
shift || true

if [[ -z "$VERSION_TAG" ]]; then
    echo "Usage: $0 <v0.2.X-slug> [--publish] [--notes \"...\"]" >&2
    exit 1
fi

PUBLISH=0
NOTES=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --publish) PUBLISH=1; shift ;;
        --notes)   NOTES="${2:-}"; shift 2 ;;
        *)         echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
PREBUILTS_DIR="${NOX_PREBUILTS_DIR:-${HOME}/nox_prebuilts}"
mkdir -p "$PREBUILTS_DIR"

echo "[prebuilt] Configuring & building Nox 3DS (${VERSION_TAG})..."
cmake -S "${REPO_ROOT}/port/3ds" -B "${REPO_ROOT}/build-3ds" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
    -DNOX_3DS_VERSION="${VERSION_TAG}"
cmake --build "${REPO_ROOT}/build-3ds" -j"${JOBS}"

STAGED_3DSX="${PREBUILTS_DIR}/nox-${VERSION_TAG}.3dsx"
cp -f "${REPO_ROOT}/build-3ds/nox.3dsx" "$STAGED_3DSX"
chmod +x "$STAGED_3DSX"
echo "[prebuilt] Staged: ${STAGED_3DSX} ($(du -h "$STAGED_3DSX" | cut -f1))"

if [[ -d "/mnt/c/3ds-dev" ]]; then
    cp -f "$STAGED_3DSX" "/mnt/c/3ds-dev/nox.3dsx"
    echo "[prebuilt] Updated Windows/WSL emulator target: /mnt/c/3ds-dev/nox.3dsx"
fi

if [[ "$PUBLISH" -eq 1 ]]; then
    cd "$REPO_ROOT"
    BRANCH="$(git rev-parse --abbrev-ref HEAD)"
    SHA="$(git rev-parse --short HEAD)"
    if [[ -z "$NOTES" ]]; then
        NOTES="Prebuilt \`.3dsx\` from branch \`${BRANCH}\` (\`${SHA}\`) for real-3DS hardware verification."
    fi
    if gh release view "$VERSION_TAG" >/dev/null 2>&1; then
        echo "[prebuilt] Uploading asset to existing pre-release ${VERSION_TAG}..."
        gh release upload "$VERSION_TAG" "${STAGED_3DSX}#nox-${VERSION_TAG}.3dsx" --clobber
    else
        echo "[prebuilt] Creating GitHub Pre-release ${VERSION_TAG} (target=${SHA})..."
        gh release create "$VERSION_TAG" \
            "${STAGED_3DSX}#nox-${VERSION_TAG}.3dsx" \
            --target "$(git rev-parse HEAD)" \
            --title "Nox 3DS ${VERSION_TAG}" \
            --notes "$NOTES" \
            --prerelease
    fi
fi
