"""CRIWARE CPK reader for NieR:Automata: list contents and extract files (with CRILAYLA)."""
import argparse
import fnmatch
import glob
import os
import struct
import sys

# @UTF column storage / type flags
STORAGE_MASK, TYPE_MASK = 0xF0, 0x0F
STORAGE_ZERO, STORAGE_CONST, STORAGE_PERROW = 0x10, 0x30, 0x50
TYPE_FMT = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f"}


def _unmask(buf):
    out, m = bytearray(buf), 0x655F
    for i in range(len(out)):
        out[i] ^= m & 0xFF
        m = (m * 0x4115) & 0xFFFFFFFF
    return bytes(out)


def parse_utf(buf):
    """Parse an @UTF table; returns a list of row dicts."""
    if buf[:4] != b"@UTF":
        buf = _unmask(buf)
        if buf[:4] != b"@UTF":
            raise ValueError("not an @UTF table")
    base = 8
    _size, = struct.unpack_from(">I", buf, 4)
    rows_off, strings_off, data_off, _name_off, ncols, row_len, nrows = struct.unpack_from(">xxHIIIHHI", buf, 8)
    rows_off += base
    strings_off += base
    data_off += base

    def string_at(off):
        start = strings_off + off
        return buf[start:buf.index(b"\0", start)].decode("utf-8", "replace")

    def read_value(typ, pos):
        if typ == 0xA:
            off, = struct.unpack_from(">I", buf, pos)
            return string_at(off), pos + 4
        if typ == 0xB:
            off, size = struct.unpack_from(">II", buf, pos)
            return buf[data_off + off:data_off + off + size], pos + 8
        fmt = TYPE_FMT[typ]
        return struct.unpack_from(fmt, buf, pos)[0], pos + struct.calcsize(fmt)

    cols, pos = [], 32
    for _ in range(ncols):
        flags = buf[pos]
        name_off, = struct.unpack_from(">I", buf, pos + 1)
        pos += 5
        const = None
        if flags & STORAGE_MASK == STORAGE_CONST:
            const, pos = read_value(flags & TYPE_MASK, pos)
        cols.append((string_at(name_off), flags, const))

    rows = []
    for r in range(nrows):
        pos, row = rows_off + r * row_len, {}
        for name, flags, const in cols:
            storage = flags & STORAGE_MASK
            if storage == STORAGE_PERROW:
                row[name], pos = read_value(flags & TYPE_MASK, pos)
            elif storage == STORAGE_CONST:
                row[name] = const
            else:
                row[name] = None
        rows.append(row)
    return rows


def decompress_crilayla(data):
    usize, hdr_off = struct.unpack_from("<II", data, 8)
    result = bytearray(usize + 0x100)
    result[:0x100] = data[hdr_off + 0x10:hdr_off + 0x110]
    in_pos = len(data) - 0x100 - 1
    out_end = 0x100 + usize - 1
    pool = left = 0
    written = 0

    def bits(n):
        nonlocal in_pos, pool, left
        out = 0
        while n:
            if left == 0:
                pool, left = data[in_pos], 8
                in_pos -= 1
            take = min(left, n)
            out = (out << take) | ((pool >> (left - take)) & ((1 << take) - 1))
            left -= take
            n -= take
        return out

    while written < usize:
        if bits(1):
            ref = out_end - written + bits(13) + 3
            length = 3
            for lvl in (2, 3, 5, 8):
                v = bits(lvl)
                length += v
                if v != (1 << lvl) - 1:
                    break
            else:
                while True:
                    v = bits(8)
                    length += v
                    if v != 0xFF:
                        break
            for _ in range(length):
                result[out_end - written] = result[ref]
                ref -= 1
                written += 1
        else:
            result[out_end - written] = bits(8)
            written += 1
    return bytes(result)


class Cpk:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self._chunk(0, b"CPK ")
        self.header = parse_utf(hdr)[0]
        toc_off = self.header["TocOffset"]
        content_off = self.header["ContentOffset"]
        self.base = min(toc_off, content_off) if content_off else toc_off
        self.files = parse_utf(self._chunk(toc_off, b"TOC ")) if toc_off else []

    def _chunk(self, off, magic):
        self.f.seek(off)
        head = self.f.read(16)
        if head[:4] != magic:
            raise ValueError(f"{self.path}: expected {magic!r} at {off:#x}")
        size, = struct.unpack_from("<Q", head, 8)
        return self.f.read(size)

    def name(self, row):
        d = row.get("DirName") or ""
        return f"{d}/{row['FileName']}" if d else row["FileName"]

    def read(self, row):
        self.f.seek(self.base + row["FileOffset"])
        data = self.f.read(row["FileSize"])
        if data[:8] == b"CRILAYLA":
            data = decompress_crilayla(data)
        return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cmd", choices=["list", "extract"])
    ap.add_argument("cpk", nargs="+")
    ap.add_argument("--match", default="*", help="glob on dir/name (e.g. '*tree*')")
    ap.add_argument("--out", help="output directory for extract")
    args = ap.parse_args()

    # cmd.exe does not expand wildcards, so accept patterns like data\data*.cpk
    paths = [p for pattern in args.cpk for p in (sorted(glob.glob(pattern)) or [pattern])]

    for path in paths:
        cpk = Cpk(path)
        for row in cpk.files:
            name = cpk.name(row)
            if not fnmatch.fnmatch(name.lower(), args.match.lower()):
                continue
            if args.cmd == "list":
                print(f"{os.path.basename(path)}\t{name}\t{row['FileSize']}\t{row['ExtractSize']}")
            else:
                dest = os.path.join(args.out, name)
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                with open(dest, "wb") as o:
                    o.write(cpk.read(row))
                print(dest)


if __name__ == "__main__":
    sys.exit(main())
