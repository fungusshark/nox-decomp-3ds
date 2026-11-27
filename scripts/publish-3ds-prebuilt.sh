#!/usr/bin/env bash
set -euo pipefail

# Build a version-stamped nox.3dsx + nox.elf prebuilt, verify ELF/SMDH invariants,
# archive both .3dsx and .elf(.gz) locally (for Luma3DS crash dump decoding),
# and optionally publish a GitHub Pre-release with optional screenshot assets.
#
# Usage:
#   ./scripts/publish-3ds-prebuilt.sh <v0.2.X-slug> [--publish] [--notes "..."] [--asset <file.png>]...

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_TAG="${1:-}"
shift || true

if [[ -z "$VERSION_TAG" ]]; then
    echo "Usage: $0 <v0.2.X-slug> [--publish] [--notes \"...\"] [--asset <path>]..." >&2
    exit 1
fi

PUBLISH=0
NOTES=""
EXTRA_ASSETS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --publish) PUBLISH=1; shift ;;
        --notes)   NOTES="${2:-}"; shift 2 ;;
        --asset)   EXTRA_ASSETS+=("${2:-}"); shift 2 ;;
        *)         echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-3ds}"
PREBUILTS_DIR="${NOX_PREBUILTS_DIR:-${HOME}/nox_prebuilts}"
mkdir -p "$PREBUILTS_DIR"

echo "[prebuilt] Configuring & building Nox 3DS (${VERSION_TAG}) in ${BUILD_DIR}..."
cmake -S "${REPO_ROOT}/port/3ds" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
    -DNOX_3DS_VERSION="${VERSION_TAG}"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

# Post-build invariant gate: verify unstripped DWARF ELF, 1 MB stack, 3DSX magic & SMDH version tag
python3 "${REPO_ROOT}/scripts/check-3ds-elf.py" \
    --build-dir "${BUILD_DIR}" \
    --expect-version "${VERSION_TAG}"

STAGED_3DSX="${PREBUILTS_DIR}/nox-${VERSION_TAG}.3dsx"
STAGED_ELF="${PREBUILTS_DIR}/nox-${VERSION_TAG}.elf"
STAGED_ELF_GZ="${PREBUILTS_DIR}/nox-${VERSION_TAG}.elf.gz"

cp -f "${BUILD_DIR}/nox.3dsx" "$STAGED_3DSX"
chmod +x "$STAGED_3DSX"
cp -f "${BUILD_DIR}/nox.elf" "$STAGED_ELF"
gzip -c -6 "${BUILD_DIR}/nox.elf" > "$STAGED_ELF_GZ"

SHA_3DSX="$(sha256sum "$STAGED_3DSX" | awk '{print $1}')"
SHA_ELF="$(sha256sum "$STAGED_ELF" | awk '{print $1}')"
echo "[prebuilt] Staged 3DSX : ${STAGED_3DSX} ($(du -h "$STAGED_3DSX" | cut -f1)) sha256=${SHA_3DSX}"
echo "[prebuilt] Staged ELF  : ${STAGED_ELF} ($(du -h "$STAGED_ELF" | cut -f1)) / .gz ($(du -h "$STAGED_ELF_GZ" | cut -f1))"

if [[ -d "/mnt/c/3ds-dev" ]] && [[ "${NOX_3DS_STAGE_WIN_DEV:-1}" == "1" ]]; then
    cp -f "$STAGED_3DSX" "/mnt/c/3ds-dev/nox.3dsx"
    echo "[prebuilt] Updated Windows/WSL emulator target: /mnt/c/3ds-dev/nox.3dsx"
fi

if [[ "$PUBLISH" -eq 1 ]]; then
    cd "$REPO_ROOT"
    BRANCH="$(git rev-parse --abbrev-ref HEAD)"
    SHA="$(git rev-parse --short HEAD)"
    if [[ -z "$NOTES" ]]; then
        NOTES="Prebuilt \`.3dsx\` + debug \`.elf.gz\` from branch \`${BRANCH}\` (\`${SHA}\`) for real-3DS hardware verification.

- \`nox-${VERSION_TAG}.3dsx\` SHA-256: \`${SHA_3DSX}\`
- \`nox-${VERSION_TAG}.elf\` SHA-256: \`${SHA_ELF}\` *(decode Luma3DS crash dumps via \`./scripts/decode-3ds-dump.py <dump.dmp> nox-${VERSION_TAG}.elf.gz\`)*"
    fi

    UPLOAD_ARGS=(
        "${STAGED_3DSX}#nox-${VERSION_TAG}.3dsx"
        "${STAGED_ELF_GZ}#nox-${VERSION_TAG}.elf.gz"
    )
    for asset in "${EXTRA_ASSETS[@]}"; do
        if [[ -f "$asset" ]]; then
            UPLOAD_ARGS+=("$asset")
        fi
    done

    if gh release view "$VERSION_TAG" --json tagName >/dev/null 2>&1; then
        echo "[prebuilt] Uploading assets to existing pre-release ${VERSION_TAG}..."
        gh release upload "$VERSION_TAG" "${UPLOAD_ARGS[@]}" --clobber
    else
        echo "[prebuilt] Creating GitHub Pre-release ${VERSION_TAG} (target=${SHA})..."
        gh release create "$VERSION_TAG" \
            "${UPLOAD_ARGS[@]}" \
            --target "$(git rev-parse HEAD)" \
            --title "Nox 3DS ${VERSION_TAG}" \
            --notes "$NOTES" \
            --prerelease
    fi
fi
