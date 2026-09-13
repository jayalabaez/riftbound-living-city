"""Original spherical volumetric weather, using Unreal's ray-marched cloud renderer."""
import json
import traceback
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'Saved' / 'VoyagerCloudsReport.json'

def main():
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    edit = unreal.MaterialEditingLibrary
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    results = []
    for name, volume in [('M_VoyagerCloudVolume', True), ('M_VoyagerCloudDistant', False)]:
        path = '/Game/Materials/' + name
        mat = assets.load_asset(path) if assets.does_asset_exist(path) else asset_tools.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
        edit.delete_all_material_expressions(mat)
        mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_VOLUME if volume else unreal.MaterialDomain.MD_SURFACE)
        mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE if volume else unreal.BlendMode.BLEND_MASKED)
        mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        mat.set_editor_property('two_sided', not volume)
        if volume:
            edit.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD,True)
        index = 0
        def node(cls, **props):
            nonlocal index
            index += 1
            n = edit.create_material_expression(mat, cls, (index % 8)*220, (index//8)*200)
            for key, value in props.items(): n.set_editor_property(key, value)
            return n
        def link(a,b,pin,out=''):
            if not edit.connect_material_expressions(a,out,b,pin): raise RuntimeError('Cannot connect '+a.get_name()+' to '+b.get_name()+'.'+pin)
        def output(n,p):
            if not edit.connect_material_property(n,'',p): raise RuntimeError('Cannot connect '+str(p))
        world = node(unreal.MaterialExpressionWorldPosition)
        center = node(unreal.MaterialExpressionVectorParameter,parameter_name='PlanetCenter',default_value=unreal.LinearColor(0,0,-60000000,0))
        rel = node(unreal.MaterialExpressionSubtract); link(world,rel,'A');link(center,rel,'B')
        seed = node(unreal.MaterialExpressionVectorParameter,parameter_name='SeedOffset',default_value=unreal.LinearColor(17.3,28.1,4.8,0))
        # Broad100km weather systems stay recognizable from orbit; smaller
        # erosion detail shapes their interior rather than aliasing into dots.
        km = node(unreal.MaterialExpressionMultiply,const_b=.0000001);link(rel,km,'A')
        coords = node(unreal.MaterialExpressionAdd);link(km,coords,'A');link(seed,coords,'B')
        noise = node(unreal.MaterialExpressionNoise,noise_function=unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_TEX3D,scale=1.0,levels=2,quality=1,level_scale=2.1,output_min=0.0,output_max=1.0,turbulence=False)
        link(coords,noise,'')
        coverage = node(unreal.MaterialExpressionSmoothStep,const_min=.57,const_max=.72);link(noise,coverage,'Value')
        albedo = node(unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(.78,.81,.85,1))
        output(albedo,unreal.MaterialProperty.MP_BASE_COLOR)
        if volume:
            attr = node(unreal.MaterialExpressionCloudSampleAttribute)
            lower = node(unreal.MaterialExpressionSmoothStep,const_min=0.0,const_max=.16);link(attr,lower,'Value','NormAltitudeInLayer')
            upper = node(unreal.MaterialExpressionSmoothStep,const_min=.58,const_max=1.0);link(attr,upper,'Value','NormAltitudeInLayer')
            inv = node(unreal.MaterialExpressionOneMinus);link(upper,inv,'')
            layer = node(unreal.MaterialExpressionMultiply);link(lower,layer,'A');link(inv,layer,'B')
            shape = node(unreal.MaterialExpressionMultiply);link(layer,shape,'A');link(coverage,shape,'B')
            detail = node(unreal.MaterialExpressionNoise,noise_function=unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_TEX3D,scale=7.0,levels=1,quality=1,output_min=.25,output_max=1.0,turbulence=False)
            link(coords,detail,'')
            eroded = node(unreal.MaterialExpressionMultiply);link(shape,eroded,'A');link(detail,eroded,'B')
            density = node(unreal.MaterialExpressionMultiply,const_b=.000045);link(eroded,density,'A')
            output(density,unreal.MaterialProperty.MP_OPACITY)
            node(unreal.MaterialExpressionVolumetricAdvancedMaterialOutput,const_phase_g=.65,const_phase_g2=-.2,const_phase_blend=.2,multi_scattering_approximation_octave_count=1,gray_scale_material=True,ray_march_volume_shadow=True)
        else:
            output(coverage,unreal.MaterialProperty.MP_OPACITY_MASK)
            rough = node(unreal.MaterialExpressionConstant,r=1.0);output(rough,unreal.MaterialProperty.MP_ROUGHNESS)
        edit.layout_material_expressions(mat)
        errors = edit.recompile_material(mat)
        if errors: raise RuntimeError(str(errors))
        if not assets.save_loaded_asset(mat,False): raise RuntimeError('Save failed '+path)
        results.append({'asset':path,'nodes':index,'volume':volume})
    REPORT.write_text(json.dumps({'status':'success','materials':results},indent=2))
    unreal.log('VOYAGER CLOUDS SUCCESS')

try:
    main()
except Exception:
    REPORT.write_text(json.dumps({'status':'failed','error':traceback.format_exc()},indent=2))
    unreal.log_error(traceback.format_exc())
    raise
