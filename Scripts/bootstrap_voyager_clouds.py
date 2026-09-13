"""Original spherical volumetric weather, using Unreal's ray-marched cloud renderer."""
import json
import traceback
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'Saved' / 'VoyagerCloudsReport.json'

# Integer-hashed corner values interpolated with a smooth cubic curve. Every
# corner and interpolation lies in [0,1], independent of material noise modes.
VALUE_NOISE = r'''
struct FVoyagerValueNoise
{
    float Hash(int3 P)
    {
        uint3 Q = asuint(P);
        uint H = Q.x * 1597334677u ^ Q.y * 3812015801u ^ Q.z * 2798796415u;
        H ^= H >> 16; H *= 2246822519u;
        H ^= H >> 13; H *= 3266489917u;
        H ^= H >> 16;
        return float(H & 0x00ffffffu) / 16777215.0;
    }
    float Value(float3 P)
    {
        int3 I = int3(floor(P));
        float3 F = frac(P); F = F * F * (3.0 - 2.0 * F);
        float A = lerp(Hash(I), Hash(I + int3(1,0,0)), F.x);
        float B = lerp(Hash(I + int3(0,1,0)), Hash(I + int3(1,1,0)), F.x);
        float C = lerp(Hash(I + int3(0,0,1)), Hash(I + int3(1,0,1)), F.x);
        float D = lerp(Hash(I + int3(0,1,1)), Hash(I + int3(1,1,1)), F.x);
        return saturate(lerp(lerp(A,B,F.y), lerp(C,D,F.y), F.z));
    }
};
FVoyagerValueNoise N;
'''

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
        def coherent_noise(coords, expression):
            custom_input = unreal.CustomInput()
            custom_input.set_editor_property('input_name','P')
            n = node(unreal.MaterialExpressionCustom,code=VALUE_NOISE+'return '+expression+';',
                     output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT1,inputs=[custom_input])
            link(coords,n,'P')
            return n
        world = node(unreal.MaterialExpressionWorldPosition)
        center = node(unreal.MaterialExpressionVectorParameter,parameter_name='PlanetCenter',default_value=unreal.LinearColor(0,0,-60000000,0))
        rel = node(unreal.MaterialExpressionSubtract); link(world,rel,'A');link(center,rel,'B')
        seed = node(unreal.MaterialExpressionVectorParameter,parameter_name='SeedOffset',default_value=unreal.LinearColor(17.3,28.1,4.8,0))
        # Weather has broad regional structure plus kilometre-scale erosion,
        # rather than one opaque cloud blanket stretching across the whole sky.
        km = node(unreal.MaterialExpressionMultiply,const_b=.0000003);link(rel,km,'A')
        coords = node(unreal.MaterialExpressionAdd);link(km,coords,'A');link(seed,coords,'B')
        noise = coherent_noise(coords,'saturate(.6*N.Value(P*.45)+.27*N.Value(P*1.3+17.0)+.13*N.Value(P*3.8-9.0))')
        coverage_start = node(unreal.MaterialExpressionScalarParameter,parameter_name='CoverageStart',default_value=.57)
        coverage_end = node(unreal.MaterialExpressionAdd,const_b=.14);link(coverage_start,coverage_end,'A')
        coverage = node(unreal.MaterialExpressionSmoothStep);link(noise,coverage,'Value');link(coverage_start,coverage,'Min');link(coverage_end,coverage,'Max')
        albedo = node(unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(.86,.87,.89,1))
        output(albedo,unreal.MaterialProperty.MP_BASE_COLOR)
        if volume:
            attr = node(unreal.MaterialExpressionCloudSampleAttribute)
            lower = node(unreal.MaterialExpressionSmoothStep,const_min=0.0,const_max=.16);link(attr,lower,'Value','NormAltitudeInLayer')
            upper = node(unreal.MaterialExpressionSmoothStep,const_min=.58,const_max=1.0);link(attr,upper,'Value','NormAltitudeInLayer')
            inv = node(unreal.MaterialExpressionOneMinus);link(upper,inv,'')
            layer = node(unreal.MaterialExpressionMultiply);link(lower,layer,'A');link(inv,layer,'B')
            shape = node(unreal.MaterialExpressionMultiply);link(layer,shape,'A');link(coverage,shape,'B')
            detail = coherent_noise(coords,'.25+.75*N.Value(P*7.0)')
            eroded = node(unreal.MaterialExpressionMultiply);link(shape,eroded,'A');link(detail,eroded,'B')
            # Volume materials route Extinction through Subsurface Color.
            # Opacity is ignored by the cloud marcher; its unconnected default
            # extinction would turn the entire layer into an opaque white shell.
            # VolumetricCloud.usf integrates these coefficients in meters.
            density = node(unreal.MaterialExpressionMultiply,const_b=.002);link(eroded,density,'A')
            output(density,unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
            node(unreal.MaterialExpressionVolumetricAdvancedMaterialOutput,const_phase_g=.65,const_phase_g2=-.2,const_phase_blend=.2,multi_scattering_approximation_octave_count=1,gray_scale_material=True,ray_march_volume_shadow=True)
        else:
            output(coverage,unreal.MaterialProperty.MP_OPACITY_MASK)
            rough = node(unreal.MaterialExpressionConstant,r=1.0);output(rough,unreal.MaterialProperty.MP_ROUGHNESS)
        edit.layout_material_expressions(mat)
        errors = edit.recompile_material(mat)
        if errors: raise RuntimeError(str(errors))
        if not assets.save_loaded_asset(mat,False): raise RuntimeError('Save failed '+path)
        results.append({'asset':path,'nodes':index,'volume':volume})
    REPORT.write_text(json.dumps({'status':'success','materials':results,'weather':'bounded coherent value-noise FBM',
                                 'noise_range':[0,1],'fbm_weights':[.6,.27,.13],
                                 'coverage_start':.57,'coverage_softness':.14,'density':.000004},indent=2))
    unreal.log('VOYAGER CLOUDS SUCCESS')

try:
    main()
except Exception:
    REPORT.write_text(json.dumps({'status':'failed','error':traceback.format_exc()},indent=2))
    unreal.log_error(traceback.format_exc())
    raise
