"""Build the nier_textures folder for texture replacement in the NieR Remastered DXVK fork.

The game reserves fixed memory for every world file, so upscaled textures cannot go into bigger loose
DAT/DTT files. Instead, the DXVK fork swaps textures when the game creates them. Each upscaled texture
from remaster.py's cache is written as <key>.dds, where the key identifies the original texture by its
first mip (see texture_key, which must match D3D11NierTextures::textureKey).
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dat  # noqa: E402
import wtp  # noqa: E402
from remaster import CACHE, RAW  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BLOCK_BYTES = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}
DDS_HEADER = 128
SAMPLE = 4096


def fnv1a64(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def texture_key(mip0, width, height):
    size = len(mip0)
    n = min(size, SAMPLE)
    mid = size // 2 - min(size // 2, SAMPLE // 2)
    h = fnv1a64(mip0[:n])
    h = fnv1a64(mip0[mid:mid + min(size - mid, SAMPLE)], h)
    h = fnv1a64(mip0[size - n:], h)
    return fnv1a64(width.to_bytes(4, "little") + height.to_bytes(4, "little"), h)


def first_mip(dds):
    """(fourcc, width, height, first mip bytes) of a legacy-header BC DDS, or None."""
    fourcc = dds[84:88]
    if dds[:4] != b"DDS " or fourcc not in BLOCK_BYTES:
        return None
    height, width = int.from_bytes(dds[12:16], "little"), int.from_bytes(dds[16:20], "little")
    size = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * BLOCK_BYTES[fourcc]
    return fourcc, width, height, dds[DDS_HEADER:DDS_HEADER + size]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", default=os.path.join(ROOT, "catalog.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "nier_textures"))
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    written = skipped = 0

    for entry in json.load(open(args.catalog)):
        # Every texture of the container that has an upscaled version in the cache
        ids = set(entry.get("textures", {}))
        if not ids:
            continue
        rel = entry["container"]
        light, _, _ = dat.read(os.path.join(RAW, rel + ".dat"))
        heavy, _, _ = dat.read(os.path.join(RAW, rel + ".dtt"))
        wta = next((d for n, d in light if n.endswith(".wta")), None)
        wtp_data = next((d for n, d in heavy if n.endswith(".wtp")), None)
        if wta is None or wtp_data is None:
            continue
        _, textures = wtp.unpack(wta, wtp_data)

        for tid in ids & set(textures):
            src = textures[tid]
            cached = os.path.join(CACHE, hashlib.sha1(src).hexdigest()[:20] + ".dds")
            info = first_mip(src)
            if info is None or not os.path.exists(cached):
                continue
            fourcc, width, height, mip0 = info
            dst = os.path.join(args.out, f"{texture_key(mip0, width, height):016x}.dds")
            if os.path.exists(dst):
                skipped += 1
                continue
            new = open(cached, "rb").read(DDS_HEADER)
            new_w, new_h = int.from_bytes(new[16:20], "little"), int.from_bytes(new[12:16], "little")
            if new[84:88] != fourcc or new_w * height != new_h * width:
                continue
            shutil.copyfile(cached, dst)
            written += 1

    print(f"{written} textures written to {args.out} ({skipped} shared)")


if __name__ == "__main__":
    main()
