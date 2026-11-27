#!/usr/bin/env python3
"""
Fast post-build invariant verifier for Nox 3DS (`nox.elf`, `nox.3dsx`, `nox.smdh`).

Checks:
  1. `nox.elf` exists, is 32-bit ARM ELF, retains `.symtab` + `.debug_*` for crash decoding,
     and defines `__stacksize__` >= 1 MB (0x100000).
  2. `nox.3dsx` exists and starts with valid `3DSX` magic.
  3. `nox.smdh` (when present) starts with `SMDH` magic and embeds `--expect-version`
     so the 3DS Homebrew Launcher displays the exact build version tag.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path


def check_elf(elf_path: Path, min_stack_bytes: int = 0x100000) -> list[str]:
    errors = []
    if not elf_path.is_file():
        return [f"Missing ELF binary: {elf_path}"]

    hdr = elf_path.read_bytes()[:20]
    if len(hdr) < 20 or hdr[:4] != b"\x7fELF":
        return [f"Invalid ELF magic in {elf_path}"]
    ei_class, ei_data = hdr[4], hdr[5]
    e_machine = struct.unpack("<H", hdr[18:20])[0]
    if ei_class != 1 or ei_data != 1 or e_machine != 40:
        errors.append(f"Unexpected ELF target (class={ei_class}, data={ei_data}, machine={e_machine}; expected 32-bit LE ARM=40)")

    # Check sections via readelf -S for DWARF debug info & symbol table
    readelf = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-readelf"
    nm = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
    objdump = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump"

    sec_out = subprocess.check_output([readelf, "-S", str(elf_path)], text=True)
    if ".symtab" not in sec_out:
        errors.append("ELF is stripped (.symtab missing) — required for Luma3DS crash dump symbolication")
    if ".debug_info" not in sec_out and ".debug_line" not in sec_out:
        errors.append("ELF lacks DWARF debug sections (.debug_info/.debug_line) — keep -g enabled")

    # Verify __stacksize__ >= 1 MB
    nm_out = subprocess.check_output([nm, "-S", "--defined-only", str(elf_path)], text=True)
    stack_sym_addr = None
    for line in nm_out.splitlines():
        parts = line.split()
        if parts and parts[-1] == "__stacksize__":
            stack_sym_addr = int(parts[0], 16)
            break

    if stack_sym_addr is None:
        errors.append("Missing `__stacksize__` symbol in ELF (must be >= 1 MB on 3DS)")
    else:
        # Read 4-byte value of __stacksize__ from .data/.rodata via objdump -s
        dump = subprocess.check_output(
            [objdump, "-s", f"--start-address={hex(stack_sym_addr)}", f"--stop-address={hex(stack_sym_addr + 4)}", str(elf_path)],
            text=True,
        )
        val = None
        for line in dump.splitlines():
            line = line.strip()
            if line.startswith(f"{stack_sym_addr:x}"):
                hex_word = line.split()[1]
                val = struct.unpack("<I", bytes.fromhex(hex_word))[0]
                break
        if val is not None and val < min_stack_bytes:
            errors.append(f"`__stacksize__` is {val} bytes (0x{val:x}), below mandatory {min_stack_bytes} bytes (1 MB)")

    return errors


def check_3dsx_and_smdh(p3dsx: Path, psmdh: Path | None, expect_version: str) -> list[str]:
    errors = []
    if not p3dsx.is_file():
        errors.append(f"Missing 3DSX binary: {p3dsx}")
    else:
        head = p3dsx.read_bytes()[:32]
        if len(head) < 32 or head[:4] != b"3DSX":
            errors.append(f"Invalid 3DSX header magic in {p3dsx}")

    if psmdh and psmdh.is_file():
        raw = psmdh.read_bytes()
        if len(raw) < 0x2008 or raw[:4] != b"SMDH":
            errors.append(f"Invalid SMDH header magic/size in {psmdh}")
        elif expect_version:
            # English title block starts at offset 0x8 + 1 * 0x200 = 0x208 (UTF-16LE, 0x200 bytes)
            eng_block = raw[0x208:0x408].decode("utf-16-le", errors="ignore")
            if expect_version not in eng_block:
                errors.append(
                    f"SMDH English title block does not contain expected version '{expect_version}' "
                    f"(got: {eng_block.replace(chr(0), ' ').strip()!r})"
                )
    elif expect_version:
        errors.append(f"SMDH file not found at {psmdh} while verifying version '{expect_version}'")

    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify Nox 3DS ELF/3DSX/SMDH post-build invariants")
    ap.add_argument("--build-dir", default="build-3ds", help="Path to 3DS CMake build directory")
    ap.add_argument("--expect-version", default="", help="Expected version string embedded in nox.smdh")
    args = ap.parse_args()

    bdir = Path(args.build_dir)
    elf = bdir / "nox.elf"
    p3dsx = bdir / "nox.3dsx"
    psmdh = bdir / "nox.smdh"

    errs = check_elf(elf) + check_3dsx_and_smdh(p3dsx, psmdh, args.expect_version)
    if errs:
        print("[check-3ds-elf] FAILED:", file=sys.stderr)
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1

    ver_msg = f", smdh_version='{args.expect_version}'" if args.expect_version else ""
    print(f"[check-3ds-elf] OK ({elf.name} symbols+DWARF+1MB stack verified, {p3dsx.name} valid{ver_msg})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
