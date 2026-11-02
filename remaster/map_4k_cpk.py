"""Like map_4k_pack.py, but reads the original textures straight from the game CPKs (every container:
pl / em / wp / ui / ... and the world), so the whole pack can be matched.

map_4k_cpk.py PACK_TEXTURE_DIR "CPK_GLOB" OUT_JSON [EXISTING_JSON]
Pairs <name>.dat (.wta) with <name>.dtt (.wtp) across all CPKs, and also reads stand-alone .wtb (header + data).
"""
import glob
import json
import os
import struct
import sys

import crc32c

sys.path.insert(0, os.path.dirname(__file__))
import dat  # noqa: E402
import wtp  # noqa: E402
from cpk import Cpk  # noqa: E402
from inject import first_mip, texture_key  # noqa: E402
from map_4k_pack import pack_format  # noqa: E402


def textures_of(light, heavy):
    out = {}
    wta = next((d for n, d in light if n.endswith(".wta")), None)
    wtp_data = next((d for n, d in heavy if n.endswith(".wtp")), None)
    if wta is not None and wtp_data is not None:
        try:
            out.update(wtp.unpack(wta, wtp_data)[1])
        except (struct.error, ValueError, IndexError):
            pass
    for n, d in light + heavy:
        if n.endswith(".wtb") and d[:4] == b"WTB\0":
            try:
                out.update({f"{n}:{k}": v for k, v in wtp.unpack(d, d)[1].items()})
            except (struct.error, ValueError, IndexError):
                pass
    return out


def main():
    pack_dir, cpk_glob, out = sys.argv[1:4]
    found = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else {}
    pack = {f[:8].upper(): f for f in os.listdir(pack_dir) if f.lower().endswith(".dds")}
    where = {}                                   # container name -> (cpk object, row) for .dat and .dtt
    cpks = [Cpk(p) for p in sorted(glob.glob(cpk_glob))]
    for c in cpks:
        for row in c.files:
            n = c.name(row)
            if n.endswith((".dat", ".dtt")):
                where.setdefault(n[:-4], {})[n[-3:]] = (c, row)
    print(len(where), "containers", flush=True)
    for i, (base, parts) in enumerate(sorted(where.items())):
        try:
            light = dat.parse(parts["dat"][0].read(parts["dat"][1]))[0] if "dat" in parts else []
            heavy = dat.parse(parts["dtt"][0].read(parts["dtt"][1]))[0] if "dtt" in parts else []
        except (ValueError, struct.error):
            continue
        for tid, src in textures_of(light, heavy).items():
            info = first_mip(src)
            if info is None:
                continue
            fourcc, width, height, mip0 = info
            crc = f"{crc32c.crc32c(mip0):08X}"
            if crc in pack and crc not in found:
                fmt, pw, ph = pack_format(os.path.join(pack_dir, pack[crc]))
                found[crc] = {"key": f"{texture_key(mip0, width, height):016x}", "file": pack[crc],
                              "orig": [fourcc.decode(), width, height], "pack": [fmt, pw, ph], "container": base}
        if i % 500 == 0:
            print(i, len(found), flush=True)
    json.dump(found, open(out, "w"), indent=1)
    print(f"{len(found)} of {len(pack)} pack textures matched")


if __name__ == "__main__":
    main()
