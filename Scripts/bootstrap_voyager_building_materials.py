"""Create the original clear architectural glass used by Voyager's real windows.

Only /Game/Materials/M_VoyagerGlass is changed. Run in the installed Unreal editor;
no online service, imported texture, existing material, or LivingCity asset is used.
"""
import json
from pathlib import Path
import traceback

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "Saved" / "VoyagerBuildingMaterialsReport.json"


def main():
    import unreal
    try:
        assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
        editing = unreal.MaterialEditingLibrary
        path = "/Game/Materials/M_VoyagerGlass"
        material = assets.load_asset(path)
        if material is None:
            material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                "M_VoyagerGlass", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew())
        if material is None:
            raise RuntimeError("Could not create architectural glass")
        editing.delete_all_material_expressions(material)
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        material.set_editor_property("two_sided", True)
        material.set_editor_property("translucency_lighting_mode",
                                     unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        material.set_editor_property("used_with_instanced_static_meshes", True)

        def scalar(name, value, prop):
            node = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, 0, 0)
            node.set_editor_property("parameter_name", name)
            node.set_editor_property("default_value", value)
            if not editing.connect_material_property(node, "", prop):
                raise RuntimeError("Cannot connect " + name)
            return node

        tint = editing.create_material_expression(material, unreal.MaterialExpressionVectorParameter, 0, 0)
        tint.set_editor_property("parameter_name", "Tint")
        tint.set_editor_property("default_value", unreal.LinearColor(.66, .80, .82, 1.))
        editing.connect_material_property(tint, "", unreal.MaterialProperty.MP_BASE_COLOR)
        opacity = scalar("Opacity", .075, unreal.MaterialProperty.MP_OPACITY)
        fade = editing.create_material_expression(material, unreal.MaterialExpressionPerInstanceFadeAmount, 0, 0)
        faded = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, 0, 0)
        editing.connect_material_expressions(opacity, "", faded, "A")
        editing.connect_material_expressions(fade, "", faded, "B")
        editing.connect_material_property(faded, "", unreal.MaterialProperty.MP_OPACITY)
        scalar("Roughness", .08, unreal.MaterialProperty.MP_ROUGHNESS)
        scalar("Specular", .5, unreal.MaterialProperty.MP_SPECULAR)
        scalar("Metallic", 0., unreal.MaterialProperty.MP_METALLIC)
        # No refraction input: two surfaces of each thin closed cube must not
        # distort the city, the ground, or distant planetary silhouettes.
        editing.layout_material_expressions(material)
        editing.recompile_material(material)
        if not assets.save_loaded_asset(material, only_if_is_dirty=False):
            raise RuntimeError("Could not save architectural glass")
        usage_enabled = bool(material.get_editor_property("used_with_instanced_static_meshes"))
        actual_path = material.get_path_name()
        if not usage_enabled or actual_path != path + ".M_VoyagerGlass":
            raise RuntimeError("Architectural glass usage/path verification failed: " + actual_path)
        result = {"status": "success", "asset": path, "object_path": actual_path,
                  "used_with_instanced_static_meshes": usage_enabled,
                  "parameters": {"Tint": [.66, .80, .82], "Opacity": .075,
                                 "Roughness": .08, "Specular": .5, "Metallic": 0.},
                  "provenance": "Original Unreal shader graph, no raster inputs"}
        unreal.log("VOYAGER BUILDING MATERIALS: SUCCESS")
    except Exception:
        result = {"status": "failed", "error": traceback.format_exc()}
        unreal.log_error(result["error"])
        raise
    finally:
        REPORT.parent.mkdir(parents=True, exist_ok=True)
        REPORT.write_text(json.dumps(result, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
