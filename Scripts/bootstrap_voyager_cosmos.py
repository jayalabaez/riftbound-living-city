"""Original procedural cosmos materials. Run in Unreal's Python commandlet.

Only /Game/Cosmos is modified; there are no paid assets or external downloads.
Native volumetric weather and original seeded orbital clouds share physical planet coordinates.
"""
import json
import traceback
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'Saved' / 'VoyagerCosmosMaterialsReport.json'
NOISE = r'''
struct FCosmosNoise {
 float Hash(int3 P) {
  uint3 Q=asuint(P);uint H=Q.x*1597334677u^Q.y*3812015801u^Q.z*2798796415u;
  H^=H>>16;H*=2246822519u;H^=H>>13;H*=3266489917u;H^=H>>16;
  return float(H&0x00ffffffu)/16777215.0;
 }
 float Value(float3 P) {
  int3 I=int3(floor(P));float3 F=frac(P);F=F*F*(3-2*F);
  float A=lerp(Hash(I),Hash(I+int3(1,0,0)),F.x);
  float B=lerp(Hash(I+int3(0,1,0)),Hash(I+int3(1,1,0)),F.x);
  float C=lerp(Hash(I+int3(0,0,1)),Hash(I+int3(1,0,1)),F.x);
  float D=lerp(Hash(I+int3(0,1,1)),Hash(I+int3(1,1,1)),F.x);
  return saturate(lerp(lerp(A,B,F.y),lerp(C,D,F.y),F.z));
 }
};FCosmosNoise N;
'''


def main():
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    edit = unreal.MaterialEditingLibrary
    factory = unreal.AssetToolsHelpers.get_asset_tools()
    results = []

    def material(name, blend=unreal.BlendMode.BLEND_OPAQUE, lit=False, volume=False):
        path = '/Game/Cosmos/' + name
        value = assets.load_asset(path) if assets.does_asset_exist(path) else factory.create_asset(name, '/Game/Cosmos', unreal.Material, unreal.MaterialFactoryNew())
        edit.delete_all_material_expressions(value)
        value.set_editor_property('blend_mode', blend)
        value.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT if lit else unreal.MaterialShadingModel.MSM_UNLIT)
        value.set_editor_property('material_domain', unreal.MaterialDomain.MD_VOLUME if volume else unreal.MaterialDomain.MD_SURFACE)
        value.set_editor_property('two_sided', not volume)
        return value

    def node(mat, cls, **props):
        value = edit.create_material_expression(mat, cls)
        for key, val in props.items():
            value.set_editor_property(key, val)
        return value

    def link(a, b, pin, out=''):
        if not edit.connect_material_expressions(a, out, b, pin):
            raise RuntimeError('Material connection failed: ' + b.get_name() + '.' + pin)

    def output(value, prop):
        if not edit.connect_material_property(value, '', prop):
            raise RuntimeError('Material output failed: ' + str(prop))

    def scalar(mat, name, val):
        return node(mat, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=val)

    def vector(mat, name, val):
        return node(mat, unreal.MaterialExpressionVectorParameter, parameter_name=name, default_value=unreal.LinearColor(*val, 0))

    def custom(mat, code, inputs, dims=1):
        args = []
        for name in inputs:
            arg = unreal.CustomInput()
            arg.set_editor_property('input_name', name)
            args.append(arg)
        value = node(mat, unreal.MaterialExpressionCustom, code=code, inputs=args, output_type=getattr(unreal.CustomMaterialOutputType, 'CMOT_FLOAT' + str(dims)))
        for name, source in inputs.items():
            link(source, value, name)
        return value

    def mul(mat, a, b):
        value = node(mat, unreal.MaterialExpressionMultiply)
        link(a, value, 'A'); link(b, value, 'B')
        return value

    def save(mat):
        edit.layout_material_expressions(mat)
        errors = edit.recompile_material(mat)
        if errors:
            raise RuntimeError(mat.get_name() + ': ' + str(errors))
        if not assets.save_loaded_asset(mat, False):
            raise RuntimeError('Save failed: ' + mat.get_path_name())
        results.append({'asset': mat.get_path_name(), 'expressions': edit.get_num_material_expressions(mat), 'compiler_errors': []})

    mat = material('M_CosmosHorizon')
    output(node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0, 0, 0, 1)), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    save(mat)

    mat = material('M_CosmosDisk', unreal.BlendMode.BLEND_ADDITIVE)
    uv = node(mat, unreal.MaterialExpressionTextureCoordinate)
    time = node(mat, unreal.MaterialExpressionTime)
    inputs = {'UV': uv, 'T': time, 'Seed': scalar(mat, 'Seed', 1), 'RingMode': scalar(mat, 'RingMode', 0)}
    structure = custom(mat, NOISE + r'''
float R=saturate(UV.y);float A=UV.x*6.28318530718;
float speed=.035/pow(1+R*3,1.5);float theta=A-T*speed;
float3 P=float3(cos(theta)*(2+R*7),sin(theta)*(2+R*7),Seed*.1);
float grain=.6*N.Value(P*5)+.28*N.Value(P*17)+.12*N.Value(P*39);
float ribbons=.85+.15*sin(R*110+grain*5+T*.8);
float edge=smoothstep(0,.08,R)*(1-smoothstep(.68,1,R));
float density=edge*(.35+.65*grain)*ribbons*lerp(1.5,.16,R);
return lerp(density,sin(R*3.14159265359)*(.85+.15*grain),RingMode);
''', inputs)
    colour = custom(mat, 'return lerp(float3(.92,.96,1),OuterTint.rgb,pow(saturate(UV.y),.45));', {'UV': uv, 'OuterTint': vector(mat, 'OuterTint', (1, .25, .04))}, 3)
    output(mul(mat, mul(mat, colour, structure), scalar(mat, 'Intensity', 1.8)), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    output(node(mat, unreal.MaterialExpressionConstant, r=1.0), unreal.MaterialProperty.MP_OPACITY)
    save(mat)

    mat = material('M_CosmosLimb', unreal.BlendMode.BLEND_ADDITIVE)
    normal = node(mat, unreal.MaterialExpressionVertexNormalWS)
    view = node(mat, unreal.MaterialExpressionCameraVectorWS)
    factor = custom(mat, '''float grazing=pow(saturate(1-abs(dot(normalize(N),normalize(V)))),5);
float day=smoothstep(-.18,.3,dot(normalize(N),normalize(SunDirection.rgb)));
return grazing*day*.55;''', {'N': normal, 'V': view, 'SunDirection': vector(mat, 'SunDirection', (-.57, .62, .53))})
    output(mul(mat, vector(mat, 'Tint', (.17, .39, .86)), factor), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    output(node(mat, unreal.MaterialExpressionConstant, r=1.0), unreal.MaterialProperty.MP_OPACITY)
    save(mat)

    # Use the installed engine's spherical cloud shader, whose Substrate volume
    # lighting is supported by UE 5.8. The project instance owns weather settings.
    weather_path = '/Game/Cosmos/MI_CosmosWeather'
    engine_weather = unreal.load_asset('/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst')
    weather_instance = assets.load_asset(weather_path) if assets.does_asset_exist(weather_path) else factory.create_asset('MI_CosmosWeather', '/Game/Cosmos', unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    edit.set_material_instance_parent(weather_instance, engine_weather)
    edit.set_material_instance_scalar_parameter_value(weather_instance, 'Cloud_GlobalCoverage', .02)
    edit.set_material_instance_scalar_parameter_value(weather_instance, 'Cloud_GlobalDensity', .008)
    edit.set_material_instance_vector_parameter_value(weather_instance, 'Cloud_AlbedoColor', unreal.LinearColor(.98, .985, 1, .5))
    assets.save_loaded_asset(weather_instance, False)
    results.append({'asset': weather_instance.get_path_name(), 'parent': engine_weather.get_path_name(), 'compiler_errors': []})

    mat = material('M_CosmosCloudDistant', unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('two_sided', False)
    world = node(mat, unreal.MaterialExpressionWorldPosition)
    center = vector(mat, 'PlanetCenter', (0, 0, -60000000))
    coords = custom(mat, 'return (W-C.rgb)*.000001+Seed.rgb+float3(T*.0007,T*.0002,0);',
                    {'W': world, 'C': center, 'Seed': vector(mat, 'SeedOffset', (17.3, 28.1, 4.8)), 'T': node(mat, unreal.MaterialExpressionTime)}, 3)
    weather = custom(mat, NOISE + '''float broad=.58*N.Value(P*.22)+.29*N.Value(P*.65+17)+.13*N.Value(P*1.9-9);
float coverage=smoothstep(CoverageStart,CoverageStart+.14,broad);
float puffs=.54*N.Value(P*2.7)+.29*N.Value(P*7.1)+.17*N.Value(P*17.3);
return saturate(coverage-(1-puffs)*.18);''', {'P': coords, 'CoverageStart': scalar(mat, 'CoverageStart', .39)})
    normal = node(mat, unreal.MaterialExpressionVertexNormalWS)
    lighting = custom(mat, 'return float3(.92,.94,.98)*(.035+.82*smoothstep(-.08,.7,dot(normalize(N),normalize(float3(-.5784,.6203,.5299)))));', {'N': normal}, 3)
    output(lighting, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity = custom(mat, 'return saturate(D*2.2);', {'D': weather})
    output(opacity, unreal.MaterialProperty.MP_OPACITY)
    save(mat)

    mat = material('M_CosmosOcean', lit=True)
    mat.set_editor_property('tangent_space_normal', False)
    world = node(mat, unreal.MaterialExpressionWorldPosition)
    normal = node(mat, unreal.MaterialExpressionVertexNormalWS)
    output(vector(mat, 'Tint', (.006, .036, .06)), unreal.MaterialProperty.MP_BASE_COLOR)
    output(node(mat, unreal.MaterialExpressionConstant, r=.17), unreal.MaterialProperty.MP_ROUGHNESS)
    output(node(mat, unreal.MaterialExpressionConstant, r=.65), unreal.MaterialProperty.MP_SPECULAR)
    waves = custom(mat, '''float3 up=normalize(N);float3 side=normalize(cross(up,abs(up.z)<.9?float3(0,0,1):float3(1,0,0)));
float3 forward=cross(up,side);float a=sin(dot(P,side)*.008+T*.6);float b=sin(dot(P,forward)*.005-T*.4);
return normalize(up+side*a*.025+forward*b*.018);''', {'N': normal, 'P': world, 'T': node(mat, unreal.MaterialExpressionTime)}, 3)
    output(waves, unreal.MaterialProperty.MP_NORMAL)
    save(mat)

    mat = material('M_CosmosDust', unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('two_sided', False)
    edit.set_base_material_usage(mat, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    density = custom(mat, NOISE + '''float3 P=(W-C)*.004;
float turbulence=.62*N.Value(P)+.27*N.Value(P*2.7)+.11*N.Value(P*7.1);
float silhouette=pow(saturate(abs(dot(normalize(Normal),normalize(View)))),1.7);
return silhouette*(.3+.7*turbulence)*DustOpacity;''',
        {'W': node(mat, unreal.MaterialExpressionWorldPosition), 'C': node(mat, unreal.MaterialExpressionObjectPositionWS),
         'Normal': node(mat, unreal.MaterialExpressionVertexNormalWS), 'View': node(mat, unreal.MaterialExpressionCameraVectorWS),
         'DustOpacity': scalar(mat, 'DustOpacity', .30)})
    depth_fade = node(mat, unreal.MaterialExpressionDepthFade, fade_distance_default=120.0)
    link(density, depth_fade, 'Opacity')
    output(depth_fade, unreal.MaterialProperty.MP_OPACITY)
    output(vector(mat, 'DustTint', (.23, .21, .18)), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    save(mat)
    REPORT.write_text(json.dumps({'status': 'success', 'materials': results, 'provenance': 'Original materials plus a project instance of the installed Unreal Engine cloud shader; no marketplace downloads', 'weather': 'Native spherical volume weather, biome coverage and original distant orbital cloud patterns'}, indent=2))
    unreal.log('VOYAGER COSMOS MATERIALS SUCCESS')


try:
    main()
except Exception:
    REPORT.write_text(json.dumps({'status': 'failed', 'error': traceback.format_exc()}, indent=2))
    unreal.log_error(traceback.format_exc())
    raise
