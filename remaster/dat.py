"""PlatinumGames DAT/DTT container: list, extract, and repack (NieR:Automata)."""
import argparse
import os
import struct
import sys

HEADER = struct.Struct("<4s7I")  # magic, count, offsets, exts, names, sizes, hashmap, unknown


def parse(buf):
    """Return (entries, hashmap_bytes); entries are (name, data) in archive order."""
    magic, count, off_tbl, ext_tbl, name_tbl, size_tbl, hash_off, _ = HEADER.unpack_from(buf, 0)
    if magic != b"DAT\0":
        raise ValueError("not a DAT/DTT container")
    offsets = struct.unpack_from(f"<{count}I", buf, off_tbl)
    sizes = struct.unpack_from(f"<{count}I", buf, size_tbl)
    name_len, = struct.unpack_from("<I", buf, name_tbl)
    names = []
    for i in range(count):
        start = name_tbl + 4 + i * name_len
        names.append(buf[start:start + name_len].split(b"\0")[0].decode("ascii", "replace"))
    hashmap = buf[hash_off:off_tbl if off_tbl > hash_off else ext_tbl] if hash_off else b""
    return [(n, buf[o:o + s]) for n, o, s in zip(names, offsets, sizes)], hashmap, (off_tbl, ext_tbl, name_tbl, size_tbl, hash_off)


def read(path):
    with open(path, "rb") as f:
        return parse(f.read())


def _align(n, a):
    return (n + a - 1) // a * a


def build(entries, hashmap=b"", data_align=16):
    """Rebuild a container from (name, data) pairs, keeping the original hashmap block."""
    count = len(entries)
    name_len = _align(max(len(n) for n, _ in entries) + 1, 4)
    pos = HEADER.size
    off_tbl = pos; pos += 4 * count
    ext_tbl = pos; pos += 4 * count
    name_tbl = pos; pos += 4 + name_len * count
    pos = _align(pos, 4)
    size_tbl = pos; pos += 4 * count
    pos = _align(pos, 4)
    hash_off = pos if hashmap else 0
    pos += len(hashmap)

    offsets, blob = [], bytearray()
    pos = _align(pos, data_align)
    for _, data in entries:
        offsets.append(pos + len(blob))
        blob += data
        blob += b"\0" * (_align(len(blob), data_align) - len(blob))

    out = bytearray(HEADER.pack(b"DAT\0", count, off_tbl, ext_tbl, name_tbl, size_tbl, hash_off, 0))
    out += struct.pack(f"<{count}I", *offsets)
    for name, _ in entries:
        out += os.path.splitext(name)[1][1:].encode()[:3].ljust(4, b"\0")
    out += struct.pack("<I", name_len)
    for name, _ in entries:
        out += name.encode().ljust(name_len, b"\0")
    out += b"\0" * (size_tbl - len(out))
    out += struct.pack(f"<{count}I", *(len(d) for _, d in entries))
    out += b"\0" * ((hash_off or len(out)) - len(out))
    out += hashmap
    out += b"\0" * (_align(len(out), data_align) - len(out))
    out += blob
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("file")
    p = sub.add_parser("extract"); p.add_argument("file"); p.add_argument("out")
    p = sub.add_parser("roundtrip", help="rebuild a file unchanged and compare members")
    p.add_argument("file")
    args = ap.parse_args()

    entries, hashmap, _ = read(args.file)
    if args.cmd == "list":
        for name, data in entries:
            print(f"{name}\t{len(data)}")
    elif args.cmd == "extract":
        os.makedirs(args.out, exist_ok=True)
        for name, data in entries:
            with open(os.path.join(args.out, name), "wb") as o:
                o.write(data)
            print(os.path.join(args.out, name))
    else:
        rebuilt, _, _ = parse(build(entries, hashmap))
        ok = rebuilt == entries
        print("roundtrip OK" if ok else "roundtrip MISMATCH")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
