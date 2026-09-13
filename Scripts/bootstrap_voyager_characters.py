"""CC0 MakeHuman characters for Riftbound, prepared as self-contained glTF.

Standalone: python Scripts/bootstrap_voyager_characters.py --prepare
Unreal: -ExecutePythonScript=<this script>
Only visual assets are used from MakeHuman; no AGPL application code is copied.
"""
from pathlib import Path
import concurrent.futures
import hashlib
import io
import json
import math
import struct
import sys
import traceback
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'Art'/'Characters'
SOURCE=ART/'Sources'
GENERATED=ART/'Generated'
REPORT=ROOT/'Saved'/'VoyagerCharactersReport.json'
PREFIX='/Game/Characters'
PACK='https://files.makehumancommunity.org/asset_packs/makehuman_system_assets/makehuman_system_assets_cc0.zip'
HEADERS={'User-Agent':'RiftboundCharacterPipeline/1.0 (github.com/jayalabaez/riftbound-living-city)'}
SELECTED=('clothes/male_worksuit01/','clothes/male_casualsuit03/','clothes/female_casualsuit01/',
          'clothes/shoes01/','hair/short01/','hair/bob01/',
          'skins/young_caucasian_male/','skins/young_african_male/','skins/young_african_female/')

def request(url,headers=None):
    with urllib.request.urlopen(urllib.request.Request(url,headers=HEADERS|dict(headers or {})),timeout=75) as response:
        return response.read()

class RemoteZip(io.RawIOBase):
    """Read selected official asset-pack members through HTTP Range requests."""
    def __init__(self,url):
        self.url=url;self.pos=0;self.cache={}
        with urllib.request.urlopen(urllib.request.Request(url,headers=HEADERS,method='HEAD'),timeout=30) as response:
            self.length=int(response.headers['Content-Length'])
    def seekable(self):return True
    def tell(self):return self.pos
    def seek(self,n,w=0):
        self.pos=n if w==0 else self.pos+n if w==1 else self.length+n
        return self.pos
    def read(self,n=-1):
        if n<0:n=self.length-self.pos
        if not n:return b''
        key=(self.pos,n)
        if key not in self.cache:
            req=urllib.request.Request(self.url,headers=HEADERS|{'Range':f'bytes={self.pos}-{self.pos+n-1}'})
            with urllib.request.urlopen(req,timeout=75) as response:
                if response.status!=206:raise RuntimeError('Asset server ignored HTTP Range; refusing full-pack download')
                self.cache[key]=response.read()
        result=self.cache[key];self.pos+=len(result);return result

def fetch_sources():
    SOURCE.mkdir(parents=True,exist_ok=True);GENERATED.mkdir(parents=True,exist_ok=True)
    tree=json.loads(request('https://api.github.com/repos/makehumancommunity/makehuman/git/trees/master?recursive=1'))
    revision=tree['sha']
    core=['LICENSE.md','LICENSE.ASSETS.md','makehuman/data/3dobjs/base.obj','makehuman/data/rigs/default.mhskel',
          'makehuman/data/rigs/default_weights.mhw','makehuman/data/eyes/high-poly/high-poly.obj',
          'makehuman/data/eyes/high-poly/high-poly.mhclo','makehuman/data/eyes/materials/brown_eye.png']
    core += ['makehuman/data/targets/macrodetails/'+v+'.target' for v in
             ('caucasian-male-young','african-male-young','african-female-young')]
    manifest={'license':'CC0-1.0','official_license':'https://static.makehumancommunity.org/about/license.html',
              'pack_license':'https://static.makehumancommunity.org/assets/assetpacks/makehuman_system_assets.html',
              'repository_revision':revision,'pack_url':PACK,'files':[]}
    def save_core(path):
        url='https://raw.githubusercontent.com/makehumancommunity/makehuman/'+revision+'/'+path
        destination=SOURCE/Path(path).name
        if not destination.exists():destination.write_bytes(request(url))
        data=destination.read_bytes()
        return {'file':str(destination.relative_to(ROOT)).replace('\\','/'),'source':url,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        manifest['files'].extend(pool.map(save_core,core))
    archive=zipfile.ZipFile(RemoteZip(PACK))
    selected=[info for info in archive.infolist() if not info.is_dir() and info.filename.startswith(SELECTED) and not info.filename.endswith('.thumb')]
    # Range reads share a seek cursor, so extract sequentially; each output is
    # an explicit CC0 core asset listed on the official pack's license table.
    for info in selected:
        destination=SOURCE/'system'/info.filename
        destination.parent.mkdir(parents=True,exist_ok=True)
        if not destination.exists():destination.write_bytes(archive.read(info))
        data=destination.read_bytes()
        manifest['files'].append({'file':str(destination.relative_to(ROOT)).replace('\\','/'),'source':PACK+'#'+info.filename,
                                  'bytes':len(data),'zip_crc32':format(info.CRC,'08x'),'sha256':hashlib.sha256(data).hexdigest()})
        print('CC0 asset',info.filename,flush=True)
    (ART/'sources.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    return manifest

VARIANTS={
    'Male':{'morph':'caucasian-male-young','skin':'young_caucasian_male/young_lightskinned_male_diffuse.png','outfit':'male_casualsuit03','hair':'short01'},
    'Female':{'morph':'african-female-young','skin':'young_african_female/young_darkskinned_female_diffuse.png','outfit':'female_casualsuit01','hair':'bob01'},
    'Guard':{'morph':'african-male-young','skin':'young_african_male/young_darkskinned_male_diffuse.png','outfit':'male_worksuit01','hair':'short01'},
}

def read_obj(path,body_only=False):
    import numpy as np
    vertices=[];uvs=[];faces=[];group=''
    for line in path.read_text(encoding='utf-8',errors='replace').splitlines():
        values=line.split()
        if not values:continue
        if values[0]=='v':vertices.append([float(x) for x in values[1:4]])
        elif values[0]=='vt':uvs.append([float(x) for x in values[1:3]])
        elif values[0]=='g':group=' '.join(values[1:])
        elif values[0]=='f' and (not body_only or group=='body'):
            corners=[]
            for item in values[1:]:
                indices=item.split('/');corners.append((int(indices[0])-1,int(indices[1])-1 if len(indices)>1 and indices[1] else 0))
            for j in range(1,len(corners)-1):faces.append([corners[0],corners[j],corners[j+1]])
    return np.asarray(vertices,dtype=float),np.asarray(uvs,dtype=float),faces

def read_proxy(path,body,body_weights):
    import numpy as np
    mapping=[];deleted=set();scales=np.ones(3);mode=''
    for line in path.read_text(encoding='utf-8',errors='replace').splitlines():
        words=line.split()
        if not words or words[0].startswith('#'):continue
        if words[0] in ('x_scale','y_scale','z_scale'):
            axis='xyz'.index(words[0][0]);a,b=int(words[1]),int(words[2])
            scales[axis]=abs(body[a,axis]-body[b,axis])/float(words[3]);continue
        if words[0]=='verts':mode='verts';continue
        if words[0]=='delete_verts':mode='delete';continue
        if not words[0].lstrip('-').isdigit():continue
        if mode=='verts':
            if len(words)==1:mapping.append(([int(words[0])],[1.],np.zeros(3)))
            elif len(words)>=9:mapping.append(([int(v) for v in words[:3]],[float(v) for v in words[3:6]],np.asarray(words[6:9],dtype=float)))
        elif mode=='delete':
            j=0
            while j<len(words):
                if j+2<len(words) and words[j+1]=='-':deleted.update(range(int(words[j]),int(words[j+2])+1));j+=3
                else:deleted.add(int(words[j]));j+=1
    points=[];weights=[]
    for ids,w,offset in mapping:
        points.append(np.sum(body[ids]*np.asarray(w)[:,None],axis=0)+offset*scales)
        weights.append(np.maximum(0,np.sum(body_weights[ids]*np.asarray(w)[:,None],axis=0)))
    return np.asarray(points),np.asarray(weights),deleted

def prepare_characters():
    import numpy as np
    from scipy.spatial.transform import Rotation
    original,body_uv,body_faces=read_obj(SOURCE/'base.obj',True)
    rig=json.loads((SOURCE/'default.mhskel').read_text())
    weights_file=json.loads((SOURCE/'default_weights.mhw').read_text())['weights']
    # Keep the full authored shoulder/limb chain, plus hands. Facial micro-bones
    # and toe bones merge into their nearest retained parent for crowd efficiency.
    retained={n for n in rig['bones'] if n.startswith(('root','spine','neck','head','jaw','eye.',
              'clavicle','shoulder','upperarm','lowerarm','wrist','pelvis','upperleg','lowerleg','foot','finger','metacarpal'))}
    def nearest(name):
        while name not in retained:name=rig['bones'][name]['parent']
        return name
    def parent(name):
        n=rig['bones'][name]['parent']
        return nearest(n) if n else None
    bone_names=[]
    def append_bone(name):
        if parent(name) and parent(name) not in bone_names:append_bone(parent(name))
        if name not in bone_names:bone_names.append(name)
    for name in sorted(retained):append_bone(name)
    bone_id={n:i for i,n in enumerate(bone_names)}
    raw_weights=np.zeros((len(original),len(bone_names)))
    for name,entries in weights_file.items():
        target=bone_id[nearest(name)]
        for index,weight in entries:raw_weights[index,target]+=weight
    records=[]
    for variant,settings in VARIANTS.items():
        body=original.copy()
        for line in (SOURCE/(settings['morph']+'.target')).read_text().splitlines():
            words=line.split()
            if words and not words[0].startswith('#'):body[int(words[0])]+=np.asarray(words[1:4],dtype=float)
        pieces=[];delete=set()
        for kind,path in [('Cloth',SOURCE/'system'/'clothes'/settings['outfit']/settings['outfit']),
                          ('Boots',SOURCE/'system'/'clothes'/'shoes01'/'shoes01'),
                          ('Hair',SOURCE/'system'/'hair'/settings['hair']/settings['hair']),
                          ('Eyes',SOURCE/'high-poly')]:
            p,uv,faces=read_obj(path.with_suffix('.obj'))
            mapped,w,hidden=read_proxy(path.with_suffix('.mhclo'),body,raw_weights)
            if len(mapped)!=len(p):raise RuntimeError('Proxy vertex correspondence mismatch '+str(path))
            delete.update(hidden);pieces.append((kind,mapped,uv,faces,w))
        visible=[face for face in body_faces if not all(c[0] in delete for c in face)]
        pieces.insert(0,('Skin',body,body_uv,visible,raw_weights))
        # All models are 178 cm tall, face +Z in glTF, grow along +Y, feet at0.
        body_indices=sorted({c[0] for face in body_faces for c in face})
        bottom=min(p[:,1].min() for _,p,_,_,_ in pieces)
        top=body[body_indices,1].max();scale=1.78/(top-bottom)
        def position(p):return (p-np.asarray([0,bottom,0]))*scale
        heads={n:position(body[rig['joints'][rig['bones'][n]['head']]].mean(0)) for n in bone_names}
        tails={n:position(body[rig['joints'][rig['bones'][n]['tail']]].mean(0)) for n in bone_names}
        binary=bytearray();gltf={'asset':{'version':'2.0','generator':'Riftbound CC0 Character Pipeline'},'scene':0,
             'scenes':[{'nodes':[0,1]}],'nodes':[{'name':'SK_Citizen'+variant,'mesh':0,'skin':0},{'name':'Armature','children':[]}],
             'meshes':[{'name':'SK_Citizen'+variant,'primitives':[]}],'materials':[],
             'bufferViews':[],'accessors':[],'buffers':[],'skins':[],'animations':[]}
        def accessor(data,ctype,shape,kind=None,bounds=False):
            array=np.asarray(data,dtype={5126:'<f4',5123:'<u2',5125:'<u4'}[ctype])
            while len(binary)%4:binary.append(0)
            offset=len(binary);payload=array.tobytes();binary.extend(payload)
            view={'buffer':0,'byteOffset':offset,'byteLength':len(payload)}
            if kind:view['target']=kind
            vi=len(gltf['bufferViews']);gltf['bufferViews'].append(view)
            entry={'bufferView':vi,'componentType':ctype,'count':len(array),'type':shape}
            if bounds:entry.update({'min':np.atleast_1d(np.min(array,axis=0)).tolist(),'max':np.atleast_1d(np.max(array,axis=0)).tolist()})
            ai=len(gltf['accessors']);gltf['accessors'].append(entry);return ai
        for name in bone_names:
            index=bone_id[name]+2;pr=parent(name)
            translation=heads[name]-heads[pr] if pr else heads[name]
            gltf['nodes'].append({'name':name.replace('.','_'),'translation':translation.tolist(),'children':[]})
            gltf['nodes'][bone_id[pr]+2 if pr else 1]['children'].append(index)
        inverses=[]
        for name in bone_names:
            matrix=np.eye(4);matrix[:3,3]=-heads[name];inverses.append(matrix.T.reshape(16))
        gltf['skins'].append({'name':'S_Citizen'+variant,'joints':[i+2 for i in range(len(bone_names))],
                              'skeleton':2,'inverseBindMatrices':accessor(inverses,5126,'MAT4')})
        triangle_count=0;contact_parts=[]
        for material_index,(kind,points,uv,faces,skin_weights) in enumerate(pieces):
            pts=position(points);normals=np.zeros_like(pts)
            for face in faces:
                ids=[c[0] for c in face];n=np.cross(pts[ids[1]]-pts[ids[0]],pts[ids[2]]-pts[ids[0]])
                for i in ids:normals[i]+=n
            normal_length=np.linalg.norm(normals,axis=1);normals/=np.maximum(normal_length[:,None],1.e-12)
            out_p=[];out_n=[];out_uv=[];out_j=[];out_w=[];indices=[];lookup={}
            for face in faces:
                for key in face:
                    if key not in lookup:
                        i,u=key;lookup[key]=len(out_p);out_p.append(pts[i]);out_n.append(normals[i]);out_uv.append([uv[u,0],1-uv[u,1]])
                        order=np.argsort(skin_weights[i])[-4:][::-1];weight=skin_weights[i,order].copy()
                        if weight.sum()<1.e-6:order=np.asarray([bone_id['head'] if kind in ('Eyes','Hair') else bone_id['root']]*4);weight=np.asarray([1.,0,0,0])
                        weight/=weight.sum();out_j.append(order);out_w.append(weight)
                    indices.append(lookup[key])
            gltf['materials'].append({'name':'M_Character'+kind,'pbrMetallicRoughness':{'baseColorFactor':[.6,.6,.6,1],'metallicFactor':0,'roughnessFactor':.65},'doubleSided':kind=='Hair'})
            contact_parts.append((np.asarray(out_p),np.asarray(out_j),np.asarray(out_w)))
            gltf['meshes'][0]['primitives'].append({'attributes':{'POSITION':accessor(out_p,5126,'VEC3',34962,True),
                'NORMAL':accessor(out_n,5126,'VEC3',34962),'TEXCOORD_0':accessor(out_uv,5126,'VEC2',34962),
                'JOINTS_0':accessor(out_j,5123,'VEC4',34962),'WEIGHTS_0':accessor(out_w,5126,'VEC4',34962)},
                'indices':accessor(indices,5125,'SCALAR',34963),'material':material_index})
            triangle_count+=len(indices)//3
        identity=Rotation.identity()
        def align(a,b):
            a=a/np.linalg.norm(a);b=b/np.linalg.norm(b);axis=np.cross(a,b)
            return Rotation.from_rotvec(axis/max(np.linalg.norm(axis),1.e-9)*math.acos(float(np.clip(a@b,-1,1))))
        rest={n:identity for n in bone_names};global_rest={}
        for n in bone_names:
            pr=parent(n);previous=global_rest[pr] if pr else identity
            if n.startswith('upperarm01'):
                desired=np.asarray([.12 if n.endswith('.L') else -.12,-1,.03])
                rest[n]=align(tails[n]-heads[n],previous.inv().apply(desired))
            elif n.startswith('lowerarm01'):
                desired=np.asarray([.025 if n.endswith('.L') else -.025,-1,.11])
                rest[n]=align(tails[n]-heads[n],previous.inv().apply(desired))
            global_rest[n]=previous*rest[n]
        animated=['root','spine01','head','upperarm01.L','upperarm01.R','lowerarm01.L','lowerarm01.R',
                  'upperleg01.L','upperleg01.R','lowerleg01.L','lowerleg01.R','foot.L','foot.R']
        durations={'Idle':3.,'Walk':1.,'Run':.72,'Talk':3.,'Death':1.35}
        for clip,duration in durations.items():
            times=np.linspace(0,duration,round(duration*30)+1);channels=[];samplers=[]
            rotation_keys={}
            time_accessor=accessor(times,5126,'SCALAR',bounds=True)
            def track(bone,path,values,shape):
                index=len(samplers);samplers.append({'input':time_accessor,'output':accessor(values,5126,shape),'interpolation':'LINEAR'})
                channels.append({'sampler':index,'target':{'node':bone_id[bone]+2,'path':path}})
            for n in animated:
                values=[]
                for t in times:
                    phase=t/duration*math.tau;wave=math.sin(phase+(math.pi if n.endswith('.R') else 0));q=rest[n]
                    if clip in ('Walk','Run'):
                        running=clip=='Run'
                        if n.startswith('upperleg01'):q=Rotation.from_euler('x',wave*(34 if running else 23),degrees=True)*q
                        elif n.startswith('lowerleg01'):q=Rotation.from_euler('x',-max(0,-wave)*(68 if running else 42),degrees=True)*q
                        elif n.startswith('foot'):q=Rotation.from_euler('x',max(0,-wave)*16,degrees=True)*q
                        elif n.startswith('upperarm01'):q=Rotation.from_euler('x',-wave*(25 if running else 15),degrees=True)*q
                        elif n.startswith('lowerarm01'):q=Rotation.from_euler('x',30 if running else 4,degrees=True)*q
                        elif n=='spine01':q=Rotation.from_euler('yz',[math.sin(phase)*2.2,math.sin(phase)*1.0],degrees=True)*q
                    elif clip=='Death':
                        a=min(1,t/duration);ease=a*a*(3-2*a)
                        if n=='root':q=Rotation.from_euler('xz',[-87*ease,8*math.sin(a*math.pi)],degrees=True)
                        elif n.startswith('upperleg01'):q=Rotation.from_euler('x',18*ease,degrees=True)*q
                        elif n.startswith('lowerleg01'):q=Rotation.from_euler('x',-32*ease,degrees=True)*q
                        elif n.startswith('upperarm01'):q=Rotation.from_euler('z',(12 if n.endswith('.L') else -12)*ease,degrees=True)*q
                    else:
                        if n=='spine01':q=Rotation.from_euler('x',math.sin(phase)*.7,degrees=True)*q
                        elif n=='head':q=Rotation.from_euler('y',math.sin(phase)*(7 if clip=='Talk' else 2),degrees=True)*q
                        elif clip=='Talk' and n=='upperarm01.R':q=Rotation.from_euler('x',-20+math.sin(phase)*6,degrees=True)*q
                        elif clip=='Talk' and n=='lowerarm01.R':q=Rotation.from_euler('x',45,degrees=True)*q
                    values.append(q.as_quat())
                track(n,'rotation',values,'VEC4')
                rotation_keys[n]=values
            translations=[]
            for t in times:
                value=heads['root'].copy()
                if clip=='Death':
                    a=min(1,t/duration);ease=a*a*(3-2*a);value[1]+=(.24-heads['root'][1])*ease;value[2]-=.2*ease
                elif clip in ('Walk','Run'):value[1]+=abs(math.sin(t/duration*math.tau))*(.024 if clip=='Walk' else .045)
                else:value[1]+=math.sin(t/duration*math.tau)*.003
                translations.append(value)
            if clip=='Death':
                # Keep the actual skinned body, clothes and shoes on the floor
                # throughout the collapse, including the low resting shoulders.
                # Root translation alone corrects contact without stretching limbs.
                for frame,value in enumerate(translations):
                    matrices={}
                    for n in bone_names:
                        pr=parent(n);m=np.eye(4)
                        if n in rotation_keys:m[:3,:3]=Rotation.from_quat(rotation_keys[n][frame]).as_matrix()
                        m[:3,3]=value if n=='root' else heads[n]-heads[pr] if pr else heads[n]
                        matrices[n]=matrices[pr]@m if pr else m
                    skin_matrices=np.asarray([matrices[n] for n in bone_names])
                    skin_matrices[:,:3,3]-=np.einsum('nij,nj->ni',skin_matrices[:,:3,:3],np.asarray([heads[n] for n in bone_names]))
                    minimum=float('inf')
                    for points,joints,weights in contact_parts:
                        rows=skin_matrices[joints,1,:]
                        heights=np.einsum('nki,ni->nk',rows[:,:,:3],points)+rows[:,:,3]
                        minimum=min(minimum,float((heights*weights).sum(1).min()))
                    value[1]+=.02-minimum
            track('root','translation',translations,'VEC3')
            gltf['animations'].append({'name':'A_Citizen'+variant+'_'+clip,'channels':channels,'samplers':samplers})
        filename='Citizen'+variant
        gltf['buffers']=[{'uri':filename+'.bin','byteLength':len(binary)}]
        (GENERATED/(filename+'.bin')).write_bytes(binary)
        (GENERATED/(filename+'.gltf')).write_text(json.dumps(gltf,separators=(',',':')),encoding='utf-8')
        records.append({'variant':variant,'source':str((GENERATED/(filename+'.gltf')).relative_to(ROOT)).replace('\\','/'),
                        'triangles':triangle_count,'bones':len(bone_names),'materials':[p[0] for p in pieces],
                        'animations':durations,'settings':settings})
        print('PREPARED',variant,triangle_count,'triangles',len(bone_names),'bones',flush=True)
    (ART/'characters.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    return records

def import_characters():
    import unreal
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    edit=unreal.MaterialEditingLibrary
    mesh_edit=unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
    asset_tools=unreal.AssetToolsHelpers.get_asset_tools()
    command_line=unreal.SystemLibrary.get_command_line().lower()
    audit_only='-riftcharacteraudit' in command_line
    materials_only=audit_only or '-riftcharactermaterialsonly' in command_line
    clips_only='-riftcharacterclipsonly' in command_line
    if clips_only:
        stale=PREFIX+'/Meshes/SK_CitizenMale'
        if assets.does_asset_exist(stale):
            registry=unreal.AssetRegistryHelpers.get_asset_registry()
            registry.search_all_assets(True)
            referencers=[str(r) for r in registry.get_referencers(stale,unreal.AssetRegistryDependencyOptions())]
            unreal.log('VOYAGER CHARACTER STALE REFERENCERS '+json.dumps(referencers))
            if referencers:raise RuntimeError('Stale character output still has asset referencers '+str(referencers))
            if not assets.delete_asset(stale):raise RuntimeError('Cannot delete unreferenced owned stale mesh '+stale)
            # UE5.8 can report deletion while retaining an uncontrolled file.
            stale_disk=(ROOT/'Content'/'Characters'/'Meshes'/'SK_CitizenMale.uasset').resolve()
            if stale_disk.is_file():
                if not stale_disk.is_relative_to((ROOT/'Content'/'Characters').resolve()):raise RuntimeError('Stale cleanup escaped owned directory')
                stale_disk.unlink()
    records=json.loads((ART/'characters.json').read_text())
    results=[];imported_textures={}
    def save(obj):
        if not assets.save_loaded_asset(obj,False):raise RuntimeError('Cannot save '+obj.get_path_name())
    def texture(path,normal=False):
        key=(str(path),normal)
        if key in imported_textures:return imported_textures[key]
        name='T_Character_'+path.stem
        destination=PREFIX+'/Textures/'+name
        task=unreal.AssetImportTask()
        for k,v in {'filename':str(path),'destination_path':PREFIX+'/Textures','destination_name':name,
                    'automated':True,'replace_existing':True,'save':False,'factory':unreal.TextureFactory()}.items():task.set_editor_property(k,v)
        asset_tools.import_asset_tasks([task]);obj=assets.load_asset(destination)
        if not isinstance(obj,unreal.Texture2D):raise RuntimeError('Texture import failed '+str(path))
        # A mostly blue outfit can trigger the factory's normal-map heuristic.
        # Change compression first: normal compression otherwise forces sRGB off.
        obj.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT)
        obj.set_editor_property('srgb',not normal)
        if normal:obj.set_editor_property('flip_green_channel',True)
        obj.set_editor_property('max_texture_size',2048)
        obj.set_editor_property('never_stream',False)
        save(obj)
        if bool(obj.get_editor_property('srgb'))!=bool(not normal):raise RuntimeError('Unexpected texture color space '+destination)
        imported_textures[key]=obj;return obj
    def create_material(variant,kind,settings):
        name='M_Citizen'+variant+'_'+kind;path=PREFIX+'/Materials/'+name
        mat=assets.load_asset(path) if assets.does_asset_exist(path) else asset_tools.create_asset(name,PREFIX+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
        edit.delete_all_material_expressions(mat)
        mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_MASKED if kind=='Hair' else unreal.BlendMode.BLEND_OPAQUE)
        mat.set_editor_property('two_sided',kind=='Hair')
        mat.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_SUBSURFACE if kind=='Skin' else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        mat.set_editor_property('dithered_lod_transition',True)
        if kind=='Hair':mat.set_editor_property('opacity_mask_clip_value',.27)
        edit.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_SKELETAL_MESH,True)
        def node(cls,**props):
            n=edit.create_material_expression(mat,cls,0,0)
            for k,v in props.items():n.set_editor_property(k,v)
            return n
        def link(a,b,pin='',out=''):
            if not edit.connect_material_expressions(a,out,b,pin):raise RuntimeError('Material link failed '+name)
        def output(a,p,out=''):
            if not edit.connect_material_property(a,out,p):raise RuntimeError('Material output failed '+name)
        if kind=='Skin':source=SOURCE/'system'/'skins'/settings['skin'];normal=None
        elif kind=='Cloth':source=SOURCE/'system'/'clothes'/settings['outfit']/(settings['outfit']+'_diffuse.png');normal=source.with_name(settings['outfit']+'_normal.png')
        elif kind=='Boots':source=SOURCE/'system'/'clothes'/'shoes01'/'shoes01_diffuse.png';normal=source.with_name('shoes01_normal.png')
        elif kind=='Hair':source=SOURCE/'system'/'hair'/settings['hair']/(settings['hair']+'_diffuse.png');normal=None
        else:source=SOURCE/'brown_eye.png';normal=None
        sampled=node(unreal.MaterialExpressionTextureSample,texture=texture(source),sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        tint=node(unreal.MaterialExpressionVectorParameter,parameter_name='Tint',default_value=unreal.LinearColor(1,1,1,1))
        color=node(unreal.MaterialExpressionMultiply);link(sampled,color,'A');link(tint,color,'B');output(color,unreal.MaterialProperty.MP_BASE_COLOR)
        if normal and normal.exists():output(node(unreal.MaterialExpressionTextureSample,texture=texture(normal,True),sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),unreal.MaterialProperty.MP_NORMAL)
        rough={'Skin':.53,'Cloth':.78,'Boots':.58,'Hair':.69,'Eyes':.16}[kind]
        output(node(unreal.MaterialExpressionScalarParameter,parameter_name='Roughness',default_value=rough),unreal.MaterialProperty.MP_ROUGHNESS)
        output(node(unreal.MaterialExpressionConstant,r=.35 if kind=='Skin' else .22 if kind=='Cloth' else .45),unreal.MaterialProperty.MP_SPECULAR)
        if kind=='Hair':output(sampled,unreal.MaterialProperty.MP_OPACITY_MASK,'A')
        if kind=='Skin':
            scatter=node(unreal.MaterialExpressionMultiply,const_b=.35);link(color,scatter,'A');output(scatter,unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
            output(node(unreal.MaterialExpressionConstant,r=.85),unreal.MaterialProperty.MP_OPACITY)
        edit.layout_material_expressions(mat)
        errors=edit.recompile_material(mat)
        if errors:raise RuntimeError(name+' shader errors '+str(errors))
        save(mat);return mat
    for record in records:
        variant=record['variant'];unreal.log('VOYAGER CHARACTER IMPORT '+variant)
        if materials_only:
            materials={kind:assets.load_asset(PREFIX+'/Materials/M_Citizen'+variant+'_'+kind) for kind in record['materials']}
            folder=PREFIX+'/Import/'+variant+'/Citizen'+variant+'/SkeletalMeshes/'
            objects=[assets.load_asset(folder+'Citizen'+variant)]
            objects.extend(assets.load_asset(folder+'Citizen'+variant+'A_Citizen'+variant+'_'+clip) for clip in record['animations'])
        else:
            materials={kind:assets.load_asset(PREFIX+'/Materials/M_Citizen'+variant+'_'+kind) if clips_only else create_material(variant,kind,record['settings']) for kind in record['materials']}
            task=unreal.AssetImportTask()
            for k,v in {'filename':str(ROOT/record['source']),'destination_path':PREFIX+'/Import/'+variant,
                        'automated':True,'replace_existing':True,'save':False}.items():task.set_editor_property(k,v)
            asset_tools.import_asset_tasks([task])
            objects=task.get_objects()
        if any(v is None for v in materials.values()) or any(o is None for o in objects):raise RuntimeError('Missing saved character input '+variant)
        unreal.log('VOYAGER CHARACTER OBJECTS '+str([(o.get_class().get_name(),o.get_path_name()) for o in objects]))
        meshes=[o for o in objects if isinstance(o,unreal.SkeletalMesh)]
        if len(meshes)!=1:raise RuntimeError('Expected one skeletal mesh for '+variant+', got '+str(len(meshes)))
        # Keep Interchange's deterministic source paths. Moving freshly imported
        # objects creates redirectors that collide with the next automated import.
        mesh=meshes[0]
        # Native skeletal reduction preserves skin weights, clothing and UV seams.
        if not materials_only and not mesh_edit.regenerate_lod(mesh,3):raise RuntimeError('Skeletal LOD generation failed '+variant)
        # An Unreal Array yields copies of UStruct entries while iterating.
        # Convert to a real Python list before changing material interfaces.
        slots=list(mesh.get_editor_property('materials'))
        slot_names=[]
        for slot in slots:
            slot_name=str(slot.get_editor_property('material_slot_name'))
            kind=next((k for k in record['materials'] if k.lower() in slot_name.lower()),None)
            if kind is None:raise RuntimeError('Unknown character material slot '+slot_name)
            if not audit_only:slot.set_editor_property('material_interface',materials[kind])
            slot_names.append(kind)
        if not audit_only:mesh.set_editor_property('materials',slots)
        if len(slots)!=5:raise RuntimeError('Expected5 character material slots')
        # UE5.8 keeps LODInfo private; native RegenerateLOD creates the standard
        # progressive reduction and screen-size settings through source models.
        if not audit_only:save(mesh)
        assigned=[s.get_editor_property('material_interface').get_path_name() for s in mesh.get_editor_property('materials')]
        expected=[materials[k].get_path_name() for k in slot_names]
        if assigned!=expected:raise RuntimeError('Assigned character materials did not persist '+str(assigned))
        texture_paths=set()
        for mat in materials.values():
            if not (ROOT/'Content'/Path(mat.get_path_name().split('.')[0].removeprefix('/Game/')+'.uasset')).is_file():raise RuntimeError('Material package missing on disk '+mat.get_path_name())
            for texture in edit.get_used_textures(mat):
                texture_paths.add(texture.get_path_name())
                if not (ROOT/'Content'/Path(texture.get_path_name().split('.')[0].removeprefix('/Game/')+'.uasset')).is_file():raise RuntimeError('Texture package missing on disk '+texture.get_path_name())
        clips={}
        for obj in objects:
            if not isinstance(obj,unreal.AnimSequence):continue
            clip=next((c for c in record['animations'] if obj.get_name().endswith('_'+c)),None)
            if not clip:raise RuntimeError('Unknown animation '+obj.get_name())
            if not audit_only:save(obj)
            clips[clip]=obj.get_path_name()
        if len(clips)!=5:raise RuntimeError('Expected5 animation clips for '+variant+', got '+str(clips))
        for obj in objects:
            if not audit_only and isinstance(obj,(unreal.Skeleton,unreal.PhysicsAsset)):save(obj)
        bounds=mesh.get_bounds()
        entry={'variant':variant,'mesh':mesh.get_path_name(),'animations':clips,'slots':slot_names,'material_paths':assigned,'texture_paths':sorted(texture_paths),
               'height_cm':bounds.box_extent.z*2,'lod_vertices':[mesh_edit.get_num_verts(mesh,i) for i in range(3)],
               'source_triangles':record['triangles'],'source_bones':record['bones']}
        if not 160<entry['height_cm']<195:raise RuntimeError('Character axis/scale mismatch '+str(entry))
        results.append(entry);unreal.log('VOYAGER CHARACTER READY '+json.dumps(entry))
    REPORT.write_text(json.dumps({'status':'success','characters':results,'source_manifest':'Art/Characters/sources.json'},indent=2))
    unreal.log('VOYAGER CHARACTERS SUCCESS:3 clothed humanoids,15 clips,3 LODs per mesh')

def main():
    if '--fetch' in sys.argv or '--prepare' in sys.argv:
        fetch_sources()
        if '--prepare' in sys.argv:prepare_characters()
        return
    if '--geometry' in sys.argv:prepare_characters();return
    import unreal
    try:import_characters()
    except Exception:
        REPORT.write_text(json.dumps({'status':'failed','error':traceback.format_exc()},indent=2))
        unreal.log_error('VOYAGER CHARACTERS FAILED\n'+traceback.format_exc());raise

if __name__=='__main__':main()
