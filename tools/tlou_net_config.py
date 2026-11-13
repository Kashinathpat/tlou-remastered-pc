#!/usr/bin/env python3
"""The Last of Us Remastered 1.11: the online configuration Factions downloads, for a private server.

On choosing Factions the game downloads http://t1ps4.final.prod.s3.amazonaws.com/./net10.bin.psarc.crypt,
mounts it at /download0/net10.bin and copies its one file, net10.bin, to /download0/net10.bin:
the DC module that overrides the game's "net" module, with the server endpoints (ticket server and the
others) and many online settings. The update ships a copy of that module (bin/dc1/net-10.bin);
this tool rewrites its server host names to a host of your choice and writes the file the game
expects, for the runtime to serve with GOW3_HTTP_FILES (nothing is downloaded from Naughty Dog).

  python tools/tlou_net_config.py servers                     the endpoints in the shipped module
  python tools/tlou_net_config.py build --host 192.168.0.10   writes <out>/<s3 host>/net10.bin.psarc.crypt
  python tools/tlou_net_config.py decrypt <file> <out>        a .crypt file's plaintext (MAC checked)
  python tools/tlou_net_config.py encrypt <file> <out>
  (--signed: the format of saves and profiles instead of downloads)

Downloads (".crypt" over HTTP; the download task at eboot 0x991AA0, header read at 0x9916A2):
a 24-byte header in clear, the plaintext length (u32, big-endian) and HMAC-SHA1 of the padded
plaintext (20 bytes), then the plaintext padded with zeros to 8 bytes, enciphered with
Blowfish-ECB (big-endian words). The game writes the first <length> bytes to a file once the
HMAC matches, and deletes it otherwise.
Saves and profiles (net-drm.cpp NetCryptAndSign / NetDecryptAndVerify): the plaintext, zeros up
to (length + 27) & ~7 - 20, then the HMAC of all that, every block enciphered.
The Blowfish key, the HMAC key and Blowfish's tables are read from your eboot.bin: none ships
with this tool.

The DC module (see tools/tlou_render_settings.py): header {"00CD", 1, data size, ..., entry count,
entry table}; entries {sid32 name, sid32 type, offset}; after the data a u32 byte count and a
bitmap marking the 8-byte words that hold module offsets. Host names are appended to the data and
the pointers to the old ones moved there. The configuration is entry 0x1D38F2F9; its fields
+0x70..+0xE8 point to lists {count, items} of {char *host, u32 port} (+0xE8: the ticket server,
read at eboot 0x3BB7C2).
"""
import argparse
import hashlib
import hmac
import lzma
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
S3_HOST = "t1ps4.final.prod.s3.amazonaws.com"
CRYPT_NAME = "net10.bin.psarc.crypt"
MODULE_IN_UPDATE = "build/ps4/main/bin/dc1/net-10.bin"
MODULE_IN_PSARC = "net10.bin"  # opened at /download0/net10.bin/net10.bin, the mounted archive
CONFIG_ENTRY = 0x1D38F2F9
SERVER_LISTS = range(0x70, 0xF0, 8)


# --- eboot.bin (SELF) ---------------------------------------------------------------------------
class Eboot:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        if d[:4] != b"\x4f\x15\x3d\x1d":
            raise SystemExit(f"{path}: not a PS4 SELF")
        count = struct.unpack_from("<H", d, 0x18)[0]
        offsets = {}
        for k in range(count):
            flags, off, _, _ = struct.unpack_from("<QQQQ", d, 0x20 + 32 * k)
            if flags & 0x800:  # blocked: the segment's data
                offsets[(flags >> 20) & 0xFFF] = off
        elf = d.find(b"\x7fELF")
        phoff = struct.unpack_from("<Q", d, elf + 0x20)[0]
        phnum = struct.unpack_from("<H", d, elf + 0x38)[0]
        self.segments = []
        for i in range(phnum):
            _, _, _, va, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", d, elf + phoff + 56 * i)
            if i in offsets and filesz:
                self.segments.append((va, offsets[i], filesz))

    def read(self, va, n):
        for v, o, s in self.segments:
            if v <= va and va + n <= v + s:
                return self.data[o + va - v:o + va - v + n]
        raise SystemExit(f"eboot.bin: address {va:#x} not in the file (not update 1.11's executable?)")

    def lea_target(self, at, opcode):
        """Target of a RIP-relative lea at `at` (opcode bytes checked)."""
        code = self.read(at, len(opcode) + 4)
        if code[:len(opcode)] != opcode:
            raise SystemExit(f"eboot.bin: unexpected code at {at:#x}: this tool needs update 1.11's executable")
        return at + len(opcode) + 4 + struct.unpack_from("<i", code, len(opcode))[0]


class Crypto:
    def __init__(self, eboot):
        e = Eboot(eboot)
        # NetDecryptAndVerify (0x997BA0): the HMAC key (lea rdi at 0x997C36), the Blowfish key
        # (set at 0x997A48: lea rsi), Blowfish's S boxes and P array (set-key at 0x9AD230).
        self.mac_key = e.read(e.lea_target(0x997C36, b"\x48\x8d\x3d"), 64)
        self.bf_key = e.read(e.lea_target(0x997A48, b"\x48\x8d\x35"), 20)
        s0 = e.lea_target(0x9AD259, b"\x48\x8d\x35")
        self.s_init = [list(struct.unpack("<256I", e.read(s0 + 0x400 * i, 0x400))) for i in range(4)]
        self.p_init = list(struct.unpack("<18I", e.read(s0 + 0x1000, 72)))
        if self.p_init[0] != 0x243F6A88 or self.s_init[0][0] != 0xD1310BA6:
            raise SystemExit("eboot.bin: Blowfish tables not where update 1.11 keeps them")
        self._schedule()

    def _schedule(self):
        self.p = self.p_init[:]
        self.s = [box[:] for box in self.s_init]
        key = self.bf_key
        for i in range(18):
            self.p[i] ^= int.from_bytes(bytes(key[(i * 4 + j) % len(key)] for j in range(4)), "big")
        left = right = 0
        for i in range(0, 18, 2):
            left, right = self._block(left, right, False)
            self.p[i], self.p[i + 1] = left, right
        for box in self.s:
            for i in range(0, 256, 2):
                left, right = self._block(left, right, False)
                box[i], box[i + 1] = left, right

    def _f(self, x):
        s = self.s
        return ((((s[0][x >> 24] + s[1][(x >> 16) & 255]) & 0xFFFFFFFF) ^ s[2][(x >> 8) & 255]) + s[3][x & 255]) & 0xFFFFFFFF

    def _block(self, left, right, decrypt):
        order = range(17, 1, -1) if decrypt else range(16)
        for i in order:
            left ^= self.p[i]
            right ^= self._f(left)
            left, right = right, left
        left, right = right, left
        if decrypt:
            right ^= self.p[1]
            left ^= self.p[0]
        else:
            right ^= self.p[16]
            left ^= self.p[17]
        return left, right

    def _ecb(self, data, decrypt):
        out = bytearray(data)
        for i in range(0, len(out) // 8 * 8, 8):
            struct.pack_into(">II", out, i, *self._block(*struct.unpack_from(">II", out, i), decrypt))
        return bytes(out)

    def _mac(self, data):
        return hmac.new(self.mac_key, data, hashlib.sha1).digest()

    def encrypt(self, plain):
        """A download: clear header (length, HMAC), then the padded plaintext enciphered."""
        body = plain + bytes(-len(plain) % 8)
        return struct.pack(">I", len(plain)) + self._mac(body) + self._ecb(body, False)

    def decrypt(self, data):
        if len(data) < 24:
            raise SystemExit("decrypt: too short for a download's header")
        length, mac = struct.unpack_from(">I", data)[0], data[4:24]
        body = self._ecb(data[24:], True)
        if not hmac.compare_digest(self._mac(body), mac) or length > len(body):
            raise SystemExit("decrypt: the HMAC does not match (not a download of this game, or damaged)")
        return body[:length]

    def encrypt_signed(self, plain):
        """NetCryptAndSign: zeros up to (len + 27) & ~7 - 20, the HMAC, every block enciphered."""
        total = (len(plain) + 27) & ~7
        body = plain + bytes(total - 20 - len(plain))
        return self._ecb(body + self._mac(body), False)

    def decrypt_signed(self, data):
        plain = self._ecb(data, True)
        body, mac = plain[:-20], plain[-20:]
        if not hmac.compare_digest(self._mac(body), mac):
            raise SystemExit("decrypt: the HMAC does not match (not a file of this game, or damaged)")
        return body


# --- PSARC ----------------------------------------------------------------------------------------
def psarc_read(path, wanted):
    with open(path, "rb") as f:
        magic, _, _, comp, toc_len, esz, count, block, _ = struct.unpack(">4sHH4sIIIII", f.read(32))
        if magic != b"PSAR":
            raise SystemExit(f"{path}: not a PSARC")
        toc = f.read(toc_len - 32)
        entries = [(toc[i * esz:i * esz + 16], struct.unpack(">I", toc[i * esz + 16:i * esz + 20])[0],
                    int.from_bytes(toc[i * esz + 20:i * esz + 25], "big"),
                    int.from_bytes(toc[i * esz + 25:i * esz + 30], "big")) for i in range(count)]
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

        by_md5 = {e[0]: i for i, e in enumerate(entries)}
        for name in read(0).replace(b"\0", b"\n").decode("utf-8", "replace").split("\n"):
            if name.lstrip("/") == wanted:
                index = by_md5.get(hashlib.md5(name.encode()).digest())
                if index is not None:
                    return read(index)
    raise SystemExit(f"{path}: no {wanted}")


def psarc_write(files):
    """PSARC 1.4, zlib, 64 KiB blocks, relative names (like the game's bin.psarc)."""
    block = 0x10000
    names = list(files)
    blobs = ["\n".join(names).encode()] + [files[n] for n in names]
    md5s = [bytes(16)] + [hashlib.md5(n.encode()).digest() for n in names]
    sizes, chunks, entries = [], [], []
    for blob in blobs:
        first = len(sizes)
        for i in range(0, max(len(blob), 1), block):
            piece = blob[i:i + block]
            packed = zlib.compress(piece, 9)
            if len(packed) >= len(piece):
                packed = piece
            sizes.append(0 if len(packed) == block else len(packed))
            chunks.append(packed)
        entries.append((first, len(blob), sum(len(c) for c in chunks[:first])))
    toc_len = 32 + 30 * len(blobs) + 2 * len(sizes)
    out = bytearray(struct.pack(">4sHH4sIIIII", b"PSAR", 1, 4, b"zlib", toc_len, 30, len(blobs), block, 0))
    for md5, (first, length, offset) in zip(md5s, entries):
        out += md5 + struct.pack(">I", first) + length.to_bytes(5, "big") + (toc_len + offset).to_bytes(5, "big")
    for s in sizes:
        out += struct.pack(">H", s)
    for c in chunks:
        out += c
    return bytes(out)


# --- DC module ----------------------------------------------------------------------------------
class Dc:
    def __init__(self, data):
        self.d = bytearray(data)
        magic, _, self.size, _, _, count, table, _ = struct.unpack_from("<IIIIIIII", data, 0)
        if magic != 0x44433030:
            raise SystemExit("not a DC module")
        self.entries = {struct.unpack_from("<I", data, table + 16 * i)[0]: struct.unpack_from("<Q", data, table + 16 * i + 8)[0]
                        for i in range(count)}
        nbytes = struct.unpack_from("<I", data, self.size)[0]
        self.bitmap = bytearray(data[self.size + 4:self.size + 4 + nbytes])

    def is_pointer(self, at):
        word = at // 8
        return word // 8 < len(self.bitmap) and self.bitmap[word // 8] >> (word % 8) & 1

    def q(self, at):
        return struct.unpack_from("<Q", self.d, at)[0]

    def cstr(self, at):
        return bytes(self.d[at:self.d.index(b"\0", at)]).decode("latin-1")

    def servers(self):
        config = self.entries.get(CONFIG_ENTRY)
        if config is None:
            raise SystemExit("net module: no configuration entry (not update 1.11's net-10.bin?)")
        out = []
        for field in SERVER_LISTS:
            if not self.is_pointer(config + field):
                continue  # +0xD0, +0xD8: numbers
            lst = self.q(config + field)
            if not 0 < lst < self.size:
                continue
            count, items = self.q(lst), self.q(lst + 8)
            if not 0 < count < 64 or not 0 < items < self.size:
                continue
            for i in range(count):
                host = self.q(items + 24 * i)
                port = struct.unpack_from("<I", self.d, items + 24 * i + 8)[0]
                out.append((field, i, self.cstr(host) if 0 < host < self.size else "?", port))
        return out

    def replace_strings(self, wanted, value):
        """Every string `wanted` accepts (text -> bool) is pointed at `value`, appended to the data."""
        new_at = (self.size + 7) & ~7
        replaced = {}
        for m in re.finditer(rb"(?<=\0)([\x20-\x7e]{3,80})\0", bytes(self.d[:self.size])):
            text = m.group(1).decode()
            if wanted(text):
                replaced[m.start()] = text
        moved = 0
        for at in range(0, self.size - 7, 8):
            if self.is_pointer(at) and self.q(at) in replaced:
                struct.pack_into("<Q", self.d, at, new_at)
                moved += 1
        data = bytes(self.d[:self.size]).ljust(new_at, b"\0") + value.encode() + b"\0"
        size = (len(data) + 7) & ~7
        data = data.ljust(size, b"\0")
        need = (size + 63) // 64  # one bit per 8-byte word
        bitmap = self.bitmap + bytes(max(0, need - len(self.bitmap)))
        header = bytearray(data[:32])
        struct.pack_into("<I", header, 8, size)
        self.d = bytearray(header + data[32:] + struct.pack("<I", len(bitmap)) + bitmap)
        self.size, self.bitmap = size, bytearray(bitmap)
        return sorted(set(replaced.values())), moved


def is_server_host(text):
    return bool(re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", text)) or text.endswith(".naughtydog.com")


def default_game():
    return ROOT.parent / "CUSA00552"


def update_folder(game):
    for name in (game.name + "-patch", game.name + "-update"):
        if (game.parent / name / "patch.psarc").is_file():
            return game.parent / name
    raise SystemExit(f"No update 1.11 beside {game} ({game.name}-patch)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", type=Path, default=default_game(), help="the game folder (CUSA00552)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("servers")
    build = sub.add_parser("build")
    build.add_argument("--host", required=True, help="IP address or host name of the private server")
    build.add_argument("--out", type=Path, default=ROOT / "analysis/tlou-user/http",
                       help="the folder GOW3_HTTP_FILES names")
    for name in ("decrypt", "encrypt"):
        p = sub.add_parser(name)
        p.add_argument("input", type=Path)
        p.add_argument("output", type=Path)
        p.add_argument("--signed", action="store_true", help="the save / profile format, not a download")
    args = parser.parse_args()
    update = update_folder(args.game)

    if args.command in ("decrypt", "encrypt"):
        crypto = Crypto(update / "eboot.bin")
        data = args.input.read_bytes()
        if args.command == "decrypt":
            result = crypto.decrypt_signed(data) if args.signed else crypto.decrypt(data)
        else:
            result = crypto.encrypt_signed(data) if args.signed else crypto.encrypt(data)
        args.output.write_bytes(result)
        print(f"{args.command}ed {args.input} -> {args.output}")
        return 0

    dc = Dc(psarc_read(update / "patch.psarc", MODULE_IN_UPDATE))
    if args.command == "servers":
        for field, index, host, port in dc.servers():
            print(f"config +{field:#04x}[{index}]: {host}:{port}" + ("   (ticket server)" if field == 0xE8 else ""))
        return 0

    old, moved = dc.replace_strings(is_server_host, args.host)
    dc = Dc(bytes(dc.d))  # re-read: the layout is checked again
    crypto = Crypto(update / "eboot.bin")
    psarc = psarc_write({MODULE_IN_PSARC: bytes(dc.d)})
    out = args.out / S3_HOST / CRYPT_NAME
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(crypto.encrypt(psarc))
    if crypto.decrypt(out.read_bytes()) != psarc:
        raise SystemExit("internal error: the written file does not decrypt back")
    print(f"Hosts {', '.join(old)} -> {args.host} ({moved} pointers)")
    for field, index, host, port in dc.servers():
        print(f"  config +{field:#04x}[{index}]: {host}:{port}")
    print(f"Wrote {out} ({out.stat().st_size} bytes); run the game with GOW3_HTTP_FILES={args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
