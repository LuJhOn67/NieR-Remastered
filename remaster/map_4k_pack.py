"""Map a Special-K style texture pack (names = CRC32C of the original mip 0) to the DXVK fork's injection keys.

map_4k_pack.py PACK_TEXTURE_DIR OUT_JSON
Scans every original world/bg texture in work\raw (DAT .wta + DTT .wtp), and for each pack file whose name matches
crc32c(original mip 0) records: injection key (texture_key, 16 hex), original fourcc / size, container, pack format.
"""
import json
import os
import struct
import sys

import crc32c

sys.path.insert(0, os.path.dirname(__file__))
import dat  # noqa: E402
import wtp  # noqa: E402
from inject import first_mip, texture_key  # noqa: E402
from remaster import RAW  # noqa: E402


def pack_format(path):
    with open(path, "rb") as f:
        h = f.read(0x94)
    fmt = struct.unpack_from("<I", h, 0x80)[0] if h[0x54:0x58] == b"DX10" else h[0x54:0x58].decode("latin1")
    return fmt, struct.unpack_from("<I", h, 0x10)[0], struct.unpack_from("<I", h, 0x0C)[0]


def main():
    pack_dir, out = sys.argv[1], sys.argv[2]
    pack = {f[:8].upper(): f for f in os.listdir(pack_dir) if f.lower().endswith(".dds")}
    found = {}
    for dp, _, files in os.walk(RAW):
        for f in files:
            if not f.endswith(".dtt"):
                continue
            dtt = os.path.join(dp, f)
            dat_path = dtt[:-4] + ".dat"
            if not os.path.exists(dat_path):
                continue
            try:
                light = dat.read(dat_path)[0]
                heavy = dat.read(dtt)[0]
            except (ValueError, struct.error):
                continue
            wta = next((d for n, d in light if n.endswith(".wta")), None)
            wtp_data = next((d for n, d in heavy if n.endswith(".wtp")), None)
            if wta is None or wtp_data is None:
                continue
            _, textures = wtp.unpack(wta, wtp_data)
            for tid, src in textures.items():
                info = first_mip(src)
                if info is None:
                    continue
                fourcc, width, height, mip0 = info
                crc = f"{crc32c.crc32c(mip0):08X}"
                if crc in pack and crc not in found:
                    fmt, pw, ph = pack_format(os.path.join(pack_dir, pack[crc]))
                    found[crc] = {"key": f"{texture_key(mip0, width, height):016x}", "file": pack[crc],
                                  "orig": [fourcc.decode(), width, height], "pack": [fmt, pw, ph],
                                  "container": os.path.relpath(dtt, RAW)}
    json.dump(found, open(out, "w"), indent=1)
    print(f"{len(found)} of {len(pack)} pack textures matched an original texture")


if __name__ == "__main__":
    main()
