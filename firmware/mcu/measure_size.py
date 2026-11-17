#!/usr/bin/env python3
"""Report the IDS-RNN detector's flash/RAM cost on Cortex-M4F from the ARM build.

  python firmware/mcu/measure_size.py [firmware/build-mcu]

Method: probe_ids_<opt>.elf and probe_base_<opt>.elf are the same program,
built with the same flags, except that only the first runs the detector. Every
symbol present in the first but not the second is cost caused by the detector,
including library routines (libm, libc) that it pulls in. Symbols are grouped
using the linker map, which records the archive each one came from.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

NM = shutil.which("arm-none-eabi-nm") or "arm-none-eabi-nm"


def symbols(elf: Path) -> dict[str, tuple[str, int]]:
    """name -> (nm type letter, size)"""
    out = subprocess.run([NM, "-S", "--size-sort", "-t", "d", str(elf)],
                         capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            _, size, kind, name = parts
            syms[name] = (kind, int(size))
    return syms


def origins(map_file: Path) -> dict[str, str]:
    """symbol/section name -> 'libm', 'libc', 'libgcc' or 'app' from the linker map."""
    text = map_file.read_text(errors="replace")
    org = {}
    # " .text.expf  0x0800...  0x78 c:/.../libm.a(libm_a-sf_exp.o)" (name may wrap to its own line)
    for m in re.finditer(r"^ \.(?:text|rodata|data|bss)\.([^\s]+)\s*\n?\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+)", text, re.M):
        name, src = m.group(1), m.group(2)
        lib = re.search(r"(lib[a-z_]+)\.a\(", src)
        org[name] = lib.group(1) if lib else "app"
    return org


def classify(name: str, kind: str, org: dict[str, str]) -> str:
    if name.startswith("IDS_"):
        return "model parameters (weights, scales, biases)"
    if kind in "bB":
        return "RAM: detector state (.bss)"
    if kind in "dD":
        return "RAM: initialised data (.data)"
    if name == "main":
        return "probe harness (main grows to feed the detector)"
    if name in ("memset", "memcpy"):
        return "libc (memset)"
    src = org.get(name)
    if src in ("libm",):
        return "libm (tanhf, expf, log1pf and helpers)"
    if src in ("libc", "libc_nano", "libg_nano"):
        return "libc (memset)"
    if name == "__errno":
        return "startup stub (__errno for libm)"
    if src == "libgcc":
        return "libgcc"
    if name.startswith(("ids_", "matvec", "seq_update", "clip8", "sigmoidf")):
        return "detector code (ids_rnn.c)"
    if src is None and kind in "tTrR":
        # static data inside libm objects (constants such as ln2HI) carry no section name of their own
        return "libm (tanhf, expf, log1pf and helpers)"
    return "other (" + (src or "?") + ")"


def report(build: Path, opt: str) -> int:
    ids, base = build / f"probe_ids_{opt}.elf", build / f"probe_base_{opt}.elf"
    s_ids, s_base = symbols(ids), symbols(base)
    org = origins(build / f"probe_ids_{opt}.map")
    groups: dict[str, list[tuple[str, int]]] = {}
    for name, (kind, size) in s_ids.items():
        base_size = s_base.get(name, (kind, 0))[1]
        if size - base_size > 0:
            groups.setdefault(classify(name, kind, org), []).append((name, size - base_size))

    print(f"\n=== Cortex-M4F, -{opt}: cost of the detector (probe_ids - probe_base) ===")
    flash = ram = 0
    for g in sorted(groups, key=lambda k: (k.startswith("RAM"), -sum(s for _, s in groups[k]))):
        total = sum(s for _, s in groups[g])
        top = ", ".join(f"{n} {s}" for n, s in sorted(groups[g], key=lambda x: -x[1])[:6])
        print(f"  {g:45s} {total:6d} B   [{top}]")
        if g.startswith("RAM"):
            ram += total
        else:
            flash += total
    print(f"  {'TOTAL FLASH':45s} {flash:6d} B")
    print(f"  {'TOTAL STATIC RAM':45s} {ram:6d} B   (+ stack, measured on target by bench)")
    return flash


def stack_usage(build: Path, opt: str) -> None:
    su = list(build.glob(f"CMakeFiles/probe_ids_{opt}.elf.dir/**/ids_rnn.c.su"))
    if su:
        print(f"  compiler stack frames (-fstack-usage, -{opt}):")
        for line in su[0].read_text().splitlines():
            parts = line.split("\t")
            print(f"    {parts[0].split(':')[-1]:20s} {parts[1]:>5s} B  {parts[2]}")


BUDGET = 6144   # bytes of flash for the whole detector (parameters + code + any library code)


def main() -> None:
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "firmware/build-mcu")
    totals = {}
    for opt in ("Os", "O2"):
        if (build / f"probe_ids_{opt}.elf").exists():
            totals[opt] = report(build, opt)
            stack_usage(build, opt)
    print()
    for opt, total in totals.items():
        print(f"-{opt}: {total} B of {BUDGET} B budget -> {'OK' if total <= BUDGET else 'OVER BUDGET'}")
    if not totals or min(totals.values()) > BUDGET:
        sys.exit(1)


if __name__ == "__main__":
    main()
