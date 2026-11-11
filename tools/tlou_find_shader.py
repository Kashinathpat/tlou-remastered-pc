#!/usr/bin/env python3
"""Finds the game's shaders in its PSARC packages: by the hash the runtime names them by, or by
file name.

Every PS4 shader binary ends with an "OrbShdr" footer whose 64-bit shader hash (at +16) is the
hash the runtime's logs and switches use (cs e44070db, ...). Naughty Dog keeps the compiled
shaders as bin/shaders/bytecode/<Name>.cxo in bin.psarc, and the update's in patch.psarc.

  python tools/tlou_find_shader.py e44070db              which file holds this shader
  python tools/tlou_find_shader.py --name Cloth          the cloth shaders and their hashes

Found this way on 10 Nov 2026: e44070db is Cs_ClothSimulate512.cxo of update 1.11.
"""
import argparse
import hashlib
import lzma
import os
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def psarc_entries(path):
    """(name, read) of every entry; names are matched to entries by the MD5 of the name (the
    update's patch.psarc does not keep entries in the order of its name list)."""
    f = open(path, "rb")
    magic, _, _, comp, toc_len, esz, count, block, _ = struct.unpack(">4sHH4sIIIII", f.read(32))
    if magic != b"PSAR":
        raise ValueError(f"{path}: not a PSARC")
    toc = f.read(toc_len - 32)
    entries = []
    for i in range(count):
        e = toc[i * esz:(i + 1) * esz]
        entries.append((e[:16], struct.unpack(">I", e[16:20])[0],
                        int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
    width = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
    raw = toc[count * esz:]
    sizes = [int.from_bytes(raw[i:i + width], "big") for i in range(0, len(raw) - width + 1, width)]

    def read(index):
        _, first, length, offset = entries[index]
        out = bytearray()
        f.seek(offset)
        while len(out) < length:
            stored = sizes[first]
            first += 1
            chunk = f.read(stored or block)
            if len(chunk) != min(block, length - len(out)):
                chunk = zlib.decompress(chunk) if comp == b"zlib" else lzma.decompress(chunk)
            out += chunk
        return bytes(out)

    names = read(0).replace(b"\0", b"\n").decode("utf-8", "replace").split("\n")
    by_md5 = {hashlib.md5(n.encode()).digest(): n for n in names if n}
    for i in range(1, count):
        yield by_md5.get(entries[i][0], f"<entry {i}>"), (lambda i=i: read(i))


def footers(data):
    """(offset, shader hash, code bytes) of every OrbShdr footer."""
    for m in re.finditer(re.escape(b"OrbShdr"), data):
        pos = m.start()
        if pos + 24 <= len(data):
            bits = struct.unpack_from("<I", data, pos + 8)[0]
            yield pos, struct.unpack_from("<Q", data, pos + 16)[0], (bits >> 8) & 0xFFFFFF


def packages(game):
    update = Path(os.environ.get("TLOU_UPDATE") or f"{game}-patch")
    out = [update / "patch.psarc"] if (update / "patch.psarc").is_file() else []
    out += sorted((game / "build/ps4/main").glob("*.psarc"))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("hashes", nargs="*", help="shader hashes (hex)")
    parser.add_argument("--name", help="list the shaders whose file name contains this")
    parser.add_argument("--game", type=Path, default=ROOT.parent / "CUSA00552")
    parser.add_argument("--package", type=Path, action="append",
                        help="search only these PSARC files")
    args = parser.parse_args()
    wanted = {int(h, 16) for h in args.hashes}
    if not wanted and not args.name:
        parser.error("give shader hashes or --name")
    for path in args.package or packages(args.game):
        print(f"-- {path}", flush=True)
        for name, read in psarc_entries(path):
            if args.name:
                if args.name.lower() in name.lower() and name.endswith(".cxo"):
                    hashes = " ".join(f"{h:08x} ({n} B)" for _, h, n in footers(read()))
                    print(f"  {name}  {hashes}")
                continue
            if name.endswith((".bnk", ".wem", ".mp4", ".bik")):
                continue
            for pos, h, n in footers(read()):
                if h in wanted:
                    print(f"  {h:08x}: {name}, footer at {pos:#x}, {n} bytes of code")


if __name__ == "__main__":
    main()
