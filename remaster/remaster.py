"""AI-upscale albedo textures of bg/wd models and write loose DAT/DTT files the game loads from data/.

Albedo textures (g_AlbedoMap*) go through Real-ESRGAN x4, then are downsampled to the target size and
re-encoded to the original BC format with a full mip chain. Normal/mask/light maps are left untouched.
Results are cached by source hash, so shared textures are only processed once.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(__file__))
import dat  # noqa: E402
import wtp  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RAW = os.path.join(ROOT, "work", "raw")
CACHE = os.path.join(ROOT, "work", "upscaled")
TMP = os.path.join(ROOT, "work", "tmp")
TEXCONV = os.path.join(ROOT, "tools", "texconv", "texconv.exe")
ESRGAN_DIR = os.path.join(ROOT, "tools", "realesrgan", "x")
MAX_SIZE = 4096
FORMATS = {"DXT1": "BC1_UNORM", "DXT3": "BC2_UNORM", "DXT5": "BC3_UNORM"}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{os.path.basename(cmd[0])} failed: {r.stdout[-400:]} {r.stderr[-400:]}")


def target_size(w, h):
    scale = 4 if max(w, h) <= 256 else 2
    while max(w, h) * scale > MAX_SIZE and scale > 1:
        scale //= 2
    return w * scale, h * scale


def color_fix(src_png, big_png, out_png, radius=8):
    """Keep the original's low-frequency color/brightness and take only fine detail from the ESRGAN output
    (Real-ESRGAN denoises BC artifacts and darkens textures by ~20% otherwise)."""
    big = Image.open(big_png).convert("RGBA")
    src_up = Image.open(src_png).convert("RGBA").resize(big.size, Image.BICUBIC)

    def low(img):
        return np.asarray(img.convert("RGB").filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32)

    rgb = np.asarray(big.convert("RGB"), dtype=np.float32) - low(big) + low(src_up)
    out = np.dstack([np.clip(rgb + 0.5, 0, 255).astype(np.uint8), np.asarray(big)[..., 3]])
    Image.fromarray(out, "RGBA").save(out_png)


def upscale_dds(src, gpu):
    """Return upscaled DDS bytes for an albedo texture (cached), or None if it should stay as-is."""
    w, h, _mips, fourcc = wtp.dds_info(src)
    if fourcc not in FORMATS:
        return None
    tw, th = target_size(w, h)
    if (tw, th) == (w, h):
        return None
    key = hashlib.sha1(src).hexdigest()[:20]
    cached = os.path.join(CACHE, key + ".dds")
    if os.path.exists(cached):
        return open(cached, "rb").read()

    os.makedirs(TMP, exist_ok=True)
    os.makedirs(CACHE, exist_ok=True)
    src_dds = os.path.join(TMP, key + ".dds")
    with open(src_dds, "wb") as f:
        f.write(src)
    run([TEXCONV, "-nologo", "-y", "-ft", "png", "-m", "1", "-o", TMP, src_dds])
    src_png = os.path.join(TMP, key + ".png")
    big_png = os.path.join(TMP, key + "_x4.png")
    run([os.path.join(ESRGAN_DIR, "realesrgan-ncnn-vulkan.exe"), "-i", src_png, "-o", big_png,
         "-n", "realesrgan-x4plus", "-s", "4", "-g", str(gpu), "-m", os.path.join(ESRGAN_DIR, "models")])

    # alpha-tested cutouts (leaves, fences): keep mip coverage so foliage doesn't thin out at distance
    alpha = np.asarray(Image.open(src_png))[..., 3]
    cutout = alpha.min() < 128 and np.isin(alpha, (0, 255)).mean() > 0.9

    out_png = os.path.join(TMP, key + "_out.png")
    color_fix(src_png, big_png, out_png)
    os.remove(big_png)
    cmd = [TEXCONV, "-nologo", "-y", "--ignore-srgb", "-f", FORMATS[fourcc], "-w", str(tw), "-h", str(th),
           "-m", "0", "-if", "FANT", "-sepalpha", "-o", TMP, out_png]
    if cutout:
        cmd[1:1] = ["--keep-coverage", "0.5"]
    run(cmd)
    os.replace(os.path.join(TMP, key + "_out.dds"), cached)
    for p in (src_dds, src_png, out_png):
        if os.path.exists(p):
            os.remove(p)
    return open(cached, "rb").read()


def albedo_ids(entry):
    return {tid for tid, t in entry["textures"].items() if any(r.startswith("g_AlbedoMap") for r in t["roles"])}


def remaster(entry, out_root, gpu, budget=None):
    """Upscale a container's albedo textures. With a budget, the DTT may grow to at most budget times
    its original size: the game reserves fixed memory per world area and fails to load bigger files
    ("Ran out of memory while loading"). The smallest (blurriest) textures are upscaled first."""
    rel = entry["container"]
    if not albedo_ids(entry):
        return 0
    dat_path, dtt_path = os.path.join(RAW, rel + ".dat"), os.path.join(RAW, rel + ".dtt")
    light, light_hash, _ = dat.read(dat_path)
    heavy, heavy_hash, _ = dat.read(dtt_path)
    wta_i = next((i for i, (n, _) in enumerate(light) if n.endswith(".wta")), None)
    wtp_i = next((i for i, (n, _) in enumerate(heavy) if n.endswith(".wtp")), None)
    if wta_i is None or wtp_i is None:
        return 0
    table, textures = wtp.unpack(light[wta_i][1], heavy[wtp_i][1])

    changed = 0
    limit = os.path.getsize(dtt_path) * budget if budget else None
    size = os.path.getsize(dtt_path)
    for tid in sorted(albedo_ids(entry) & set(textures), key=lambda t: len(textures[t])):
        new = upscale_dds(textures[tid], gpu)
        if new is None:
            continue
        growth = len(new) - len(textures[tid])
        if limit is not None and size + growth > limit:
            continue
        textures[tid] = new
        size += growth
        changed += 1
    if not changed:
        return 0

    wta_bytes, wtp_bytes = wtp.repack(table, textures)
    light[wta_i] = (light[wta_i][0], wta_bytes)
    heavy[wtp_i] = (heavy[wtp_i][0], wtp_bytes)
    out = os.path.join(out_root, rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out + ".dat", "wb") as f:
        f.write(dat.build(light, light_hash))
    with open(out + ".dtt", "wb") as f:
        f.write(dat.build(heavy, heavy_hash))
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", default=os.path.join(ROOT, "catalog.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "phase2-remaster", "data"))
    ap.add_argument("--only", nargs="*", help="container names, e.g. bg/bg0055")
    ap.add_argument("--kind", choices=["plants", "structures", "all"], default="all")
    ap.add_argument("--gpu", type=int, default=0)
    ap.add_argument("--budget", type=float, default=None,
                    help="max DTT size as a multiple of the original, e.g. 1.25 (default: no limit)")
    args = ap.parse_args()

    entries = [e for e in json.load(open(args.catalog)) if "textures" in e]
    if args.only:
        entries = [e for e in entries if e["container"] in args.only]
    if args.kind != "all":
        def is_plant(e):
            return any(m["effect"].startswith("Plt") for m in e["materials"])
        entries = [e for e in entries if is_plant(e) == (args.kind == "plants")]

    for n, e in enumerate(entries, 1):
        try:
            count = remaster(e, args.out, args.gpu, args.budget)
            print(f"[{n}/{len(entries)}] {e['container']}: {count} textures upscaled", flush=True)
        except Exception as ex:  # keep the batch going; report the failure
            print(f"[{n}/{len(entries)}] {e['container']}: FAILED {ex}", flush=True)


if __name__ == "__main__":
    main()
