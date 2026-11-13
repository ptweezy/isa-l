#!/usr/bin/env python3
"""Rank the hottest instructions in a sample(1) report.

    hot.py REPORT.sample BINARY [TOP]

sample(1) prints a call tree in which every line is
    <count> <symbol>  (in <image>) + <offset>  [<address>]
and a line's own samples are its count less the counts of its children. This
adds up the own samples per (symbol, offset), prints the share of each
function, and, for functions in BINARY, the disassembly of the hottest
stretches with a sample count on every instruction.

Functions in system libraries are listed with their offsets only: they are
not disassembled.
"""
import collections
import re
import subprocess
import sys

LINE = re.compile(
    r"^(?P<lead>[\s+!:|]*)(?P<n>\d+)\s+(?P<sym>.+?)\s+\(in (?P<img>[^)]+)\)"
    r"(?:\s+\+\s+(?P<off>\d+))?(?:.*?\[(?P<addr>0x[0-9a-f]+)\])?"
)


def parse(path):
    """Yield (depth, count, symbol, image, offset) for the heaviest thread."""
    rows = []
    in_tree = False
    for line in open(path, errors="replace"):
        if line.startswith("Call graph:"):
            in_tree = True
            continue
        if in_tree and (line.startswith("Total number in stack")
                        or line.startswith("Sort by top of stack")):
            break
        if not in_tree:
            continue
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        depth = len(m.group("lead"))
        rows.append((depth, int(m.group("n")), m.group("sym"), m.group("img"),
                     int(m.group("off") or 0)))
    return rows


def own_counts(rows):
    """Own samples per (image, symbol, offset): count less direct children."""
    own = collections.Counter()
    for i, (depth, n, sym, img, off) in enumerate(rows):
        kids = 0
        child_depth = None
        for d2, n2, _, _, _ in rows[i + 1:]:
            if d2 <= depth:
                break
            if child_depth is None:
                child_depth = d2
            if d2 == child_depth:
                kids += n2
        if n - kids > 0:
            own[(img, sym, off)] += n - kids
    return own


def disassemble(binary):
    """Map symbol -> list of (offset, text) from otool."""
    try:
        text = subprocess.run(["otool", "-tvV", binary], capture_output=True,
                              text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return {}
    funcs = {}
    cur = None
    base = None
    for line in text.splitlines():
        if line.endswith(":") and not line.startswith(("0", " ", "\t")):
            cur = line[:-1].lstrip("_")
            funcs[cur] = []
            base = None
            continue
        m = re.match(r"^([0-9a-f]{8,16})\s+(.*)$", line)
        if m and cur is not None:
            addr = int(m.group(1), 16)
            if base is None:
                base = addr
            funcs[cur].append((addr - base, m.group(2).strip()))
    return funcs


def main():
    report, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    own = own_counts(parse(report))
    total = sum(own.values())
    if not total:
        sys.exit("no samples parsed from " + report)

    per_func = collections.Counter()
    for (img, sym, off), n in own.items():
        per_func[(img, sym)] += n

    print("samples: %d" % total)
    print("\nfunctions (own samples):")
    for (img, sym), n in per_func.most_common(12):
        print("  %5.1f%%  %-40s %s" % (100.0 * n / total, sym, img))

    funcs = disassemble(binary)
    for (img, sym), n in per_func.most_common(6):
        if 100.0 * n / total < 2.0:
            break
        offs = {off: c for (i, s, off), c in own.items() if (i, s) == (img, sym)}
        print("\n== %s (%s): %.1f%% of samples ==" % (sym, img, 100.0 * n / total))
        code = funcs.get(sym.lstrip("_"))
        if not code:
            for off, c in sorted(offs.items(), key=lambda kv: -kv[1])[:top]:
                print("  +%-6d %6d  %5.2f%%" % (off, c, 100.0 * c / total))
            continue
        # sample(1) reports the address of the instruction that was running,
        # and a slow instruction is usually blamed on the one after it.
        index = {off: i for i, (off, _) in enumerate(code)}
        hot = sorted(offs.items(), key=lambda kv: -kv[1])[:top]
        show = set()
        for off, _ in hot:
            i = index.get(off)
            if i is None:
                continue
            for j in range(max(0, i - 3), min(len(code), i + 3)):
                show.add(j)
        prev = None
        for j in sorted(show):
            if prev is not None and j != prev + 1:
                print("        ...")
            off, text = code[j]
            c = offs.get(off, 0)
            print("  +%-6d %6s %6s  %s" % (
                off, c if c else "", ("%.2f%%" % (100.0 * c / total)) if c else "",
                text))
            prev = j


if __name__ == "__main__":
    main()
