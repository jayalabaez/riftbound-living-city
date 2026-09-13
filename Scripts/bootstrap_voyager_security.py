"""Original Vesper hover sedan and Sentinel drone; centimetres, +X forward.
Reuses this project's mesh authoring utilities and existing Kestrel PBR materials.
Run --prepare with Python, then execute this file in Unreal to import three LODs.
No downloaded vehicle meshes or paid assets.
"""
from pathlib import Path
import sys, math, json, traceback
sys.path.insert(0,str(Path(__file__).resolve().parent))
import bootstrap_voyager_ships as geo
ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'Art/Security'; GEO=ART/'Meshes'
REPORT=ROOT/'Saved/VoyagerSecurityAssetsReport.json'
SLOTS=['Paint','Trim','Metal','Glass','Light','Red','Blue','Rubber']
def box(m,p,s,b,slot):m.chamfer_box(p,s,b,slot)
def ring(m,p,r,t,lod,slot='Metal'):
    n=(40,24,12)[lod]
    points=[(p[0]+r*math.cos(i*math.tau/n),p[1]+r*math.sin(i*math.tau/n),p[2]) for i in range(n+1)]
    m.tube(points,t,(10,8,6)[lod],slot)
def cruiser(lod):
    m=geo.Mesh(SLOTS)
    # Shoulder, sill and bonnet sections: a low automotive silhouette with bevelled edges.
    sections=[(-258,77,30,55),(-244,94,18,68),(-190,103,15,79),(-105,105,15,85),(50,104,15,80),(155,99,18,72),(232,89,24,55),(258,68,30,43)]
    rows=[]
    for x,w,low,top in sections:
        rows.append([(x,-w+8,low),(x,w-8,low),(x,w,low+12),(x,w,top-12),(x,w-12,top),(x,-w+12,top),(x,-w,top-12),(x,-w,low+12)])
    for a,b in zip(rows,rows[1:]):
        for j in range(8):m.quad([a[j],a[(j+1)%8],b[(j+1)%8],b[j]],'Paint')
    m.quad(rows[0],'Paint',(-1,0,0));m.quad(rows[-1],'Paint',(1,0,0))
    box(m,(-20,0,16),(450,183,17),6,'Trim')
    # Cabin: raked front/rear glass, two side doors per flank, narrow pillars.
    m.quad([(108,-83,79),(108,83,79),(28,76,141),(28,-76,141)],'Glass')
    m.quad([(-192,85,80),(-192,-85,80),(-133,-76,139),(-133,76,139)],'Glass')
    box(m,(-52,0,142),(171,155,12),5,'Paint')
    for side in [-1,1]:
        y=side*96
        m.quad([(92,y,81),(-15,y,84),(-15,side*78,136),(25,side*77,136)],'Glass')
        m.quad([(-22,y,84),(-177,side*91,82),(-131,side*77,134),(-22,side*78,136)],'Glass')
        m.tube([(108,side*84,78),(28,side*77,140)],3,8,'Metal')
        m.tube([(-192,side*86,79),(-133,side*77,140)],3,8,'Metal')
        m.tube([(-19,side*97,81),(-19,side*78,140)],3,8,'Trim')
        m.tube([(-186,side*99,78),(98,side*100,78)],2,8,'Metal')
        box(m,(-20,side*104,43),(317,4,4),1,'Trim')
        # Door shut lines and inset handles follow the actual side surface.
        for x in [-170,-18,105]:m.tube([(x,side*105,29),(x,side*105,72)],.8,6,'Rubber')
        for x in [-128,54]:box(m,(x,side*106,68),(22,3,3),1,'Metal')
        box(m,(74,side*114,91),(27,19,13),4,'Paint')
        box(m,(66,side*124,92),(16,2,8),1,'Glass')
        box(m,(241,side*67,54),(12,36,6),2,'Light')
        box(m,(-248,side*64,60),(8,47,7),2,'Red')
        box(m,(-56,side*45,155),(45,68,7),3,'Red' if side<0 else 'Blue')
        # Four integrated lift nacelles replace wheels, with recessed duct throats.
        for x in [-174,158]:
            box(m,(x,side*99,18),(88,50,36),10,'Trim')
            ring(m,(x,side*107,0),29,5,lod)
            ring(m,(x,side*107,-5),23,2,lod,'Blue')
            box(m,(x,side*107,1),(22,22,8),3,'Metal')
            for k in range((8,6,4)[lod]):
                a=k*math.tau/((8,6,4)[lod]);m.tube([(x,side*107,0),(x+24*math.cos(a),side*107+24*math.sin(a),0)],1.4,6,'Metal')
    box(m,(255,0,32),(11,128,14),4,'Trim');box(m,(-255,0,32),(10,147,13),3,'Trim')
    box(m,(260,0,43),(3,61,12),1,'Metal')
    if lod<2:
        for y in range(-27,28,6):box(m,(262,y,43),(2,2,9),.5,'Trim')
        for side in [-1,1]:
            m.tube([(105,side*4,81),(75,side*46,106)],1,6,'Rubber')
            for x in range(-224,-199,5):box(m,(x,side*47,79),(2,47,2),.4,'Trim')
    return m
def drone(lod):
    m=geo.Mesh(SLOTS)
    m.loft([(-63,0,0,10,9),(-44,0,0,33,17),(5,0,0,38,21),(47,0,0,27,15),(63,0,0,8,7)],(32,20,12)[lod],'Paint')
    box(m,(30,0,-18),(52,36,17),5,'Trim')
    box(m,(59,0,-18),(3,18,12),2,'Glass')
    box(m,(62,0,-18),(2,6,6),1,'Red')
    m.tube([(12,0,-30),(78,0,-30)],5,12,'Metal')
    ring(m,(0,0,17),14,2,lod,'Blue')
    for x in [-45,40]:
        for y in [-65,65]:
            m.tube([(x*.4,y*.35,0),(x,y,4)],6,8,'Trim')
            ring(m,(x,y,4),33,5,lod)
            ring(m,(x,y,0),29,2,lod,'Blue')
            box(m,(x,y,4),(12,12,13),3,'Rubber')
            for k in range((6,4,3)[lod]):
                a=k*math.tau/((6,4,3)[lod]);m.tube([(x,y,4),(x+27*math.cos(a),y+27*math.sin(a),4)],2,6,'Metal')
    box(m,(-23,0,21),(30,20,3),1,'Light')
    return m
def prepare():
    GEO.mkdir(parents=True,exist_ok=True);geo.GEO=GEO
    (GEO/'ship_slots.mtl').write_text(''.join('newmtl '+s+'\nKd .5 .5 .5\n' for s in SLOTS))
    records={}
    for name,fn in [('SM_VesperCruiser',cruiser),('SM_SentinelDrone',drone)]:
        lods=[];slots=None
        for i in range(3):
            m=fn(i);m.slots=[s for s in SLOTS if any(f[3]==s for f in m.faces)]
            if slots is not None:assert slots==m.slots
            slots=m.slots;lods.append(m.write(name,i))
        records[name]={'slots':slots,'lods':lods}
    (ART/'meshes.json').write_text(json.dumps(records,indent=2))
    print({n:[l['triangles'] for l in d['lods']] for n,d in records.items()})
def build():
    import unreal as u
    a=u.get_editor_subsystem(u.EditorAssetSubsystem);t=u.AssetToolsHelpers.get_asset_tools();e=u.MaterialEditingLibrary;mt=u.get_editor_subsystem(u.StaticMeshEditorSubsystem)
    palettes={'Paint':('HullPaint',(.095,.15,.20),.38,.26),'Trim':('DarkPanels',(.025,.034,.045),.65,.36),'Metal':('BrushedMetal',(.30,.35,.39),.8,.23),
              'Glass':('HullPaint',(.008,.020,.030),.65,.08),'Rubber':('Interior',(.012,.014,.017),0,.82),'Light':('Thruster',(.8,.91,1),0,.2),'Red':('Beacon',(1,.008,.012),0,.2),'Blue':('Beacon',(.005,.22,1),0,.2)}
    mats={}
    for name,(base,color,metal,rough) in palettes.items():
        path='/Game/Security/MI_'+name
        mat=a.load_asset(path) if a.does_asset_exist(path) else t.create_asset('MI_'+name,'/Game/Security',u.MaterialInstanceConstant,u.MaterialInstanceConstantFactoryNew())
        e.set_material_instance_parent(mat,u.load_asset('/Game/Ships/Materials/M_Kestrel'+base))
        e.set_material_instance_vector_parameter_value(mat,'Tint',u.LinearColor(*color,1))
        for key,val in [('Metallic',metal),('Roughness',rough),('Glow',3 if name in ['Light','Red','Blue'] else 0)]:e.set_material_instance_scalar_parameter_value(mat,key,val)
        a.save_loaded_asset(mat,False);mats[name]=mat
    records=json.loads((ART/'meshes.json').read_text());result=[]
    for name,record in records.items():
        opt=u.FbxImportUI()
        for k,v in {'import_mesh':True,'import_as_skeletal':False,'import_materials':False,'import_textures':False,'automated_import_should_detect_type':False,'mesh_type_to_import':u.FBXImportType.FBXIT_STATIC_MESH}.items():opt.set_editor_property(k,v)
        d=opt.get_editor_property('static_mesh_import_data')
        for k,v in {'combine_meshes':True,'generate_lightmap_u_vs':False,'auto_generate_collision':False,'convert_scene':False,'convert_scene_unit':False,'force_front_x_axis':False,'import_uniform_scale':1.,'normal_import_method':u.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS}.items():d.set_editor_property(k,v)
        task=u.AssetImportTask()
        for k,v in {'filename':str(ROOT/record['lods'][0]['file']),'destination_path':'/Game/Security','destination_name':name,'automated':True,'replace_existing':True,'save':False,'factory':u.FbxFactory(),'options':opt}.items():task.set_editor_property(k,v)
        t.import_asset_tasks([task]);mesh=u.load_asset('/Game/Security/'+name)
        if not isinstance(mesh,u.StaticMesh):raise RuntimeError('Missing mesh '+name)
        for i,slot in enumerate(record['slots']):mesh.set_material(i,mats[slot])
        for i in [1,2]:
            if mt.import_lod(mesh,i,str(ROOT/record['lods'][i]['file']))!=i:raise RuntimeError('LOD import failed')
        mt.set_lod_screen_sizes(mesh,[1,.35,.1]);mesh.set_editor_property('allow_cpu_access',True)
        actual=[]
        for i in range(3):
            settings=mt.get_lod_build_settings(mesh,i)
            settings.set_editor_property('generate_lightmap_u_vs',False);settings.set_editor_property('recompute_normals',False);mt.set_lod_build_settings(mesh,i,settings)
            verts=[];triangles=0
            for section in range(mesh.get_num_sections(i)):
                buf=u.ProceduralMeshLibrary.get_section_from_static_mesh(mesh,i,section);verts+=list(buf[0]);triangles+=len(buf[1])//3
            bounds=[[min(getattr(v,c) for v in verts),max(getattr(v,c) for v in verts)] for c in 'xyz']
            assert triangles==record['lods'][i]['triangles']
            assert all(abs(bounds[j][k]-record['lods'][i]['bounds_cm'][j][k])<1 for j in range(3) for k in range(2)),bounds
            actual.append({'triangles':triangles,'bounds':bounds})
        mesh.set_editor_property('allow_cpu_access',False);a.save_loaded_asset(mesh,False);result.append({'mesh':name,'lods':actual})
    REPORT.write_text(json.dumps({'status':'success','meshes':result},indent=2));u.log('VOYAGER SECURITY ASSETS SUCCESS')
if __name__=='__main__':
    if '--prepare' in sys.argv:prepare()
    else:
        try:build()
        except Exception:
            REPORT.write_text(json.dumps({'status':'failed','error':traceback.format_exc()}));raise
