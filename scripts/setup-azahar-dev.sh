#!/usr/bin/env bash
set -euo pipefail

# Optional local developer setup for WSL2 Azahar AppImage + shared /mnt/c/3ds-dev directory.
# If console-port-kit is installed locally, delegates directly; otherwise runs self-contained install.

KIT_SCRIPT="${HOME}/github-giraffepus/console-port-kit/plugins/console-port-kit/skills/wsl-3ds-emulator-harness/scripts/install-wsl-azahar.sh"
if [[ -x "$KIT_SCRIPT" ]]; then
    exec "$KIT_SCRIPT" "$@"
fi

INSTALL_DIR="${HOME}/.local/share/azahar-emu"
DEPS_DIR="${HOME}/.local/lib/azahar-deps"
BIN_DIR="${HOME}/.local/bin"
mkdir -p "$INSTALL_DIR" "$DEPS_DIR" "$BIN_DIR" "${HOME}/nox_prebuilts"

if [[ -d "/mnt/c" ]]; then
    mkdir -p /mnt/c/3ds-dev/sdmc/3ds/nox
    [[ ! -e "${INSTALL_DIR}/sdmc" ]] && ln -s /mnt/c/3ds-dev/sdmc "${INSTALL_DIR}/sdmc"
fi

if [[ ! -x "${INSTALL_DIR}/azahar.AppImage" ]]; then
    APPIMAGE_URL="$(python3 - << 'PY'
import json, urllib.request
req = urllib.request.Request(
    "https://api.github.com/repos/azahar-emu/azahar/releases/latest",
    headers={"User-Agent": "nox-decomp-3ds-setup/1.0"}
)
with urllib.request.urlopen(req, timeout=15) as resp:
    rel = json.loads(resp.read().decode("utf-8"))
for a in rel.get("assets", []):
    name = a.get("name", "")
    if name.endswith(".AppImage"):
        print(a["browser_download_url"])
        break
PY
)"
    curl -fL "$APPIMAGE_URL" -o "${INSTALL_DIR}/azahar.AppImage"
    chmod +x "${INSTALL_DIR}/azahar.AppImage"
fi

if command -v apt-get >/dev/null 2>&1 && command -v dpkg-deb >/dev/null 2>&1; then
    TMP_DEB="$(mktemp -d)"
    trap 'rm -rf "$TMP_DEB"' EXIT
    (
        cd "$TMP_DEB"
        for pkg in libopengl0 libjack-jackd2-0 libusb-1.0-0; do
            apt-get download "$pkg" >/dev/null 2>&1 || true
        done
        for deb in ./*.deb; do
            [[ -f "$deb" ]] && dpkg-deb -x "$deb" "$DEPS_DIR"
        done
    )
fi

cat << 'EOF' > "${BIN_DIR}/azahar"
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${HOME}/.local/lib/azahar-deps/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
if [[ -e /dev/fuse ]]; then
    exec "${HOME}/.local/share/azahar-emu/azahar.AppImage" "$@"
else
    exec "${HOME}/.local/share/azahar-emu/azahar.AppImage" --appimage-extract-and-run "$@"
fi
EOF
chmod +x "${BIN_DIR}/azahar"
echo "[setup-azahar-dev] Ready: ${BIN_DIR}/azahar (prebuilts dir: ${HOME}/nox_prebuilts)"
