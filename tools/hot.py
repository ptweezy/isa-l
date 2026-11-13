#!/usr/bin/env python3
"""Rank the hottest instructions in a profile written by `bench --prof`.

    hot.py PROFILE BINARY [TOP]

PROFILE has one "address count" line per instruction that was seen running,
and the address of prof_start() in the running process, from which the load
address of BINARY follows. This prints the share of each function, and for
the hottest functions of BINARY the disassembly around the hottest
instructions, with the samples of every instruction next to it.

Addresses outside BINARY (shared libraries) are counted together and are not
disassembled.

The interrupt usually lands one or two instructions after the one that was
slow, so read a count as "here or just before".
"""
import bisect
import collections
import re
import subprocess
import sys


def run(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout
    except OSError:
        return ""


def symbols(binary):
    """Sorted [(address, name)] of the functions in the binary."""
    out = []
    for line in run("nm", "-n", binary).splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+([tT])\s+(\S+)$", line)
        if m:
            out.append((int(m.group(1), 16), m.group(3).lstrip("_")))
    return sorted(out)


def disassemble(binary):
    """{address: text} for every instruction, from otool or objdump."""
    insns = {}
    text = run("otool", "-tvV", binary)
    if text.strip():
        for line in text.splitlines():
            m = re.match(r"^([0-9a-f]{8,16})\s+(.*)$", line)
            if m:
                insns[int(m.group(1), 16)] = m.group(2).strip()
        if insns:
            return insns
    for line in run("objdump", "-d", "--no-show-raw-insn", binary).splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+(.*)$", line)
        if m:
            insns[int(m.group(1), 16)] = m.group(2).strip()
    return insns


def main():
    profile, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    counts = {}
    ref = None
    lost = 0
    for line in open(profile):
        f = line.split()
        if f[0] == "ref":
            ref = (f[1], int(f[2], 16))
        elif f[0] == "lost":
            lost = int(f[1])
        else:
            counts[int(f[0], 16)] = int(f[1])
    total = sum(counts.values())
    if not total:
        sys.exit("no samples in " + profile)

    syms = symbols(binary)
    names = {name: addr for addr, name in syms}
    if not ref or ref[0] not in names:
        sys.exit("cannot find %s in %s" % (ref[0] if ref else "ref", binary))
    slide = ref[1] - names[ref[0]]
    addrs = [a for a, _ in syms]
    lo, hi = addrs[0], addrs[-1] + 0x10000

    per_func = collections.Counter()
    per_insn = collections.defaultdict(dict)
    for pc, n in counts.items():
        a = pc - slide
        if lo <= a < hi:
            i = bisect.bisect_right(addrs, a) - 1
            name = syms[i][1]
            per_insn[name][a] = per_insn[name].get(a, 0) + n
        else:
            name = "(outside the binary)"
        per_func[name] += n

    print("samples: %d%s" % (total, " (%d lost)" % lost if lost else ""))
    print("\nfunctions:")
    for name, n in per_func.most_common(12):
        print("  %5.1f%%  %s" % (100.0 * n / total, name))

    insns = disassemble(binary)
    order = sorted(insns)
    index = {a: i for i, a in enumerate(order)}
    for name, n in per_func.most_common(4):
        if 100.0 * n / total < 3.0 or name not in per_insn:
            continue
        mine = per_insn[name]
        start = names[name]
        print("\n== %s: %.1f%% of samples ==" % (name, 100.0 * n / total))
        show = set()
        for a, _ in sorted(mine.items(), key=lambda kv: -kv[1])[:top]:
            i = index.get(a)
            if i is None:
                continue
            show.update(range(max(0, i - 4), min(len(order), i + 3)))
        prev = None
        for i in sorted(show):
            if prev is not None and i != prev + 1:
                print("        ...")
            a = order[i]
            c = mine.get(a, 0)
            print("  +%-6d %6s %6s  %s" % (
                a - start, c if c else "",
                ("%.2f%%" % (100.0 * c / total)) if c else "", insns[a]))
            prev = i


if __name__ == "__main__":
    main()
