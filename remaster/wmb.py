"""Minimal WMB3 reader (NieR:Automata): mesh-group names and materials with their texture slots."""
import json
import struct
import sys


def _cstr(buf, off):
    return buf[off:buf.index(b"\0", off)].decode("ascii", "replace") if off else ""


def parse(buf):
    if buf[:4] != b"WMB3":
        raise ValueError("not WMB3")
    # 40-byte fixed header, then (offset, count) pairs
    pairs = struct.unpack_from("<24I", buf, 40)
    tables = dict(zip(
        ["bone", "bone_tt", "vertex_group", "mesh", "mesh_group_info", "col_tree",
         "bone_map", "boneset", "material", "mesh_group", "mesh_material", "world_data"],
        zip(pairs[0::2], pairs[1::2])))

    materials = []
    off, count = tables["material"]
    for i in range(count):
        base = off + i * 48
        name_o, effect_o, tech_o, _u, tex_o, tex_n = struct.unpack_from("<6I", buf, base + 8)
        textures = []
        for t in range(tex_n):
            slot_o, tex_id = struct.unpack_from("<II", buf, tex_o + t * 8)
            textures.append({"slot": _cstr(buf, slot_o), "id": f"{tex_id:08x}"})
        materials.append({"name": _cstr(buf, name_o), "effect": _cstr(buf, effect_o),
                          "technique": _cstr(buf, tech_o), "textures": textures})

    groups = []
    off, count = tables["mesh_group"]
    for i in range(count):
        # mesh group: nameOffset first; the record size is derived from the following table
        name_o, = struct.unpack_from("<I", buf, off + i * 0x2C)
        groups.append(_cstr(buf, name_o))
    return {"materials": materials, "mesh_groups": groups}


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as f:
        print(json.dumps(parse(f.read()), indent=1))
