"""Build a catalog of every bg/wd model: materials, shaders (effects) and the textures each one uses."""
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dat  # noqa: E402
import wmb  # noqa: E402
import wtp  # noqa: E402

RAW = os.path.join(os.path.dirname(__file__), "..", "work", "raw")


def entries_by_ext(path):
    if not os.path.exists(path):
        return {}
    entries, _, _ = dat.read(path)
    return {os.path.splitext(n)[1]: d for n, d in entries}


def main(out_path):
    catalog = []
    for dtt in sorted(glob.glob(os.path.join(RAW, "*", "*.dtt"))):
        stem = dtt[:-4]
        rel = os.path.relpath(stem, RAW).replace("\\", "/")
        try:
            catalog.extend(describe(rel, dtt, stem + ".dat"))
        except (ValueError, IndexError, KeyError, struct.error) as e:
            catalog.append({"container": rel, "error": f"{type(e).__name__}: {e}"})
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(catalog, f, indent=0)
    print(f"{len(catalog)} models -> {out_path}")


def describe(rel, dtt, dat_path):
    heavy, light = entries_by_ext(dtt), entries_by_ext(dat_path)
    if ".wmb" not in heavy:
        return []
    model = wmb.parse(heavy[".wmb"])
    textures = {}
    if ".wta" in light and ".wtp" in heavy:
        table, dds = wtp.unpack(light[".wta"], heavy[".wtp"])
        for i, tid in enumerate(table.ids):
            w, h, mips, fourcc = wtp.dds_info(dds[tid])
            textures[tid] = {"w": w, "h": h, "mips": mips, "fourcc": fourcc,
                             "dxgi": table.dxgi_format(i), "roles": []}
    for m in model["materials"]:
        for t in m["textures"]:
            if t["id"] in textures and t["slot"] not in textures[t["id"]]["roles"]:
                textures[t["id"]]["roles"].append(t["slot"])
    return [{"container": rel, "mesh_groups": model["mesh_groups"],
             "materials": [{k: m[k] for k in ("name", "effect", "technique")} for m in model["materials"]],
             "textures": textures}]


if __name__ == "__main__":
    main(sys.argv[1])
