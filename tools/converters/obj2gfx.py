#!/usr/bin/env python3
"""Convert a Wavefront OBJ (+MTL/PNG) into a compiled-in AC furniture model.

Emits a single C file containing GC-tiled texture data, an RGB5A3 palette,
an N64-format Vtx array and an emu64 Gfx display list using the repo's
Dolphin-extended F3DEX macros — the same shape as src/data/model/int_tak_nes01.c,
but as plain initialized arrays (no ISO asset loading), so it works for custom
assets on the PC build.

Coordinate/format contract (ground truth from the PC port):
  - Vtx literal rows are {x, y, z, flag, s, t, b0, b1, b2, b3} (tools/converters/vtxdis.py).
    Furniture uses the normal variant: b0..b2 = signed s8 normal bytes (lighting is
    normalized in pc/shaders/default.vert), b3 = alpha 255.
  - UVs are direct texel coordinates in s10.5 (1 texel = 32) with gsSPTexture(0,0,...)
    (emu64.c texture_matrix). V flipped: GC row 0 is the texture's top.
  - One room tile = 40 world units = 4000 model units at the standard profile
    scale of 0.01 (mFI_UNIT_BASE_SIZE; ac_my_room_draw.c_inc Matrix_scale).
  - CI8 texture data is GC-tiled in 8x4 blocks (pc/src/pc_gx_texture.c decode_CI8);
    RGB565 uses 4x4 blocks of big-endian u16 (decode_RGB565).
  - The palette is u16 RGB5A3 in NATIVE byte order: the Dolphin LOADTLUT path marks
    the slot little-endian on PC (emu64.c dl_G_LOADTLUT -> pc_gx_tlut_set_native_le).
  - Triangles are packed with gsSPNTrianglesInit_5b/gsSPNTriangles_5b: 5-bit indices,
    max 32 vertices per gsSPVertex batch, max 128 triangles per Init chain,
    padding slots repeat index 0.

Usage:
  python tools/converters/obj2gfx.py PSONE.obj --name int_psx \
      --out src/data/model/int_psx.c [--fit 3800] [--scale N] [--rotate-y 180]
"""
import argparse
import math
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

TILE_UNITS = 4000.0  # one 1x1 furniture tile in model units at profile scale 0.01


# ----------------------------------------------------------------------
# OBJ / MTL parsing
# ----------------------------------------------------------------------

def parse_mtl(path):
    """Return ({material_name: texture_path or None}, {material_name: (r,g,b)})."""
    materials = {}
    colors = {}
    if not os.path.exists(path):
        return materials, colors
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "newmtl":
            cur = parts[1]
            materials[cur] = None
            colors[cur] = (0.8, 0.8, 0.8)
        elif parts[0] == "map_Kd" and cur:
            materials[cur] = parts[-1]
        elif parts[0] == "Kd" and cur:
            try:
                colors[cur] = tuple(float(x) for x in parts[1:4])
            except ValueError:
                pass
    return materials, colors


def parse_obj(path):
    """Return (positions, uvs, normals, faces). Faces are (corners, material)
    where corners is a list of (v, vt, vn) 0-based indices (vt/vn may be None)."""
    positions, uvs, normals, faces = [], [], [], []
    materials = {}
    mtl_colors = {}
    cur_mtl = None
    obj_dir = os.path.dirname(os.path.abspath(path))

    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]
        if tag == "v":
            positions.append(tuple(float(x) for x in parts[1:4]))
        elif tag == "vt":
            uvs.append(tuple(float(x) for x in parts[1:3]))
        elif tag == "vn":
            normals.append(tuple(float(x) for x in parts[1:4]))
        elif tag == "mtllib":
            tex_map, color_map = parse_mtl(os.path.join(obj_dir, parts[1]))
            materials.update(tex_map)
            mtl_colors.update(color_map)
        elif tag == "usemtl":
            cur_mtl = parts[1]
        elif tag == "f":
            def resolve(idx, lst):
                # OBJ indices are 1-based; negative means relative to the
                # elements defined so far (-1 = most recent).
                if idx > 0 and idx <= len(lst):
                    return idx - 1
                if idx < 0 and -idx <= len(lst):
                    return len(lst) + idx
                sys.exit(f"{path}: invalid OBJ index {idx}")

            corners = []
            for corner in parts[1:]:
                bits = corner.split("/")
                v = int(bits[0])
                vt = int(bits[1]) if len(bits) > 1 and bits[1] else None
                vn = int(bits[2]) if len(bits) > 2 and bits[2] else None
                corners.append((resolve(v, positions),
                                resolve(vt, uvs) if vt is not None else None,
                                resolve(vn, normals) if vn is not None else None))
            # fan-triangulate
            for i in range(1, len(corners) - 1):
                faces.append(([corners[0], corners[i], corners[i + 1]], cur_mtl))

    tex_by_mtl = {m: t for m, t in materials.items()}
    return positions, uvs, normals, faces, tex_by_mtl, mtl_colors, obj_dir


# ----------------------------------------------------------------------
# Texture conversion
# ----------------------------------------------------------------------

def tile_ci8(indices, w, h):
    """Linear row-major index bytes -> GC 8x4-block tiled bytes."""
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 7) // 8):
            for y in range(4):
                for x in range(8):
                    px, py = bx * 8 + x, by * 4 + y
                    out.append(indices[py * w + px] if px < w and py < h else 0)
    return bytes(out)


def tile_rgb565(pixels, w, h):
    """RGB pixel rows -> GC 4x4-block tiled big-endian RGB565 bytes."""
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            for y in range(4):
                for x in range(4):
                    px, py = bx * 4 + x, by * 4 + y
                    if px < w and py < h:
                        r, g, b = pixels[py * w + px][:3]
                        val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                    else:
                        val = 0
                    out += val.to_bytes(2, "big")
    return bytes(out)


def rgb5a3_opaque(r, g, b):
    """Opaque RGB5A3 entry: bit15=1, RGB555."""
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def build_color_atlas(material_names, mtl_colors, fmt):
    """Untextured models: bake each material's diffuse colour into one cell of a
    small atlas so the normal single-texture pipeline still applies. Returns
    (texture_dict, {material_name: (u, v)}) with UVs at each cell's centre."""
    count = max(1, len(material_names))
    cells = 1
    while cells * cells < count:
        cells += 1
    cell_px = 8                      # solid blocks; centre-sampled, so size is
    size = max(8, cells * cell_px)   # only about staying off the cell seams
    size = (size + 3) // 4 * 4       # Dolphin SETTIMG needs height % 4 == 0

    pixels = [(0, 0, 0)] * (size * size)
    uv_by_mtl = {}
    for i, name in enumerate(material_names):
        cx, cy = (i % cells) * cell_px, (i // cells) * cell_px
        r, g, b = mtl_colors.get(name, (0.8, 0.8, 0.8))
        rgb = (max(0, min(255, int(round(r * 255)))),
               max(0, min(255, int(round(g * 255)))),
               max(0, min(255, int(round(b * 255)))))
        for y in range(cell_px):
            for x in range(cell_px):
                px, py = cx + x, cy + y
                if px < size and py < size:
                    pixels[py * size + px] = rgb
        uv_by_mtl[name] = ((cx + cell_px / 2.0) / size,
                           (cy + cell_px / 2.0) / size)

    if fmt == "ci8":
        seen, indices, palette = {}, [], []
        for p in pixels:
            if p not in seen:
                seen[p] = len(palette)
                palette.append(rgb5a3_opaque(*p))
            indices.append(seen[p])
        palette += [0] * (256 - len(palette))
        tex = {"w": size, "h": size, "fmt": "ci8",
               "data": tile_ci8(indices, size, size), "palette": palette[:256]}
    else:
        tex = {"w": size, "h": size, "fmt": "rgb565",
               "data": tile_rgb565(pixels, size, size), "palette": None}
    return tex, uv_by_mtl


def convert_texture(path, fmt):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    if w > 1024 or h > 1024:
        sys.exit(f"{path}: max texture size is 1024x1024")
    if h % 4:
        sys.exit(f"{path}: texture height must be a multiple of 4 (Dolphin SETTIMG packs h/4-1)")

    if fmt == "ci8":
        pal_im = im.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
        indices = list(pal_im.getdata())
        raw_pal = (pal_im.getpalette() + [0] * (256 * 3))[: 256 * 3]
        palette = [rgb5a3_opaque(raw_pal[i * 3], raw_pal[i * 3 + 1], raw_pal[i * 3 + 2])
                   for i in range(256)]
        return {"w": w, "h": h, "fmt": "ci8",
                "data": tile_ci8(indices, w, h), "palette": palette}
    else:  # rgb565
        return {"w": w, "h": h, "fmt": "rgb565",
                "data": tile_rgb565(list(im.getdata()), w, h), "palette": None}


# ----------------------------------------------------------------------
# Geometry
# ----------------------------------------------------------------------

def build_batches(faces, positions, uvs, normals, transform, tex_size, max_batch=32,
                  uv_by_mtl=None):
    """Greedy-batch triangles: each batch owns a contiguous Vtx slice of <=32
    entries indexed 0..31 by the 5-bit triangle commands. Returns (vtx_rows, batches)
    where each batch is {"base": int, "count": int, "tris": [(a,b,c), ...]}.
    When uv_by_mtl is given the model is untextured and every corner samples its
    material's colour cell instead of the OBJ's own UVs."""
    tw, th = tex_size
    vtx_rows = []
    batches = []
    batch = None

    def corner_key(c, mtl):
        # Colour-atlas models share positions across materials but need distinct
        # UVs per material, so the material is part of the vertex identity.
        return c if uv_by_mtl is None else (c, mtl)

    def emit_vertex(c, mtl):
        v, vt, vn = c
        x, y, z = transform(positions[v])
        if uv_by_mtl is not None:
            u, vv = uv_by_mtl.get(mtl, (0.5, 0.5))
        elif vt is not None:
            u, vv = uvs[vt]
        else:
            u, vv = 0.0, 0.0
        s = int(round(u * tw * 32))
        t = int(round((1.0 - vv) * th * 32))
        for val, what in ((x, "x"), (y, "y"), (z, "z"), (s, "s"), (t, "t")):
            if not -32768 <= val <= 32767:
                sys.exit(f"s16 overflow: {what}={val} (scale the model down)")
        if vn is not None:
            nx, ny, nz = normals[vn]
        else:
            nx, ny, nz = 0.0, 1.0, 0.0
        mag = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        nb = [max(-127, min(127, int(round(c / mag * 127)))) for c in (nx, ny, nz)]
        return (x, y, z, 0, s, t, nb[0] & 0xFF, nb[1] & 0xFF, nb[2] & 0xFF, 0xFF)

    for corners, mtl in faces:
        keys = [corner_key(c, mtl) for c in corners]
        if batch is None or len(set(keys) - set(batch["map"])) + len(batch["map"]) > max_batch \
                or len(batch["tris"]) >= 128:
            batch = {"base": len(vtx_rows), "map": {}, "tris": []}
            batches.append(batch)
        tri = []
        for c, key in zip(corners, keys):
            if key not in batch["map"]:
                batch["map"][key] = len(batch["map"])
                vtx_rows.append(emit_vertex(c, mtl))
            tri.append(batch["map"][key])
        batch["tris"].append(tuple(tri))

    for b in batches:
        b["count"] = len(b["map"])
        del b["map"]
    return vtx_rows, batches


def transform_factory(positions, fit, scale, rotate_y):
    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    cx = (min(xs) + max(xs)) / 2.0
    cz = (min(zs) + max(zs)) / 2.0
    floor = min(ys)

    rad = math.radians(rotate_y)
    cos_r, sin_r = math.cos(rad), math.sin(rad)

    if scale is None:
        extent = max(max(xs) - min(xs), max(zs) - min(zs)) or 1.0
        scale = fit / extent

    def transform(p):
        x, y, z = p[0] - cx, p[1] - floor, p[2] - cz
        xr = x * cos_r + z * sin_r
        zr = -x * sin_r + z * cos_r
        return (int(round(xr * scale)), int(round(y * scale)), int(round(zr * scale)))

    height = (max(ys) - floor) * scale
    return transform, scale, height


# ----------------------------------------------------------------------
# C emission
# ----------------------------------------------------------------------

def emit_tri_chain(lines, tris):
    """gsSPNTrianglesInit_5b carries 3 tris, each continuation carries 4;
    unused slots repeat index 0 (degenerate) like the stock models."""
    flat = [i for t in tris for i in t]
    head, rest = flat[:9], flat[9:]
    head += [0] * (9 - len(head))
    lines.append(f"    gsSPNTrianglesInit_5b({len(tris)}, "
                 + ", ".join(str(i) for i in head) + "),")
    for off in range(0, len(rest), 12):
        chunk = rest[off:off + 12]
        chunk += [0] * (12 - len(chunk))
        lines.append("    gsSPNTriangles_5b(" + ", ".join(str(i) for i in chunk) + "),")


def emit_c(name, tex, vtx_rows, batches, out_path, scale, height, src_names):
    lines = [
        f"/* Generated by tools/converters/obj2gfx.py from {' '.join(src_names)}.",
        f" * {len(vtx_rows)} vertices, {sum(len(b['tris']) for b in batches)} triangles,",
        f" * {len(batches)} vertex batches, source scale {scale:.2f} units/unit,",
        f" * model height {height:.0f} units ({height * 0.01:.1f} world units at profile scale 0.01).",
        " * Do not edit by hand — re-run the converter. */",
        '#include "libforest/gbi_extensions.h"',
        '#include "PR/gbi.h"',
        '#include "dolphin/gx.h"',
        "",
    ]

    data = tex["data"]
    lines.append(f"u8 {name}_tex[{len(data)}] = {{")
    for off in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[off:off + 16]) + ",")
    lines.append("};")
    lines.append("")

    if tex["palette"] is not None:
        lines.append(f"u16 {name}_pal[256] = {{")
        for off in range(0, 256, 8):
            lines.append("    " + ", ".join(f"0x{v:04X}" for v in tex["palette"][off:off + 8]) + ",")
        lines.append("};")
        lines.append("")

    lines.append(f"Vtx {name}_v[] = {{")
    for row in vtx_rows:
        lines.append("    { " + ", ".join(str(v) for v in row) + " },")
    lines.append("};")
    lines.append("")

    lines.append(f"Gfx {name}_model[] = {{")
    lines.append("    gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_ON),")
    lines.append("    gsDPSetCombineLERP(TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0, "
                 "PRIMITIVE, 0, COMBINED, 0, 0, 0, 0, COMBINED),")
    lines.append("    gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_AA_ZB_TEX_EDGE2),")
    if tex["fmt"] == "ci8":
        lines.append(f"    gsDPLoadTLUT_Dolphin(15, 256, 1, {name}_pal),")
        lines.append(f"    gsDPSetTextureImage_Dolphin(G_IM_FMT_CI, G_IM_SIZ_8b, "
                     f"{tex['w']}, {tex['h']}, {name}_tex),")
    else:
        lines.append(f"    gsDPSetTextureImage_Dolphin(G_IM_FMT_I, G_IM_SIZ_16b, "
                     f"{tex['w']}, {tex['h']}, {name}_tex),")
    lines.append("    gsDPSetTile_Dolphin(G_DOLPHIN_TLUT_DEFAULT_MODE, 0, 15, "
                 "GX_CLAMP, GX_CLAMP, 0, 0),")
    lines.append("    gsDPSetPrimColor(0, 128, 255, 255, 255, 255),")
    lines.append("    gsSPLoadGeometryMode(G_ZBUFFER | G_SHADE | G_CULL_BACK | G_FOG | "
                 "G_LIGHTING | G_SHADING_SMOOTH | G_DECAL_LEQUAL),")
    for b in batches:
        lines.append(f"    gsSPVertex(&{name}_v[{b['base']}], {b['count']}, 0),")
        emit_tri_chain(lines, b["tris"])
    lines.append("    gsSPEndDisplayList(),")
    lines.append("};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("obj", help="input .obj (with .mtl + texture next to it)")
    ap.add_argument("--name", required=True, help="C symbol prefix, e.g. int_psx")
    ap.add_argument("--out", required=True, help="output .c path")
    ap.add_argument("--fit", type=float, default=3800.0,
                    help="auto-scale so the larger XZ extent equals this many model "
                         "units (default 3800 = 95%% of a 1x1 tile)")
    ap.add_argument("--scale", type=float, default=None,
                    help="explicit units-per-OBJ-unit scale (overrides --fit)")
    ap.add_argument("--rotate-y", type=float, default=0.0,
                    help="pre-rotation around Y in degrees")
    ap.add_argument("--tex-format", choices=["ci8", "rgb565"], default="ci8")
    args = ap.parse_args()

    positions, uvs, normals, faces, tex_by_mtl, mtl_colors, obj_dir = parse_obj(args.obj)
    if not faces:
        sys.exit("no faces in OBJ")

    tex_paths = {t for t in tex_by_mtl.values() if t}
    if len(tex_paths) > 1:
        sys.exit(f"model uses {len(tex_paths)} textures; single-texture models only for now")

    uv_by_mtl = None
    if tex_paths:
        tex_rel = tex_paths.pop()
        tex_path = tex_rel if os.path.isabs(tex_rel) else os.path.join(obj_dir, tex_rel)
        if not os.path.exists(tex_path):
            # Blender often writes bare filenames; look in common subdirs
            for sub in ("", "textures", "texture"):
                cand = os.path.join(obj_dir, sub, os.path.basename(tex_rel))
                if os.path.exists(cand):
                    tex_path = cand
                    break
            else:
                sys.exit(f"texture not found: {tex_rel}")
        tex = convert_texture(tex_path, args.tex_format)
        source_note = os.path.basename(tex_path)
    else:
        # Untextured: bake per-material diffuse colours into a small atlas.
        used = []
        for _corners, mtl in faces:
            if mtl is not None and mtl not in used:
                used.append(mtl)
        if not used:
            sys.exit("no textures and no materials in OBJ — nothing to shade with")
        tex, uv_by_mtl = build_color_atlas(used, mtl_colors, args.tex_format)
        source_note = f"{len(used)} material colours"

    transform, scale, height = transform_factory(positions, args.fit, args.scale, args.rotate_y)
    vtx_rows, batches = build_batches(faces, positions, uvs, normals, transform,
                                      (tex["w"], tex["h"]), uv_by_mtl=uv_by_mtl)

    emit_c(args.name, tex, vtx_rows, batches, args.out, scale, height,
           [os.path.basename(args.obj), source_note])

    tris = sum(len(b["tris"]) for b in batches)
    print(f"{args.out}: {len(vtx_rows)} Vtx ({len(vtx_rows) * 16} B), {tris} tris, "
          f"{len(batches)} batches, tex {tex['w']}x{tex['h']} {tex['fmt']} "
          f"({len(tex['data'])} B), height {height:.0f} units "
          f"({height * 0.01:.1f} world), footprint scale {scale:.1f}")
    print(f"externs:\n  extern u8 {args.name}_tex[];"
          + (f"\n  extern u16 {args.name}_pal[];" if tex["palette"] else "")
          + f"\n  extern Vtx {args.name}_v[];\n  extern Gfx {args.name}_model[];")


if __name__ == "__main__":
    main()
