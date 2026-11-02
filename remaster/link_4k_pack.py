"""Put the matched files of a 4K texture pack into <game>\\nier_textures under their replacement keys.

link_4k_pack.py PACK_TEXTURE_DIR MAP_JSON GAME_DIR
MAP_JSON comes from map_4k_cpk.py. Uses hard links when the pack and the game are on the same drive (no extra
space), copies otherwise. Uncompressed pack files are skipped (only BC1-3 and BC7 are supported).
"""
import json
import os
import shutil
import sys

SUPPORTED = {70, 71, 72, 73, 74, 75, 76, 77, 78, 97, 98, 99}   # DXGI BC1-3 and BC7


def main():
    pack_dir, map_json, game = sys.argv[1:4]
    out = os.path.join(game, "nier_textures")
    os.makedirs(out, exist_ok=True)
    linked = copied = skipped = 0
    for entry in json.load(open(map_json)).values():
        if entry["pack"][0] not in SUPPORTED:
            skipped += 1
            continue
        src = os.path.join(pack_dir, entry["file"])
        dst = os.path.join(out, entry["key"] + ".dds")
        if os.path.exists(dst):
            continue
        try:
            os.link(src, dst)
            linked += 1
        except OSError:
            shutil.copyfile(src, dst)
            copied += 1
    print(f"{linked} linked, {copied} copied, {skipped} skipped -> {out}")


if __name__ == "__main__":
    main()
