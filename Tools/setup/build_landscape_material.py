# Imports Tools/setup/textures and builds /Game/Summit/Materials/M_SummitLandscape (UnrealEditor-Cmd -run=pythonscript).
#
# Far away the landscape shows the real Sentinel-2 colour draped at true scale; close up it blends to tiled
# Poly Haven scans (snow, cliff rock, moraine) chosen by where the satellite sees snow and by slope, with normal
# and roughness maps. Rebuilding replaces the material in place, so maps that use it pick up the change.
import json
import os

import unreal

ROOT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
TEXTURES = os.path.join(ROOT, "Tools", "setup", "textures")
TERRAIN_DIR = os.path.join(ROOT, "Tools", "terrain", "output", "everest")
DEST = "/Game/Summit/Terrain"
MATERIAL = "/Game/Summit/Materials/M_SummitLandscape"
MEL = unreal.MaterialEditingLibrary
TILE_CM = {"snow": 400.0, "rock": 800.0, "moraine": 500.0}  # world size of one detail tile
NEAR_CM, FAR_CM = 2000.0, 30000.0                             # detail fades out between 20 m and 300 m

meta = json.load(open(next(os.path.join(TERRAIN_DIR, f) for f in os.listdir(TERRAIN_DIR) if f.endswith(".json"))))
extent_cm = meta["grid"]["extent_m"] * 100.0
origin = meta["ue_import"]["location_cm"]


def import_textures():
    tasks = []
    for name in sorted(os.listdir(TEXTURES)):
        if name.endswith(".png"):
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", os.path.join(TEXTURES, name))
            task.set_editor_property("destination_path", DEST)
            task.set_editor_property("automated", True)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("save", False)
            tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    textures = {}
    for task in tasks:
        name = os.path.splitext(os.path.basename(task.get_editor_property("filename")))[0]
        tex = unreal.load_asset(f"{DEST}/{name}")
        assert tex, f"import failed: {name}"
        if name.endswith("_nor"):
            tex.set_editor_property("srgb", False)
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        elif name.endswith("_rough"):
            tex.set_editor_property("srgb", False)
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS)
        elif name == "T_satellite":
            tex.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
            tex.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
            tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
        unreal.EditorAssetLibrary.save_loaded_asset(tex, only_if_is_dirty=False)
        textures[name] = tex
    return textures


class Graph:
    def __init__(self, mat):
        self.mat = mat
        self.y = 0

    def node(self, cls, **props):
        self.y += 120
        n = MEL.create_material_expression(self.mat, cls, -1600, self.y)
        for key, value in props.items():
            n.set_editor_property(key, value)
        return n

    def link(self, src, dst, dst_input="", src_output=""):
        assert MEL.connect_material_expressions(src, src_output, dst, dst_input), \
            f"connect {src.get_name()}.{src_output} -> {dst.get_name()}.{dst_input}"

    def op(self, cls, a, b=None, a_out="", b_out="", **props):
        n = self.node(cls, **props)
        self.link(a, n, "A", a_out)
        if b is not None:
            self.link(b, n, "B", b_out)
        return n

    def lerp(self, a, b, alpha):
        n = self.node(unreal.MaterialExpressionLinearInterpolate)
        self.link(a, n, "A")
        self.link(b, n, "B")
        self.link(alpha, n, "Alpha")
        return n

    def sat(self, x):
        n = self.node(unreal.MaterialExpressionSaturate)
        self.link(x, n)
        return n

    def ramp(self, x, offset, gain, x_out=""):
        """saturate((x - offset) * gain)"""
        sub = self.op(unreal.MaterialExpressionSubtract, x, a_out=x_out, const_b=offset)
        return self.sat(self.op(unreal.MaterialExpressionMultiply, sub, const_b=gain))

    def sample(self, tex, uv, sampler):
        n = self.node(unreal.MaterialExpressionTextureSample, texture=tex, sampler_type=sampler)
        self.link(uv, n, "UVs")
        return n


def build_material(tx):
    mat = unreal.load_asset(MATERIAL)
    if mat is None:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_SummitLandscape", "/Game/Summit/Materials", unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    g = Graph(mat)
    S = unreal.MaterialSamplerType

    pos = g.node(unreal.MaterialExpressionWorldPosition)
    xy = g.node(unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
    g.link(pos, xy)

    # Satellite UV: (world - landscape origin) / extent. Image row 0 is north = landscape min Y.
    shifted = g.op(unreal.MaterialExpressionSubtract, xy,
                   g.node(unreal.MaterialExpressionConstant2Vector, r=origin["x"], g=origin["y"]))
    sat_uv = g.op(unreal.MaterialExpressionMultiply, shifted, const_b=1.0 / extent_cm)
    satellite = g.sample(tx["T_satellite"], sat_uv, S.SAMPLERTYPE_COLOR)

    layers = {}
    for role, tile in TILE_CM.items():
        uv = g.op(unreal.MaterialExpressionMultiply, xy, const_b=1.0 / tile)
        layers[role] = (g.sample(tx[f"T_{role}_diff"], uv, S.SAMPLERTYPE_COLOR),
                        g.sample(tx[f"T_{role}_nor"], uv, S.SAMPLERTYPE_NORMAL),
                        g.sample(tx[f"T_{role}_rough"], uv, S.SAMPLERTYPE_MASKS))

    # Masks: steep faces shed snow and show rock; elsewhere snow follows what the satellite actually saw.
    normal_ws = g.node(unreal.MaterialExpressionVertexNormalWS)
    nz = g.node(unreal.MaterialExpressionComponentMask, r=False, g=False, b=True, a=False)
    g.link(normal_ws, nz)
    steep = g.ramp(nz, 0.80, -8.0)
    lum = g.op(unreal.MaterialExpressionDotProduct, satellite,
               g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.3, 0.59, 0.11, 0.0)),
               a_out="RGB")
    sat_snow = g.ramp(lum, 0.30, 4.0)
    holds_snow = g.ramp(nz, 0.65, 6.0)  # below ~50 degrees
    snow = g.op(unreal.MaterialExpressionMultiply, sat_snow, holds_snow)

    depth = g.node(unreal.MaterialExpressionPixelDepth)
    far = g.ramp(depth, NEAR_CM, 1.0 / (FAR_CM - NEAR_CM))

    def blend(index, red_only=False):
        """moraine -> rock by slope, then -> snow by the snow mask."""
        parts = [layers[role][index] for role in ("moraine", "rock", "snow")]
        if red_only:
            masked = []
            for part in parts:
                m = g.node(unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
                g.link(part, m)
                masked.append(m)
            parts = masked
        return g.lerp(g.lerp(parts[0], parts[1], steep), parts[2], snow)

    detail_color = blend(0)
    near_color = g.node(unreal.MaterialExpressionLinearInterpolate, const_alpha=0.6)  # 60 % scan detail, 40 % real colour
    g.link(satellite, near_color, "A", "RGB")
    g.link(detail_color, near_color, "B")
    base = g.node(unreal.MaterialExpressionLinearInterpolate)
    g.link(near_color, base, "A")
    g.link(satellite, base, "B", "RGB")
    g.link(far, base, "Alpha")
    assert MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    flat = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    normal = g.lerp(blend(1), flat, far)
    assert MEL.connect_material_property(normal, "", unreal.MaterialProperty.MP_NORMAL)

    rough = g.lerp(blend(2, red_only=True), g.node(unreal.MaterialExpressionConstant, r=0.8), far)
    assert MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


textures = import_textures()
material = build_material(textures)
print(f"SUMMIT_MATERIAL built {material.get_path_name()} with {len(textures)} textures, "
      f"{MEL.get_num_material_expressions(material)} nodes")
