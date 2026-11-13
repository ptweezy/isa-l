"""Time the PyPI wheels on the reference .gz files that ./bench --dump wrote.

Compare the "one call" column with the "gzip, one call" column of ./bench: a
wheel that is much slower than the same library built natively was built
without its fast paths.
"""
import platform
import subprocess
import sys
import time
import zlib

from isal import isal_zlib
from zlib_ng import zlib_ng

CHUNK = 128 * 1024
ROUNDS = 7

BACKENDS = [
    ("zlib (Python's)", zlib),
    ("isal wheel", isal_zlib),
    ("zlib-ng wheel", zlib_ng),
]


def one_call(mod, data, size):
    return len(mod.decompress(data, wbits=31, bufsize=size))


def stream(mod, data, size):
    d = mod.decompressobj(wbits=31)
    view = memoryview(data)
    pos = total = 0
    buf = b""
    while not d.eof:
        if not buf:
            buf = view[pos:pos + CHUNK]
            pos += CHUNK
        total += len(d.decompress(buf, CHUNK))
        buf = d.unconsumed_tail
    return total


def main():
    print("machine:", platform.machine(), platform.platform(), "| python", sys.version.split()[0])
    print("zlib runtime:", zlib.ZLIB_RUNTIME_VERSION,
          "| isal wheel:", getattr(isal_zlib, "ISAL_VERSION", "?"),
          "| zlib-ng wheel:", getattr(zlib_ng, "ZLIBNG_VERSION", "?"))
    tool = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
    out = subprocess.run(tool + [zlib.__file__], capture_output=True, text=True).stdout
    print("Python's zlib links:", " ".join(l.strip() for l in out.splitlines() if "libz" in l) or "(static)")

    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        size = len(zlib.decompress(data, 31))
        best = {(name, fn.__name__): float("inf") for name, _ in BACKENDS for fn in (one_call, stream)}
        for _ in range(ROUNDS):  # interleaved
            for fn in (one_call, stream):
                for name, mod in BACKENDS:
                    start = time.perf_counter()
                    got = fn(mod, data, size)
                    elapsed = time.perf_counter() - start
                    assert got == size, (name, fn.__name__, got, size)
                    best[name, fn.__name__] = min(best[name, fn.__name__], elapsed)
        print(f"\n### {path.rsplit('/', 1)[-1]}: {size / 1e6:.1f} MB\n")
        print(f"Python wheels, gzip, MB/s of output, best of {ROUNDS} interleaved rounds:\n")
        print("| backend | one call | 128 KiB stream |")
        print("|---|---:|---:|")
        for name, _ in BACKENDS:
            print(f"| {name} | {size / 1e6 / best[name, 'one_call']:.0f} | {size / 1e6 / best[name, 'stream']:.0f} |")


main()
