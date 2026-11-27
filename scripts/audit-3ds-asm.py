#!/usr/bin/env python3
"""
Compact ARM11 Disassembly Summary Script (`audit-3ds-asm.py`).

Instead of dumping thousands of lines of `arm-none-eabi-objdump -d` into an LLM's
context window, this script summarizes target functions in ~1 line each:
  - Total instruction count & memory load/store count (`ldr*`/`str*`/`ldm*`/`stm*`)
  - Flags expensive/forbidden ARM11 instructions in hot loops:
    * Double-precision VFP (`vdiv.f64`, `vsqrt.f64`, `vmul.f64`, `vadd.f64`, `__aeabi_d*`)
    * Single-precision VFP divides (`vdiv.f32`)
    * Software integer division calls (`__aeabi_idiv`, `__aeabi_uidiv`, `__divsi3`, `__udivsi3`)
    * Alignment-sensitive multi-word / VFP loads (`ldm`, `stm`, `ldrd`, `strd`, `vldr`, `vstr`)

Usage:
  ./scripts/audit-3ds-asm.py [build-3ds/nox.elf] <symbol_or_substr1> [symbol2 ...]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

OBJDUMP = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump"
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"


def find_symbols(elf: Path, patterns: list[str]) -> list[tuple[str, int, int]]:
    out = subprocess.check_output([NM, "-S", "--size-sort", "--defined-only", str(elf)], text=True)
    # Also grab unsized/all text symbols in case a symbol has small size
    out_all = subprocess.check_output([NM, "-S", "--defined-only", str(elf)], text=True)
    seen = {}
    for line in (out + "\n" + out_all).splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ("T", "t"):
            addr, size, _, name = int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]
            seen[name] = (name, addr, size)
    matched = []
    for pat in patterns:
        for name, info in sorted(seen.items()):
            if name == pat or pat in name:
                if info not in matched:
                    matched.append(info)
    return matched


def audit_function(elf: Path, name: str, addr: int, size: int) -> str:
    stop = addr + max(size, 64)
    asm = subprocess.check_output(
        [OBJDUMP, "-d", f"--start-address={hex(addr)}", f"--stop-address={hex(stop)}", str(elf)],
        text=True,
    )
    insns = 0
    mem_ops = 0
    flags = {
        "f64": 0,
        "vdiv_f32": 0,
        "sw_idiv": 0,
        "multi_align": 0,
    }
    call_targets = []
    for ln in asm.splitlines():
        m = re.match(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z0-9.]+)\s*(.*)$", ln)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        insns += 1
        if op.startswith(("ldr", "str", "ldm", "stm", "vldr", "vstr", "vldm", "vstm")):
            mem_ops += 1
        if op.endswith(".f64") or "__aeabi_d" in rest:
            flags["f64"] += 1
        if op.startswith("vdiv.f32"):
            flags["vdiv_f32"] += 1
        if any(k in rest for k in ("__aeabi_idiv", "__aeabi_uidiv", "__divsi3", "__udivsi3", "__aeabi_ldivmod", "__aeabi_uldivmod")):
            flags["sw_idiv"] += 1
        if op.startswith(("ldm", "stm", "ldrd", "strd", "vldr", "vstr")) and "sp" not in rest.split(",")[0]:
            flags["multi_align"] += 1
        if op in ("bl", "blx") and "<" in rest:
            callee = rest.split("<", 1)[1].split(">", 1)[0]
            if callee not in call_targets:
                call_targets.append(callee)

    warn_str = ", ".join(f"{k}={v}" for k, v in flags.items() if v > 0) or "clean"
    calls_str = f" calls=[{', '.join(call_targets[:6])}]" if call_targets else " leaf"
    return f"{name} (@0x{addr:08x}, {size}B): insns={insns}, mem={mem_ops}, hazards=({warn_str}){calls_str}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Compact ARM11 function disassembly hazard & instruction auditor")
    ap.add_argument("elf_or_sym", help="Path to nox.elf (or first symbol if build-3ds/nox.elf is default)")
    ap.add_argument("symbols", nargs="*", help="Function symbols to summarize (e.g. sub_4C9B20 sub_469140)")
    args = ap.parse_args()

    if Path(args.elf_or_sym).is_file():
        elf = Path(args.elf_or_sym)
        syms = args.symbols
    else:
        elf = Path("build-3ds/nox.elf")
        syms = [args.elf_or_sym] + list(args.symbols)

    if not syms:
        ap.error("Specify at least one function symbol name")
    if not elf.is_file():
        print(f"ERROR: ELF not found: {elf}", file=sys.stderr)
        return 1

    found = find_symbols(elf, syms)
    if not found:
        print(f"No matching text symbols in {elf} for: {syms}", file=sys.stderr)
        return 1

    for name, addr, size in found:
        print(audit_function(elf, name, addr, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
