"""Read back generated skinned character data; no Unreal or source mutations."""
from pathlib import Path
import json
import numpy as np
from scipy.spatial.transform import Rotation

ROOT=Path(__file__).resolve().parents[1]

def inspect(path):
    data=json.loads(path.read_text());blob=(path.parent/data['buffers'][0]['uri']).read_bytes()
    def read(index):
        a=data['accessors'][index];v=data['bufferViews'][a['bufferView']]
        width={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        return np.frombuffer(blob,dtype={5126:'<f4',5123:'<u2',5125:'<u4'}[a['componentType']],count=a['count']*width,
                             offset=v.get('byteOffset',0)+a.get('byteOffset',0)).reshape(a['count'],width)
    nodes=data['nodes'];parents={c:i for i,n in enumerate(nodes) for c in n.get('children',[])}
    joints=data['skins'][0]['joints'];inverse=read(data['skins'][0]['inverseBindMatrices']).reshape(-1,4,4).transpose(0,2,1)
    parts=[]
    for part in data['meshes'][0]['primitives']:
        a=part['attributes'];parts.append((read(a['POSITION']),read(a['JOINTS_0']),read(a['WEIGHTS_0'])))
    eyes=data['meshes'][0]['primitives'][4];uv=read(eyes['attributes']['TEXCOORD_0'])
    assert not np.any((uv[:,0]>.87)&(uv[:,1]>.87)), 'Opaque corneal atlas patch retained'
    assert len(data['animations'])==6, 'Expected idle/walk/run/talk/death/aim'
    result={'variant':path.stem,'opaque_cornea_removed':True,'clips':{}}
    for clip in data['animations']:
        name=clip['name'].split('_')[-1]
        if name not in ('Walk','Run','Death'):continue
        tracks={}
        for channel in clip['channels']:
            sampler=clip['samplers'][channel['sampler']];times=read(sampler['input'])[:,0]
            tracks[(channel['target']['node'],channel['target']['path'])]=read(sampler['output'])
        clearances=[];feet=[]
        for frame in range(len(times)):
            world=[]
            for i,n in enumerate(nodes):
                matrix=np.eye(4)
                matrix[:3,3]=tracks.get((i,'translation'),[n.get('translation',[0,0,0])]*len(times))[frame]
                if (i,'rotation') in tracks:matrix[:3,:3]=Rotation.from_quat(tracks[(i,'rotation')][frame]).as_matrix()
                world.append(world[parents[i]]@matrix if i in parents else matrix)
            skin=np.asarray([world[i] for i in joints])@inverse
            low=float('inf')
            for points,indices,weights in parts:
                rows=skin[indices,1,:];heights=np.einsum('nki,ni->nk',rows[:,:,:3],points)+rows[:,:,3]
                low=min(low,float((heights*weights).sum(1).min()))
            clearances.append(low)
            feet.append([world[next(i for i,n in enumerate(nodes) if n['name']=='foot_'+side)][:3,3] for side in ('L','R')])
        assert min(clearances)>.0199 and max(clearances)<.0201, (path.stem,name,'floor contact',min(clearances),max(clearances))
        report={'frames':len(times),'minimum_clearance_cm':min(clearances)*100,'maximum_clearance_cm':max(clearances)*100}
        if name!='Death':
            stride,stance=(2.10,.39) if name=='Run' else (1.24,.58)
            worst=0.
            for side,offset in enumerate((0.,.5)):
                previous=None
                for frame,t in enumerate(times):
                    u=(float(t/times[-1])+offset)%1.
                    foot=feet[frame][side].copy();foot[2]+=stride*float(t/times[-1])
                    if u<stance and previous is not None and u>=previous[0]:
                        worst=max(worst,float(np.linalg.norm((foot-previous[1])[[0,2]])))
                    previous=(u,foot) if u<stance else None
            report['worst_stance_slip_per_frame_cm']=worst*100
            assert worst<.002,(path.stem,name,'stance slipping',worst)
        result['clips'][name]=report
    return result

if __name__=='__main__':
    results=[inspect(path) for path in sorted((ROOT/'Art/Characters/Generated').glob('Citizen*.gltf'))]
    assert len(results)==3
    report=ROOT/'Saved/VoyagerCharacterGaitReport.json';report.write_text(json.dumps({'status':'passed','characters':results},indent=2))
    print(json.dumps(results,indent=2));print('PASS: eye shell removal, all locomotion/death ground contact and stance motion; '+str(report))
