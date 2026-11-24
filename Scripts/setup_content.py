"""
Imports the game's art into the Unreal project and creates the assets the C++ code loads.
Run headless from the project folder:

  "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ^
      RacingSim.uproject -run=pythonscript -script="Scripts/setup_content.py" -unattended -nosplash

It is safe to run again: existing assets are replaced. The source files are in SourceArt/:

  SourceArt/Models, SourceArt/Textures   glb models and PNGs (from the Java game's src/main/resources)
  SourceArt/Liveries                     painted liveries and helmets (the Java game's `gradlew bakeLiveries`)
  SourceArt/Generated                    sponsor boards, smoke puffs, tire and body detail maps (`java Scripts/BakeTextures.java SourceArt/Generated`)

  /Game/Textures/{Track,Car,Props,UI,Sky}/*   PNGs
  /Game/Textures/Liveries/T_{Livery,Helmet}_N  painted cars
  /Game/Textures/Generated/*                   banner, smoke puffs, tire and body detail maps
  /Game/Models/{Car,Props}/*                   glb models (one static mesh each)
  /Game/Materials/*                            M_Surface(+TwoSided), M_Fence, M_Sky, M_Smoke
  /Game/Maps/Oval                              empty level; the game mode spawns everything
"""
import os
import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ART = os.path.join(PROJECT_DIR, "SourceArt")
RES = ART

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary


def log(msg):
    unreal.log("[setup_content] " + msg)


# ------------------------------------------------------------------------------- textures

def import_textures(src_dir, dest):
    tasks = []
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith(".png"):
            continue
        t = unreal.AssetImportTask()
        t.filename = os.path.join(src_dir, name)
        t.destination_path = dest
        t.automated = True
        t.replace_existing = True
        t.save = False
        tasks.append(t)
    asset_tools.import_asset_tasks(tasks)
    for t in tasks:
        name = os.path.splitext(os.path.basename(t.filename))[0]
        tex = unreal.load_asset(dest + "/" + name)
        if tex is None:
            log("FAILED texture " + name)
            continue
        if name.endswith("_normal"):
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
            tex.set_editor_property("srgb", False)
        elif name.endswith("_rough"):
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
            tex.set_editor_property("srgb", False)
        eal.save_loaded_asset(tex)
        log("texture " + dest + "/" + name)


def import_liveries(src_dir, dest):
    """Baked liveries from `gradlew bakeLiveries`: N-name-livery[-2048].png -> T_Livery_N (T_Helmet_N)."""
    import re
    if not os.path.isdir(src_dir):
        log("FAILED liveries: %s missing (bake them with the Java game's `gradlew bakeLiveries`)" % src_dir)
        return
    tasks = []
    for name in sorted(os.listdir(src_dir)):
        m = re.match(r"(\d+)-.*-(livery|helmet)(-\d+)?\.png$", name)
        if not m:
            continue
        t = unreal.AssetImportTask()
        t.filename = os.path.join(src_dir, name)
        t.destination_path = dest
        t.destination_name = "T_%s_%s" % (m.group(2).capitalize(), m.group(1))
        t.automated = True
        t.replace_existing = True
        t.save = True
        tasks.append(t)
    asset_tools.import_asset_tasks(tasks)
    log("liveries: %d textures -> %s" % (len(tasks), dest))


# --------------------------------------------------------------------------------- models

def gltf_pipeline():
    """The default glTF pipeline, set to combine each file into one static mesh."""
    # an in-memory pipeline (the engine's pipeline assets are not in the commandlet's asset registry)
    p = unreal.new_object(unreal.InterchangeGenericAssetsPipeline, name="GLTFCombined")
    mesh = p.get_editor_property("mesh_pipeline")
    mesh.set_editor_property("combine_static_meshes_behavior", unreal.InterchangeCombineStaticMeshesBehavior.ALL)
    mesh.set_editor_property("collision", False)
    mesh.set_editor_property("import_skeletal_meshes", False)
    global _pipeline_keepalive
    _pipeline_keepalive = p
    return p.get_path_name()


def import_models(src_dir, dest, pipeline):
    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith(".glb"):
            continue
        base = os.path.splitext(name)[0]
        folder = dest + "/" + base
        if eal.does_directory_exist(folder):
            eal.delete_directory(folder)
        params = unreal.ImportAssetParameters()
        params.is_automated = True
        params.replace_existing = True
        params.override_pipelines.append(unreal.SoftObjectPath(pipeline))
        source = unreal.InterchangeManager.create_source_data(os.path.join(src_dir, name))
        manager.import_asset(folder, source, params)
        # move the combined mesh to dest/<base> so the code can load it by a fixed path
        meshes = [a for a in eal.list_assets(folder, recursive=True)
                  if unreal.EditorAssetLibrary.find_asset_data(a).asset_class_path.asset_name == "StaticMesh"]
        if not meshes:
            log("FAILED model " + name)
            continue
        target = dest + "/" + base + "_mesh"
        if eal.does_asset_exist(target):
            eal.delete_asset(target)
        eal.rename_asset(meshes[0].split(".")[0], target)
        sm = unreal.load_asset(target)
        bounds = sm.get_bounding_box()
        slots = [str(m.get_editor_property("material_slot_name")) for m in sm.get_editor_property("static_materials")]
        log("model %s -> %s  bounds min=%s max=%s  slots=%s" % (name, target, bounds.min, bounds.max, slots))
        eal.save_loaded_asset(sm)
        # the materials and textures the import made live in dest/<base>/
        eal.save_directory(folder, only_if_is_dirty=False, recursive=True)


# ------------------------------------------------------------------------------ materials

def new_material(path):
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    folder, name = path.rsplit("/", 1)
    return asset_tools.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())


def tex_param(mat, name, default, sampler, x, y):
    n = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("texture", unreal.load_asset(default))
    n.set_editor_property("sampler_type", sampler)
    return n


def scalar(mat, name, value, x, y):
    n = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", value)
    return n


def mult(mat, a, a_out, b, b_out, x, y):
    n = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, x, y)
    mel.connect_material_expressions(a, a_out, n, "A")
    mel.connect_material_expressions(b, b_out, n, "B")
    return n


def lerp_one(mat, value, value_out, alpha, x, y, one=(1.0, 1.0, 1.0)):
    """lerp(constant, value, alpha): the texture is ignored while its Use* switch is 0."""
    c = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, x - 200, y)
    c.set_editor_property("constant", unreal.LinearColor(one[0], one[1], one[2], 1.0))
    n = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
    mel.connect_material_expressions(c, "", n, "A")
    mel.connect_material_expressions(value, value_out, n, "B")
    mel.connect_material_expressions(alpha, "", n, "Alpha")
    return n


def make_surface_material(path="/Game/Materials/M_Surface", two_sided=False):
    m = new_material(path)
    m.set_editor_property("two_sided", two_sided)
    m.set_editor_property("used_with_instanced_static_meshes", True)   # trees and props
    albedo = tex_param(m, "Albedo", "/Game/Textures/Track/asphalt_albedo", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -1200, -300)
    normal = tex_param(m, "Normal", "/Game/Textures/Track/asphalt_normal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, -1200, 100)
    rough = tex_param(m, "RoughTex", "/Game/Textures/Track/asphalt_rough", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, -1200, 400)
    use_albedo = scalar(m, "UseAlbedo", 0.0, -900, -450)
    use_normal = scalar(m, "UseNormal", 0.0, -900, 0)
    use_rough = scalar(m, "UseRoughTex", 0.0, -900, 550)
    tint = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -900, -200)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    vcol = mel.create_material_expression(m, unreal.MaterialExpressionVertexColor, -900, -100)

    base = lerp_one(m, albedo, "RGB", use_albedo, -600, -350)
    base = mult(m, base, "", vcol, "", -400, -300)
    base = mult(m, base, "", tint, "", -200, -300)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    nrm = lerp_one(m, normal, "RGB", use_normal, -600, 100, one=(0.0, 0.0, 1.0))
    mel.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)

    roughness = scalar(m, "Roughness", 1.0, -600, 450)
    r = lerp_one(m, rough, "R", use_rough, -600, 600)
    r = mult(m, r, "", roughness, "", -300, 500)
    mel.connect_material_property(r, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # ambient occlusion packed in the roughness texture's green channel (car body seams)
    use_ao = scalar(m, "UseAO", 0.0, -600, 800)
    one = mel.create_material_expression(m, unreal.MaterialExpressionConstant, -600, 900)
    one.set_editor_property("r", 1.0)
    ao = mel.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 850)
    mel.connect_material_expressions(one, "", ao, "A")
    mel.connect_material_expressions(rough, "G", ao, "B")
    mel.connect_material_expressions(use_ao, "", ao, "Alpha")
    mel.connect_material_property(ao, "", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
    mel.connect_material_property(scalar(m, "Metallic", 0.0, -300, 300), "", unreal.MaterialProperty.MP_METALLIC)
    emissive = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -300, 700)
    emissive.set_editor_property("parameter_name", "Emissive")
    emissive.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 1))
    mel.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    log("material " + path)


def make_fence_material():
    m = new_material("/Game/Materials/M_Fence")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("opacity_mask_clip_value", 0.35)
    m.set_editor_property("used_with_instanced_static_meshes", True)   # leaves
    albedo = tex_param(m, "Albedo", "/Game/Textures/Track/chainlink", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -900, -200)
    tint = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -900, 100)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    base = mult(m, albedo, "RGB", tint, "", -500, -200)
    vcol = mel.create_material_expression(m, unreal.MaterialExpressionVertexColor, -700, 0)
    base = mult(m, base, "", vcol, "", -300, -200)   # leaf cards carry ambient occlusion in their vertex colors
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(albedo, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    emissive = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -500, 500)
    emissive.set_editor_property("parameter_name", "Emissive")
    emissive.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 1))
    mel.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(scalar(m, "Roughness", 0.4, -500, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(scalar(m, "Metallic", 0.8, -500, 350), "", unreal.MaterialProperty.MP_METALLIC)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    log("material /Game/Materials/M_Fence")


def make_sky_material():
    """Unlit sky dome: the equirect golden-hour sky (blender/make_sky.py) sampled by view direction.
    Blender's equirect u = 0.5 - azimuth / 2 pi becomes 0.5 + atan2(y, x) / 2 pi in Unreal axes."""
    m = new_material("/Game/Materials/M_Sky")
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("is_sky", True)
    cam = mel.create_material_expression(m, unreal.MaterialExpressionCameraVectorWS, -1200, 0)
    neg = mel.create_material_expression(m, unreal.MaterialExpressionMultiply, -1000, 0)
    mel.connect_material_expressions(cam, "", neg, "A")
    neg.set_editor_property("const_b", -1.0)
    nrm = mel.create_material_expression(m, unreal.MaterialExpressionNormalize, -900, 0)
    mel.connect_material_expressions(neg, "", nrm, "VectorInput")

    def mask(r, g, b, y):
        n = mel.create_material_expression(m, unreal.MaterialExpressionComponentMask, -750, y)
        n.set_editor_property("r", r)
        n.set_editor_property("g", g)
        n.set_editor_property("b", b)
        mel.connect_material_expressions(nrm, "", n, "")
        return n
    dx, dy, dz = mask(True, False, False, -100), mask(False, True, False, 0), mask(False, False, True, 150)
    atan = mel.create_material_expression(m, unreal.MaterialExpressionArctangent2, -600, -50)
    mel.connect_material_expressions(dy, "", atan, "Y")
    mel.connect_material_expressions(dx, "", atan, "X")
    u = mel.create_material_expression(m, unreal.MaterialExpressionMultiply, -500, -50)
    mel.connect_material_expressions(atan, "", u, "A")
    u.set_editor_property("const_b", 1.0 / 6.2831853)
    u2 = mel.create_material_expression(m, unreal.MaterialExpressionAdd, -420, -50)
    mel.connect_material_expressions(u, "", u2, "A")
    u2.set_editor_property("const_b", 0.5)
    clampz = mel.create_material_expression(m, unreal.MaterialExpressionClamp, -600, 150)
    mel.connect_material_expressions(dz, "", clampz, "")
    clampz.set_editor_property("min_default", -1.0)
    clampz.set_editor_property("max_default", 1.0)
    acos = mel.create_material_expression(m, unreal.MaterialExpressionArccosine, -500, 150)
    mel.connect_material_expressions(clampz, "", acos, "")
    v = mel.create_material_expression(m, unreal.MaterialExpressionMultiply, -420, 150)
    mel.connect_material_expressions(acos, "", v, "A")
    v.set_editor_property("const_b", 1.0 / 3.1415927)
    code = mel.create_material_expression(m, unreal.MaterialExpressionAppendVector, -330, 50)
    mel.connect_material_expressions(u2, "", code, "A")
    mel.connect_material_expressions(v, "", code, "B")
    tex = tex_param(m, "Sky", "/Game/Textures/Sky/sky_sunset", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -500, 0)
    mel.connect_material_expressions(code, "", tex, "UVs")
    bright = scalar(m, "Brightness", 1.0, -500, 300)
    e = mult(m, tex, "RGB", bright, "", -250, 0)
    mel.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    log("material /Game/Materials/M_Sky")


def make_smoke_material():
    """Tire smoke puffs: translucent camera-facing quads; per-instance custom data 0 = opacity,
    1 = cell of the 2 x 2 puff atlas (TireSmoke.puffAtlas)."""
    m = new_material("/Game/Materials/M_Smoke")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)
    uv = mel.create_material_expression(m, unreal.MaterialExpressionTextureCoordinate, -1300, 0)
    cell = mel.create_material_expression(m, unreal.MaterialExpressionPerInstanceCustomData, -1300, 200)
    cell.set_editor_property("data_index", 1)
    alpha = mel.create_material_expression(m, unreal.MaterialExpressionPerInstanceCustomData, -600, 400)
    alpha.set_editor_property("data_index", 0)
    code = mel.create_material_expression(m, unreal.MaterialExpressionCustom, -1000, 0)
    code.set_editor_property("code", "float c = floor(Cell + 0.5);\nreturn (UV + float2(fmod(c, 2.0), floor(c / 2.0))) * 0.5;")
    code.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    i1 = unreal.CustomInput(); i1.set_editor_property("input_name", "UV")
    i2 = unreal.CustomInput(); i2.set_editor_property("input_name", "Cell")
    code.set_editor_property("inputs", [i1, i2])
    mel.connect_material_expressions(uv, "", code, "UV")
    mel.connect_material_expressions(cell, "", code, "Cell")
    tex = tex_param(m, "Puffs", "/Game/Textures/Generated/smoke_puffs", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -700, 0)
    mel.connect_material_expressions(code, "", tex, "UVs")
    tint = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -700, -250)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.80, 0.80, 0.80, 1))
    base = mult(m, tex, "RGB", tint, "", -400, -100)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    op = mult(m, tex, "A", alpha, "", -300, 300)
    mel.connect_material_property(op, "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    log("material /Game/Materials/M_Smoke")


def import_generated(src_dir, dest):
    """Textures the Java game paints at start-up, baked by Scripts/BakeTextures.java."""
    if not os.path.isdir(src_dir):
        log("FAILED generated textures: run `java Scripts/BakeTextures.java SourceArt/Generated` first")
        return
    import_textures(src_dir, dest)


def sky_settings():
    tex = unreal.load_asset("/Game/Textures/Sky/sky_sunset")
    if tex:
        # the view-direction lookup wraps at the back: no mips, so there is no seam
        tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        # the 16-bit PNG imports as linear; its colors are sRGB like the other textures
        tex.set_editor_property("srgb", True)
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
        eal.save_loaded_asset(tex)


# ------------------------------------------------------------------------------------ map
def make_map():
    path = "/Game/Maps/Oval"
    if eal.does_asset_exist(path):
        log("map " + path + " already exists")
        return
    world = asset_tools.create_asset("Oval", "/Game/Maps", unreal.World, unreal.WorldFactory())
    eal.save_loaded_asset(world)
    log("map " + path)


def main():
    import_textures(os.path.join(RES, "Textures", "Track"), "/Game/Textures/Track")
    import_textures(os.path.join(RES, "Textures", "Car"), "/Game/Textures/Car")
    import_textures(os.path.join(RES, "Textures", "Props"), "/Game/Textures/Props")
    import_textures(os.path.join(RES, "Textures", "UI"), "/Game/Textures/UI")
    import_textures(os.path.join(RES, "Textures", "Sky"), "/Game/Textures/Sky")
    sky_settings()
    import_generated(os.path.join(ART, "Generated"), "/Game/Textures/Generated")
    import_liveries(os.path.join(ART, "Liveries"), "/Game/Textures/Liveries")
    pipeline = gltf_pipeline()
    import_models(os.path.join(RES, "Models", "Car"), "/Game/Models/Car", pipeline)
    import_models(os.path.join(RES, "Models", "Props"), "/Game/Models/Props", pipeline)
    make_surface_material()
    make_surface_material("/Game/Materials/M_SurfaceTwoSided", True)
    make_fence_material()
    make_sky_material()
    make_smoke_material()
    make_map()
    log("done")


if __name__ == "__main__":
    main()
