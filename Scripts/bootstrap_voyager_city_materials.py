"""Build the original procedural interior and citizen finishes for Voyager.

Run with UnrealEditor-Cmd -ExecutePythonScript=<this file>.  The two named
materials are the only assets this script changes. No imported artwork, online
service, or LivingCity module is required. Re-running is deterministic.
"""

import json
from pathlib import Path
import traceback


ROOT = Path(__file__).resolve().parents[1]
REPORT_PATH = ROOT / "Saved" / "VoyagerCityMaterialsReport.json"
DIRECTORY = "/Game/Materials"
MATERIAL_NAMES = ("M_VoyagerInterior", "M_VoyagerCitizen")


def build_materials():
    import unreal

    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    editing = unreal.MaterialEditingLibrary
    results = []

    def node(material, cls, **properties):
        value = editing.create_material_expression(material, cls, 0, 0)
        if value is None:
            raise RuntimeError("Cannot create " + str(cls))
        for key, setting in properties.items():
            value.set_editor_property(key, setting)
        return value

    def link(source, destination, input_name=""):
        if not editing.connect_material_expressions(source, "", destination, input_name):
            raise RuntimeError("Cannot connect " + source.get_name() + " to " +
                               destination.get_name() + "." + input_name)

    def output(source, prop):
        if not editing.connect_material_property(source, "", prop):
            raise RuntimeError("Cannot connect " + str(prop))

    def unary(material, cls, source, input_name="", **properties):
        value = node(material, cls, **properties)
        link(source, value, input_name)
        return value

    def binary(material, cls, first, second=None, **properties):
        value = node(material, cls, **properties)
        link(first, value, "A")
        if second is not None:
            link(second, value, "B")
        return value

    def constant(material, value):
        return node(material, unreal.MaterialExpressionConstant, r=value)

    def scalar(material, name, value):
        return node(material, unreal.MaterialExpressionScalarParameter,
                    parameter_name=name, default_value=value)

    def tint(material, rgb):
        return node(material, unreal.MaterialExpressionVectorParameter,
                    parameter_name="Tint", default_value=unreal.LinearColor(*rgb, 1))

    def scale(material, source, value):
        return binary(material, unreal.MaterialExpressionMultiply, source, const_b=value)

    def add(material, source, value):
        return binary(material, unreal.MaterialExpressionAdd, source, const_b=value)

    def component(material, source, channel):
        return unary(material, unreal.MaterialExpressionComponentMask, source,
                     r=channel == "r", g=channel == "g", b=channel == "b", a=False)

    def blend(material, first, second, alpha):
        value = node(material, unreal.MaterialExpressionLinearInterpolate)
        link(first, value, "A")
        link(second, value, "B")
        link(alpha, value, "Alpha")
        return value

    def ramp(material, source, low, high):
        return unary(material, unreal.MaterialExpressionSmoothStep, source, "Value",
                     const_min=low, const_max=high)

    def invert(material, source):
        return unary(material, unreal.MaterialExpressionOneMinus, source)

    def saturate(material, source):
        return unary(material, unreal.MaterialExpressionClamp, source,
                     min_default=0., max_default=1.)

    def local_coordinates(material):
        # Instance space also works for each static mesh component of a moving
        # citizen. The transform keeps the noise stable far from the origin.
        world = node(material, unreal.MaterialExpressionWorldPosition)
        local = unary(material, unreal.MaterialExpressionTransformPosition, world,
                      transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
                      transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_INSTANCE)
        return binary(material, unreal.MaterialExpressionMultiply, local,
                      scalar(material, "DetailScale", 1.))

    def noise(material, coordinates, frequency):
        return unary(material, unreal.MaterialExpressionNoise, coordinates,
                     noise_function=unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_TEX3D,
                     scale=frequency, levels=1, quality=1, level_scale=2.,
                     output_min=0., output_max=1., turbulence=False)

    def close_detail(material, metres):
        depth = node(material, unreal.MaterialExpressionPixelDepth)
        return invert(material, ramp(material, depth, metres * 50., metres * 100.))

    perturb_asset = assets.load_asset("/Engine/Functions/Engine_MaterialFunctions03/Procedurals/PerturbNormalLQ")
    if perturb_asset is None:
        raise RuntimeError("Missing installed Unreal PerturbNormalLQ function")

    def bump(material, height, strength, fade):
        height = binary(material, unreal.MaterialExpressionMultiply, height,
                        scalar(material, "BumpStrength", strength))
        height = binary(material, unreal.MaterialExpressionMultiply, height, fade)
        perturb = node(material, unreal.MaterialExpressionMaterialFunctionCall,
                       material_function=perturb_asset)
        link(height, perturb, "Bump")
        material.set_editor_property("tangent_space_normal", False)
        output(perturb, unreal.MaterialProperty.MP_NORMAL)

    def material(name):
        path = DIRECTORY + "/" + name
        value = assets.load_asset(path) if assets.does_asset_exist(path) else None
        if value is None:
            value = asset_tools.create_asset(name, DIRECTORY, unreal.Material,
                                             unreal.MaterialFactoryNew())
        if value is None or not isinstance(value, unreal.Material):
            raise RuntimeError("Expected a material at " + path)
        editing.delete_all_material_expressions(value)
        value.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        value.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
        value.set_editor_property("two_sided", False)
        editing.set_base_material_usage(value, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
        return value

    def save(value, parameters, description):
        editing.layout_material_expressions(value)
        compile_result = editing.recompile_material(value)
        if compile_result:
            errors = [compile_result] if isinstance(compile_result, str) else [str(e) for e in compile_result]
            raise RuntimeError(value.get_name() + " shader errors: " + "; ".join(errors))
        if not assets.save_loaded_asset(value, False):
            raise RuntimeError("Could not save " + value.get_path_name())
        if not assets.does_asset_exist(value.get_path_name()):
            raise RuntimeError("Missing saved asset " + value.get_path_name())
        expression_count = editing.get_num_material_expressions(value)
        if expression_count < 8:
            raise RuntimeError("Unexpectedly empty material graph")
        results.append({"path": value.get_path_name(), "parameters": parameters,
                        "expressions": expression_count, "description": description,
                        "compiler_errors": [],
                        "compiler_validation": "returned errors checked" if compile_result is not None
                        else "recompile API does not expose errors"})
        unreal.log("VOYAGER CITY MATERIALS: SAVED " + value.get_name())

    interior = material("M_VoyagerInterior")
    local = local_coordinates(interior)
    grain = noise(interior, local, 1.7)
    fade = close_detail(interior, 25.)
    finish = scalar(interior, "SurfaceType", 0.)
    wood = ramp(interior, finish, .49, .51)
    metal = ramp(interior, finish, 1.49, 1.51)
    fabric = ramp(interior, finish, 2.49, 2.51)
    tile = ramp(interior, finish, 3.49, 3.51)

    # Low contrast plaster pores, long irregular wood grain, brushed metal,
    # woven cloth and narrow ceramic grout. None emit light. SurfaceType is a
    # small scalar so the same parent can serve batched furniture instances.
    plaster_value = add(interior, scale(interior, grain, .075), .95)
    grain_phase = binary(interior, unreal.MaterialExpressionAdd,
                         scale(interior, component(interior, local, "r"), .55),
                         scale(interior, grain, .11))
    wood_rings = unary(interior, unreal.MaterialExpressionSine, grain_phase, period=1.)
    wood_value = add(interior, scale(interior, wood_rings, .075), .91)
    brushed_value = add(interior, scale(interior, wood_rings, .025), .975)
    cloth_x = unary(interior, unreal.MaterialExpressionSine,
                    scale(interior, component(interior, local, "r"), 1.8), period=1.)
    cloth_y = unary(interior, unreal.MaterialExpressionSine,
                    scale(interior, component(interior, local, "b"), 1.8), period=1.)
    weave = binary(interior, unreal.MaterialExpressionMultiply, cloth_x, cloth_y)
    cloth_value = add(interior, scale(interior, weave, .025), .965)

    # Cube UVs stay well-conditioned on horizontal floors and wall panels.
    # Grout is an artistic module grid; geometry sets the actual slab size.
    uv = node(interior, unreal.MaterialExpressionTextureCoordinate, u_tiling=6., v_tiling=6.)
    tile_x = unary(interior, unreal.MaterialExpressionFrac, component(interior, uv, "r"))
    tile_y = unary(interior, unreal.MaterialExpressionFrac, component(interior, uv, "g"))
    seams = saturate(interior, binary(interior, unreal.MaterialExpressionAdd,
                                     invert(interior, ramp(interior, tile_x, .01, .025)),
                                     invert(interior, ramp(interior, tile_y, .01, .025))))
    tile_value = add(interior, scale(interior, seams, -.22), .98)
    variation = blend(interior, plaster_value, wood_value, wood)
    variation = blend(interior, variation, brushed_value, metal)
    variation = blend(interior, variation, cloth_value, fabric)
    variation = blend(interior, variation, tile_value, tile)
    variation = blend(interior, constant(interior, .97), variation, fade)
    output(binary(interior, unreal.MaterialExpressionMultiply, tint(interior, (.58, .56, .52)), variation),
           unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = blend(interior, constant(interior, .92), constant(interior, .63), wood)
    roughness = blend(interior, roughness, constant(interior, .36), metal)
    roughness = blend(interior, roughness, constant(interior, .94), fabric)
    roughness = blend(interior, roughness, constant(interior, .31), tile)
    roughness = add(interior, binary(interior, unreal.MaterialExpressionAdd, roughness,
                                     scale(interior, grain, .035)), -.0175)
    output(roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    metal_only = binary(interior, unreal.MaterialExpressionMultiply, metal, invert(interior, fabric))
    output(scale(interior, metal_only, .72), unreal.MaterialProperty.MP_METALLIC)
    output(constant(interior, .35), unreal.MaterialProperty.MP_SPECULAR)
    bump(interior, variation, .10, fade)
    save(interior, {"Tint": [.58, .56, .52],
                    "SurfaceType": {"default": 0., "values": {"plaster": 0, "wood": 1, "metal": 2, "fabric": 3, "tile": 4}},
                    "DetailScale": 1., "BumpStrength": .10},
         "Restrained original procedural plaster, wood, brushed metal, fabric and ceramic finishes")

    citizen = material("M_VoyagerCitizen")
    local = local_coordinates(citizen)
    grain = noise(citizen, local, 2.4)
    skin = saturate(citizen, scalar(citizen, "IsSkin", 0.))
    fade = close_detail(citizen, 14.)
    cloth_grain = add(citizen, scale(citizen, grain, .065), .945)
    skin_grain = add(citizen, scale(citizen, grain, .025), .975)
    variation = blend(citizen, cloth_grain, skin_grain, skin)
    variation = blend(citizen, constant(citizen, .98), variation, fade)
    output(binary(citizen, unreal.MaterialExpressionMultiply, tint(citizen, (.18, .26, .29)), variation),
           unreal.MaterialProperty.MP_BASE_COLOR)
    cloth_roughness = scalar(citizen, "Roughness", .82)
    roughness = blend(citizen, cloth_roughness, constant(citizen, .54), skin)
    roughness = add(citizen, binary(citizen, unreal.MaterialExpressionAdd, roughness,
                                   scale(citizen, grain, .04)), -.02)
    output(saturate(citizen, roughness), unreal.MaterialProperty.MP_ROUGHNESS)
    output(constant(citizen, 0.), unreal.MaterialProperty.MP_METALLIC)
    output(blend(citizen, constant(citizen, .25), constant(citizen, .32), skin),
           unreal.MaterialProperty.MP_SPECULAR)
    bump(citizen, variation, .035, fade)
    save(citizen, {"Tint": [.18, .26, .29], "IsSkin": 0., "Roughness": .82,
                   "DetailScale": 1., "BumpStrength": .035},
         "Matte clothing and restrained skin roughness with stable local detail")

    missing = [DIRECTORY + "/" + name for name in MATERIAL_NAMES
               if not assets.does_asset_exist(DIRECTORY + "/" + name)]
    if missing:
        raise RuntimeError("Missing generated materials: " + ", ".join(missing))
    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps({"status": "success", "materials": results,
                                      "provenance": "Original shader-node graphs; installed Epic PerturbNormalLQ normal function; no imported raster artwork",
                                      "asset_scope": list(MATERIAL_NAMES)}, indent=2), encoding="utf-8")
    unreal.log("VOYAGER CITY MATERIALS: SUCCESS / " + str(REPORT_PATH))


def main():
    try:
        build_materials()
    except Exception:
        detail = traceback.format_exc()
        REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
        REPORT_PATH.write_text(json.dumps({"status": "failed", "error": detail}, indent=2), encoding="utf-8")
        import unreal
        unreal.log_error("VOYAGER CITY MATERIALS FAILED:\n" + detail)
        raise


if __name__ == "__main__":
    main()
