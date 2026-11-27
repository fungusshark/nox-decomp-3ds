#!/usr/bin/env python3
"""
Luma3DS ARM11 Crash Dump (`.dmp`) Decoder & Symbolicator for `nox-decomp-3ds`.

Accepts a Luma3DS Rosalina crash dump (`sdmc:/luma/dumps/arm11/crash_dump_*.dmp`)
and a matching archived `nox-<tag>.elf` (or compressed `nox-<tag>.elf.gz`) from
`~/nox_prebuilts/` or GitHub Pre-releases, printing:
  - Exception type (Prefetch Abort / Data Abort / Undefined Instruction / SVC Break),
    processor registers (`R0..R12`, `SP`, `LR`, `PC`, `CPSR`, `DFSR`/`IFSR`, `FAR`),
  - Exact `file:line` & inlined function stack for `PC` and `LR`, and
  - Probable call-chain candidates scanned from the captured stack dump.

Usage:
  ./scripts/decode-3ds-dump.py <crash_dump_00000000.dmp> <nox-v0.2.X.elf[.gz]> [--base 0x00100000]
"""

import argparse
import gzip
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

EXCEPTION_TYPES = {
    0: "FIQ",
    1: "Undefined Instruction",
    2: "Prefetch Abort",
    3: "Data Abort (check unaligned VLDR/LDM/STRD or NULL deref!)",
}


def addr2line(elf_path: Path, addrs: list[int]) -> list[str]:
    if not addrs:
        return []
    tool = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line"
    if not Path(tool).is_file():
        tool = shutil.which("arm-none-eabi-addr2line") or "addr2line"
    cmd = [tool, "-f", "-i", "-p", "-e", str(elf_path)] + [hex(a) for a in addrs]
    out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    return [ln.strip() for ln in out.splitlines()]


def get_text_bounds(elf_path: Path, default_base: int) -> tuple[int, int]:
    readelf = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-readelf"
    if not Path(readelf).is_file():
        return default_base, default_base + 0x00800000
    out = subprocess.check_output([readelf, "-l", str(elf_path)], text=True)
    lo, hi = None, None
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD" and len(parts) >= 6:
            vaddr = int(parts[2], 16)
            memsz = int(parts[5], 16)
            lo = vaddr if lo is None else min(lo, vaddr)
            hi = (vaddr + memsz) if hi is None else max(hi, vaddr + memsz)
    if lo is None or hi is None:
        return default_base, default_base + 0x00800000
    return lo, hi


def main() -> int:
    ap = argparse.ArgumentParser(description="Decode Luma3DS ARM11 crash_dump_*.dmp against nox.elf")
    ap.add_argument("dump", help="Path to Luma3DS crash_dump_*.dmp")
    ap.add_argument("elf", help="Path to matching nox-<tag>.elf or nox-<tag>.elf.gz")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x00100000, help="3DS process text base (default 0x00100000)")
    args = ap.parse_args()

    dmp_bytes = Path(args.dump).read_bytes()
    if len(dmp_bytes) < 0x58:
        print(f"ERROR: {args.dump} is too small ({len(dmp_bytes)} B) to be a Luma3DS crash dump", file=sys.stderr)
        return 1

    magic0, magic1 = struct.unpack_from("<II", dmp_bytes, 0)
    if magic0 != 0xDEADC0DE or (magic1 >> 16) != 0xCAFE:
        print(f"WARN: Unexpected Luma3DS header magic (0x{magic0:08X} 0x{magic1:08X}); attempting parse anyway", file=sys.stderr)

    ver_min = magic1 & 0xFFFF
    processor, exc_type, _, nb_regs, code_sz, stack_sz, add_sz = struct.unpack_from("<HHHIIII", dmp_bytes, 8)
    hdr_sz = 0x20
    reg_bytes = nb_regs * 4
    regs = list(struct.unpack_from(f"<{nb_regs}I", dmp_bytes, hdr_sz)) if len(dmp_bytes) >= hdr_sz + reg_bytes else []

    elf_in = Path(args.elf)
    tmp_dir = None
    if elf_in.suffix == ".gz":
        tmp_dir = tempfile.TemporaryDirectory()
        elf_path = Path(tmp_dir.name) / elf_in.stem
        with gzip.open(elf_in, "rb") as fin, open(elf_path, "wb") as fout:
            shutil.copyfileobj(fin, fout)
    else:
        elf_path = elf_in

    try:
        text_lo, text_hi = get_text_bounds(elf_path, args.base)
        print(f"=== Luma3DS Crash Dump Report (v1.{ver_min}, ARM{processor}) ===")
        print(f"Exception : {EXCEPTION_TYPES.get(exc_type, f'Type {exc_type}')}")

        if len(regs) >= 17:
            for i in range(0, 13, 4):
                chunk = "  ".join(f"R{j:<2d}=0x{regs[j]:08X}" for j in range(i, min(13, i + 4)))
                print(f"  {chunk}")
            sp, lr, pc, cpsr = regs[13], regs[14], regs[15], regs[16]
            print(f"  SP =0x{sp:08X}  LR =0x{lr:08X}  PC =0x{pc:08X}  CPSR=0x{cpsr:08X}")
            if len(regs) >= 20:
                dfsr, ifsr, far = regs[17], regs[18], regs[19]
                print(f"  DFSR=0x{dfsr:08X}  IFSR=0x{ifsr:08X}  FAR=0x{far:08X} (Fault Address)")

            print("\n=== Faulting PC & Caller LR ===")
            for label, addr in [("PC", pc), ("LR", (lr - 4) if lr >= 4 else lr)]:
                lines = addr2line(elf_path, [addr])
                loc = lines[0] if lines else "??"
                print(f"  {label} (0x{addr:08X}) -> {loc}")

        stack_off = hdr_sz + reg_bytes + code_sz
        if stack_sz > 0 and len(dmp_bytes) >= stack_off + stack_sz:
            stack_words = struct.unpack_from(f"<{stack_sz // 4}I", dmp_bytes, stack_off)
            candidates = []
            seen = set()
            for idx, w in enumerate(stack_words):
                if text_lo <= w < text_hi and (w & 3) == 0:
                    call_site = w - 4
                    if call_site not in seen:
                        seen.add(call_site)
                        candidates.append((idx * 4, call_site))
                    if len(candidates) >= 24:
                        break

            if candidates:
                print(f"\n=== Probable Stack Return Addresses (SP+offset in [{hex(text_lo)}..{hex(text_hi)})) ===")
                resolved = addr2line(elf_path, [c[1] for c in candidates])
                idx_r = 0
                for sp_off, addr in candidates:
                    desc = resolved[idx_r] if idx_r < len(resolved) else "??"
                    idx_r += 1
                    if "??:0" not in desc and "??:?" not in desc:
                        print(f"  [SP+0x{sp_off:04X}] 0x{addr:08X} -> {desc}")
    finally:
        if tmp_dir:
            tmp_dir.cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
