"""Riftbound nature: CC0 scanned surfaces and economical, authored vegetation.

python Scripts/bootstrap_voyager_nature.py --prepare
UnrealEditor-Cmd Riftbound.uproject -unattended -NullRHI -ExecutePythonScript=<file>

The standalone preparation step downloads only explicitly listed CC0 source maps
and two lightweight plant meshes from Poly Haven's public API, checks MD5, records
SHA256/provenance, and creates three authored OBJ LODs per combined static mesh.
No executable/plugin, Blender install, paid asset or runtime network is required.
"""
from pathlib import Path
import concurrent.futures
import datetime
import hashlib
import json
import math
import random
import struct
import sys
import traceback
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "Art" / "Nature"
TEX = ART / "Textures"
GEO = ART / "Meshes"
REF = ART / "Sources"
REPORT = ROOT / "Saved" / "VoyagerNatureReport.json"
DIRECTORY = "/Game/Nature"
HEADERS = {"User-Agent": "RiftboundNatureAssetPipeline/1.0 (github.com/jayalabaez/riftbound-living-city)"}
TAU = math.tau

MAPS = {
    "Bark": ("pine_bark", {"Color": "Diffuse", "Normal": "nor_dx", "Roughness": "Rough"}),
    "Leaves": ("tree_small_02", {"Color": "leaves_diff", "Normal": "leaves_nor_dx", "Roughness": "leaves_rough", "Alpha": "leaves_alpha"}),
    "Needles": ("pine_sapling_small", {"Color": "twig_diff", "Normal": "twig_nor_dx", "Roughness": "twig_rough"}),
    "Grass": ("grass_medium_01", {"Color": "Diffuse", "Normal": "nor_dx", "Roughness": "Rough", "Alpha": "Alpha"}),
    "Fern": ("fern_02", {"Color": "Diffuse", "Normal": "nor_dx", "Roughness": "Rough", "Alpha": "Alpha"}),
    "Ground": ("leafy_grass", {"Color": "Diffuse", "Normal": "nor_dx", "Roughness": "Rough"}),
    "Rock": ("rock_boulder_cracked", {"Color": "Diffuse", "Normal": "nor_dx", "Roughness": "Rough"}),
}


def request(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=90) as response:
        return response.read()


def download(info, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    existing = destination.read_bytes() if destination.is_file() else b""
    if not existing or hashlib.md5(existing).hexdigest() != info["md5"]:
        existing = request(info["url"])
        if hashlib.md5(existing).hexdigest() != info["md5"]:
            raise RuntimeError("Source checksum mismatch: " + info["url"])
        destination.write_bytes(existing)
    return {"file": str(destination.relative_to(ROOT)).replace("\\", "/"),
            "url": info["url"], "bytes": len(existing), "upstream_md5": info["md5"],
            "sha256": hashlib.sha256(existing).hexdigest()}


def prepare_sources():
    for directory in (TEX, GEO, REF):
        directory.mkdir(parents=True, exist_ok=True)
    sources = {}
    metadata = {}
    for slug, _ in MAPS.values():
        if slug in metadata:
            continue
        files = json.loads(request("https://api.polyhaven.com/files/" + slug))
        detail = json.loads(request("https://api.polyhaven.com/info/" + slug))
        metadata[slug] = files
        sources[slug] = {"name": detail.get("name", slug), "authors": detail.get("authors", {}),
                         "asset_page": "https://polyhaven.com/a/" + slug,
                         "license": "CC0-1.0", "license_url": "https://polyhaven.com/license",
                         "files": []}
    maps = {}
    jobs = []
    for label, (slug, channels) in MAPS.items():
        maps[label] = {}
        for channel, key in channels.items():
            fmt = "png" if channel == "Alpha" else "jpg"
            info = metadata[slug][key]["2k"][fmt]
            path = TEX / Path(info["url"]).name
            maps[label][channel] = str(path.relative_to(ROOT)).replace("\\", "/")
            jobs.append((slug, info, path))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda job: (job[0], download(job[1], job[2])), jobs))
    for slug, info in results:
        sources[slug]["files"].append(info)
    # Only vertex/UV buffers are needed; our materials use the explicit maps above.
    for slug in ("grass_medium_01", "fern_02"):
        info = metadata[slug]["gltf"]["2k"]["gltf"]
        sources[slug]["files"].append(download(info, REF / (slug + ".gltf")))
        for relative, data in info.get("include", {}).items():
            if relative.endswith(".bin"):
                sources[slug]["files"].append(download(data, REF / relative))
    manifest = {"prepared_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "asset_license": "CC0-1.0", "maps": maps, "sources": sources}
    (ART / "sources.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def mul(a, b): return tuple(x * b for x in a)
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def length(a): return math.sqrt(dot(a, a))
def norm(a): return mul(a, 1.0 / max(length(a), 1e-9))
def lerp(a, b, t): return add(mul(a, 1-t), mul(b, t))


class Mesh:
    def __init__(self, slots):
        self.slots = slots
        self.vertices = []
        self.normals = []
        self.uvs = []
        self.faces = []

    def vertex(self, p, n, uv):
        self.vertices.append(p)
        self.normals.append(norm(n))
        self.uvs.append(uv)
        return len(self.vertices)

    def triangle(self, a, b, c, slot):
        if length(cross(sub(self.vertices[b-1], self.vertices[a-1]), sub(self.vertices[c-1], self.vertices[a-1]))) > 1e-6:
            self.faces.append((a, b, c, slot))

    def tube(self, points, radius, tip, sides, slot=0):
        rings = []
        arc = 0.0
        for j, p in enumerate(points):
            direction = norm(sub(points[min(j+1, len(points)-1)], points[max(0, j-1)]))
            x = norm(cross(direction, (0, 0, 1) if abs(direction[2]) < .9 else (0, 1, 0)))
            y = cross(direction, x)
            if j:
                arc += length(sub(p, points[j-1]))
            r = radius * (1-j/(len(points)-1)) + tip * j/(len(points)-1)
            ring = []
            for k in range(sides+1):
                angle = TAU*k/sides
                radial = add(mul(x, math.cos(angle)), mul(y, math.sin(angle)))
                rib = 1 + .035*math.sin(angle*5 + j*.73)
                ring.append(self.vertex(add(p, mul(radial, r*rib)), radial, (k/sides, arc/150)))
            rings.append(ring)
        for j in range(len(rings)-1):
            for k in range(sides):
                a,b,c,d = rings[j][k],rings[j][k+1],rings[j+1][k+1],rings[j+1][k]
                self.triangle(a,b,c,slot); self.triangle(a,c,d,slot)
        # End caps protect thin silhouette tips and root cross-sections.
        for ring, p, n, reverse in ((rings[0], points[0], norm(sub(points[0], points[1])), True),
                                   (rings[-1], points[-1], norm(sub(points[-1], points[-2])), False)):
            center = self.vertex(p,n,(.5,.5))
            for k in range(sides):
                self.triangle(center, ring[k+1] if reverse else ring[k], ring[k] if reverse else ring[k+1], slot)

    def leaf_card(self, center, direction, width, height, angle, slot=1, curvature=.11,
                  uvbox=(.008,.035,.48,.982), segments=3):
        axis = norm(direction)
        side = norm(cross(axis, (0,0,1) if abs(axis[2]) < .92 else (0,1,0)))
        other = cross(axis, side)
        side = add(mul(side,math.cos(angle)),mul(other,math.sin(angle)))
        normal = norm(cross(side,axis))
        rows = []
        for j in range(segments+1):
            t = j/segments
            row=[]
            p=add(center,mul(axis,(t-.5)*height))
            p=add(p,mul(normal,math.sin(t*math.pi)*height*curvature))
            for k in (0,1):
                uv=(uvbox[0]+(uvbox[2]-uvbox[0])*k,1-(uvbox[3]+(uvbox[1]-uvbox[3])*t))
                row.append(self.vertex(add(p,mul(side,(k-.5)*width)),normal,uv))
            rows.append(row)
        for j in range(segments):
            a,b=rows[j];d,c=rows[j+1]
            self.triangle(a,b,c,slot);self.triangle(a,c,d,slot)

    def needle(self, base, direction, blade_length, width, slot=1, proxy=False):
        d=norm(direction);side=norm(cross(d,(0,0,1) if abs(d[2])<.9 else (0,1,0)))
        normal=norm(cross(side,d))
        mid=add(base,mul(d,blade_length*.45));tip=add(base,mul(d,blade_length))
        # UVs cover an actual scanned pine needle strip in the source twig atlas.
        if proxy:
            a=self.vertex(base,normal,(.85,.19))
            b=self.vertex(add(mid,mul(side,width)),normal,(.86,.32))
            c=self.vertex(tip,normal,(.85,.46))
            d_id=self.vertex(add(mid,mul(side,-width)),normal,(.84,.32))
            self.triangle(a,b,c,slot);self.triangle(a,c,d_id,slot)
            return
        a=self.vertex(add(base,mul(side,-width*.25)),normal,(.84,.19))
        b=self.vertex(add(base,mul(side,width*.25)),normal,(.86,.19))
        c=self.vertex(add(mid,mul(side,width)),normal,(.86,.32))
        d_id=self.vertex(add(mid,mul(side,-width)),normal,(.84,.32))
        e=self.vertex(add(tip,mul(normal,.7)),normal,(.85,.46))
        self.triangle(a,b,c,slot);self.triangle(a,c,d_id,slot);self.triangle(d_id,c,e,slot)

    def normalize_height(self, height):
        bottom=min(p[2] for p in self.vertices)
        top=max(p[2] for p in self.vertices)
        sz=height/(top-bottom)
        self.vertices=[(p[0],p[1],(p[2]-bottom)*sz) for p in self.vertices]
        self.normals=[norm((n[0],n[1],n[2]/sz)) for n in self.normals]

    def save(self, name, lod):
        path=GEO/(name+"_LOD"+str(lod)+".obj")
        mtl=GEO/(name+".mtl")
        mtl.write_text("\n".join("newmtl "+slot+"\nKd 0.6 0.6 0.6\n" for slot in self.slots),encoding="utf-8")
        with path.open("w",encoding="utf-8") as out:
            out.write("# Riftbound nature / centimetres / Z-up / source CC0 credit: Art/Nature/PROVENANCE.md\n")
            out.write("mtllib "+mtl.name+"\no "+name+"\ns 1\n")
            for p in self.vertices:out.write("v %.6f %.6f %.6f\n"%p)
            for uv in self.uvs:out.write("vt %.6f %.6f\n"%uv)
            for n in self.normals:out.write("vn %.7f %.7f %.7f\n"%n)
            for index,slot in enumerate(self.slots):
                out.write("usemtl "+slot+"\n")
                for a,b,c,s in self.faces:
                    if s==index:out.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
        bounds=[[min(p[i] for p in self.vertices),max(p[i] for p in self.vertices)] for i in range(3)]
        return {"file":str(path.relative_to(ROOT)).replace("\\","/"),"triangles":len(self.faces),
                "vertices":len(self.vertices),"bounds_cm":bounds,"sha256":hashlib.sha256(path.read_bytes()).hexdigest()}


def curve(start,end,bend,segments=6):
    return [add(lerp(start,end,t/segments),mul(bend,math.sin(math.pi*t/segments))) for t in range(segments+1)]


def broadleaf(lod, shrub=False):
    mesh=Mesh(["M_NatureBark","M_NatureDryLeaves" if shrub else "M_NatureLeaves"])
    sides=(12,8,6)[lod]
    trunk=[(math.sin(t*.63)*12+t*1.2,math.cos(t*.47)*10-10,t*80) for t in range(11)]
    mesh.tube(trunk,27,1.5,sides)
    for index in range(12):
        if shrub and index%2:continue
        rnd=random.Random(913+index)
        t=.28+.055*index;angle=index*2.399+rnd.uniform(-.2,.2)
        start=lerp(trunk[int(t*10)],trunk[min(10,int(t*10)+1)],t*10%1)
        spread=(1.1-t)*390+rnd.uniform(15,60)
        end=add(start,(math.cos(angle)*spread,math.sin(angle)*spread,100+rnd.uniform(10,90)))
        primary=curve(start,end,(20,-18,-32),6 if lod<2 else 4)
        mesh.tube(primary,10*(1-t)+2,.8,max(5,sides-3))
        for j in range(5):
            sr=random.Random(661+index*91+j*19)
            u=.22+j*.17;p=lerp(start,end,u)
            phi=angle+(1 if j%2 else -1)*sr.uniform(.3,.9)
            twig_end=add(p,(math.cos(phi)*sr.uniform(62,112),math.sin(phi)*sr.uniform(62,112),sr.uniform(55,120)))
            mesh.tube(curve(p,twig_end,(0,0,-12),3 if lod<2 else 2),2.5,.18,max(4,sides-6))
            # Each curved card holds a photographed compound leafy twig. The
            # same branch topology survives in every LOD; card density reduces.
            count=((4,3,2) if shrub else (8,5,3))[lod]
            for k in range(count):
                lr=random.Random(index*701+j*43+k*107)
                pos=lerp(p,twig_end,.36+lr.random()*.64)
                pos=add(pos,(lr.uniform(-25,25),lr.uniform(-25,25),lr.uniform(-8,32)))
                axis=norm((lr.uniform(-.9,.9),lr.uniform(-.9,.9),lr.uniform(.2,1)))
                mesh.leaf_card(pos,axis,lr.uniform(43,66)*(1+lod*.13),lr.uniform(90,132)*(1+lod*.1),lr.uniform(0,TAU),segments=(3,2,1)[lod])
    # Root flare is individually tapered, not a cylinder intersecting the soil.
    for i in range(7):
        a=i*TAU/7;mesh.tube([(0,0,42),(math.cos(a)*24,math.sin(a)*24,8),(math.cos(a)*62,math.sin(a)*62,0)],9,.4,max(4,sides-4))
    if shrub:
        mesh.vertices=[(p[0]*.31,p[1]*.31,p[2]*.22) for p in mesh.vertices]
    mesh.normalize_height(140 if shrub else 800)
    return mesh


def conifer(lod):
    mesh=Mesh(["M_NatureBark","M_NatureNeedles"])
    sides=(12,7,4)[lod]
    trunk=[(math.sin(i*.5)*8,math.cos(i*.35)*5-5,i*100) for i in range(13)]
    mesh.tube(trunk,29,1.2,sides)
    for level in range(11 if lod<2 else 9):
        z=180+level*86;branch_length=(1-level/12)*250+25
        arm_count=5 if lod<2 else 4
        for arm in range(arm_count):
            angle=arm*TAU/arm_count+level*1.29
            radial=(math.cos(angle),math.sin(angle),0)
            start=(math.sin(level*.6)*7,0,z)
            end=add(start,add(mul(radial,branch_length),(0,0,30+level*2)))
            mesh.tube(curve(start,end,(0,0,-28),(5,3,2)[lod]),max(1.8,7-level*.42),.35,max(3,sides-4))
            if lod==2:
                for segment in range(3):
                    center=lerp(start,end,.3+segment*.29)
                    for needle in range(8):
                        phi2=angle+needle*TAU/8
                        mesh.needle(center,(math.cos(phi2),math.sin(phi2),.6),60,8,proxy=True)
                continue
            for twig in range(5 if lod==0 else 3):
                tr=random.Random(1937+level*499+arm*83+twig)
                f=.25+twig*(.15 if lod==0 else .28);root=lerp(start,end,f)
                phi=angle+(1 if twig%2 else -1)*.61
                tdir=(math.cos(phi),math.sin(phi),.2)
                tlen=38+(1-f)*35
                tip=add(root,mul(tdir,tlen))
                mesh.tube(curve(root,tip,(0,0,9),2 if lod==0 else 1),.65,.08,4 if lod==0 else 3)
                for segment in range((5,3,3)[lod]):
                    center=lerp(root,tip,.12+segment/((5,3,3)[lod])*.85)
                    for needle in range((7,4,2)[lod]):
                        phi2=angle+needle*TAU/((7,4,2)[lod])+tr.uniform(-.2,.2)
                        direction=norm((math.cos(phi2)*.7,math.sin(phi2)*.7,tr.uniform(.25,.95)))
                        mesh.needle(center,direction,tr.uniform(11,20)*(1+lod*.5),(.6,2.6,1.2)[lod])
    mesh.normalize_height(1200)
    return mesh


def read_gltf_mesh(slug,index):
    data=json.loads((REF/(slug+".gltf")).read_text())
    buffers=[(REF/Path(v["uri"]).name).read_bytes() for v in data["buffers"]]
    def accessor(i):
        a=data["accessors"][i];view=data["bufferViews"][a["bufferView"]]
        components={"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4}[a["type"]]
        fmt={5126:"f",5125:"I",5123:"H",5121:"B"}[a["componentType"]]
        packed=struct.Struct("<"+fmt*components);stride=view.get("byteStride",packed.size)
        offset=view.get("byteOffset",0)+a.get("byteOffset",0)
        return [packed.unpack_from(buffers[view["buffer"]],offset+j*stride) for j in range(a["count"])]
    mesh=data["meshes"][data["nodes"][index]["mesh"]]
    p=mesh["primitives"][0]
    return accessor(p["attributes"]["POSITION"]),accessor(p["attributes"]["NORMAL"]),accessor(p["attributes"]["TEXCOORD_0"]),accessor(p["indices"])


def simplify_blades(mesh,max_faces):
    """Collapse longitudinal outline edges; keep every scanned blade and UV.

    Cross-blade edges are excluded so reduction cannot erase thin leaves. Each
    blade's UV endpoints and the clump's geometric extremes remain fixed.
    """
    neighbors={i:set() for i in range(1,len(mesh.vertices)+1)}
    for a,b,c,slot in mesh.faces:
        for vertex in (a,b,c):neighbors[vertex].update((a,b,c))
    seen=set();components=[]
    for first in neighbors:
        if first in seen:continue
        stack=[first];component=set()
        while stack:
            i=stack.pop()
            if i in component:continue
            component.add(i);stack.extend(neighbors[i]-component)
        seen.update(component);components.append(component)
    fixed=set()
    for axis in range(3):
        fixed.add(min(neighbors,key=lambda i:mesh.vertices[i-1][axis]))
        fixed.add(max(neighbors,key=lambda i:mesh.vertices[i-1][axis]))
    reduced=[]
    for component in components:
        faces=[face for face in mesh.faces if face[0] in component]
        if len(faces)<=max_faces:reduced.extend(faces);continue
        uv=lambda i:mesh.uvs[i-1]
        a,b=max(((i,j) for i in component for j in component),key=lambda p:sum((uv(p[0])[k]-uv(p[1])[k])**2 for k in (0,1)))
        direction=(uv(b)[0]-uv(a)[0],uv(b)[1]-uv(a)[1]);span=math.hypot(*direction)
        direction=(direction[0]/span,direction[1]/span)
        projection=lambda i:uv(i)[0]*direction[0]+uv(i)[1]*direction[1]
        protected=fixed|{min(component,key=projection),max(component,key=projection)}
        while len(faces)>max_faces:
            edges={}
            for a,b,c,_ in faces:
                for edge in ((a,b),(b,c),(c,a)):
                    key=tuple(sorted(edge));edges[key]=edges.get(key,0)+1
            candidates=[]
            for (a,b),count in edges.items():
                if count!=1:continue
                du=(uv(b)[0]-uv(a)[0],uv(b)[1]-uv(a)[1]);extent=math.hypot(*du)
                if extent<1e-9 or abs((du[0]*direction[0]+du[1]*direction[1])/extent)<.6:continue
                for remove,keep in ((a,b),(b,a)):
                    if remove in protected:continue
                    cost=sum((mesh.vertices[a-1][k]-mesh.vertices[b-1][k])**2 for k in range(3))
                    candidates.append((cost,remove,keep))
            if not candidates:break
            _,remove,keep=min(candidates)
            new=[]
            for a,b,c,slot in faces:
                ids=[keep if i==remove else i for i in (a,b,c)]
                if len(set(ids))==3:new.append((*ids,slot))
            if len(new)==len(faces):break
            faces=new
        reduced.extend(faces)
    result=Mesh(mesh.slots)
    used=sorted({i for face in reduced for i in face[:3]})
    remap={i:result.vertex(mesh.vertices[i-1],mesh.normals[i-1],mesh.uvs[i-1]) for i in used}
    for a,b,c,slot in reduced:result.triangle(remap[a],remap[b],remap[c],slot)
    return result


def scanned_plant(lod,fern=False):
    if not fern and lod>0:return simplify_blades(scanned_plant(0),10 if lod==1 else 4)
    mesh=Mesh(["M_NatureFern" if fern else "M_NatureGrass"])
    if fern:
        # Three small photographed fronds preserve organic shape from overhead.
        slug="fern_02";source_indices=([2,3,2],[2,3],[2])[lod]
    else:
        slug="grass_medium_01";source_indices=([4,12,16],[12,16],[14,15])[lod]
    for j,index in enumerate(source_indices):
        points,normals,uvs,indices=read_gltf_mesh(slug,index)
        angle=j*2.399;c=math.cos(angle);s=math.sin(angle)
        ids=[]
        for p,n,uv in zip(points,normals,uvs):
            # glTF metres/Y-up -> centimetres/Z-up. Ignore display-layout node
            # translation; each source mesh is already authored at its root.
            q=(p[0]*100,-p[2]*100,p[1]*100);nn=(n[0],-n[2],n[1])
            q=(q[0]*c-q[1]*s,q[0]*s+q[1]*c,q[2])
            nn=(nn[0]*c-nn[1]*s,nn[0]*s+nn[1]*c,nn[2])
            ids.append(mesh.vertex(q,nn,(uv[0],1-uv[1])))
        for k in range(0,len(indices),3):
            mesh.triangle(ids[indices[k][0]],ids[indices[k+1][0]],ids[indices[k+2][0]],0)
    mesh.normalize_height(70 if fern else 55)
    # Root-centered clumps, independent from the source asset's presentation grid.
    cx=(max(p[0] for p in mesh.vertices)+min(p[0] for p in mesh.vertices))*.5
    cy=(max(p[1] for p in mesh.vertices)+min(p[1] for p in mesh.vertices))*.5
    max_width=max(max(p[i] for p in mesh.vertices)-min(p[i] for p in mesh.vertices) for i in (0,1))
    scale=(110 if fern else 85)/max_width
    mesh.vertices=[((p[0]-cx)*scale,(p[1]-cy)*scale,p[2]) for p in mesh.vertices]
    return mesh


def boulder(lod):
    mesh=Mesh(["M_NatureRock"]);sides=(36,20,12)[lod];rings=(20,12,8)[lod]
    grid=[]
    for j in range(rings+1):
        phi=.001+(math.pi-.002)*j/rings;row=[]
        for i in range(sides+1):
            theta=TAU*i/sides
            n=(math.sin(phi)*math.cos(theta),math.sin(phi)*math.sin(theta),math.cos(phi))
            rugged=1+.11*math.sin(n[0]*9+n[1]*6)*math.sin(n[2]*11+1)+.045*math.sin(n[0]*24-n[2]*13)
            p=(n[0]*125*rugged,n[1]*103*rugged,max(-65,n[2]*90*rugged))
            row.append(mesh.vertex(p,n,(theta/TAU*2,phi/math.pi)))
        grid.append(row)
    for j in range(rings):
        for i in range(sides):
            a,b,c,d=grid[j][i],grid[j+1][i],grid[j+1][i+1],grid[j][i+1]
            mesh.triangle(a,b,c,0);mesh.triangle(a,c,d,0)
    mesh.normalize_height(180)
    return mesh


def prepare_geometry():
    results={}
    for name,maker in [("SM_Broadleaf",lambda l:broadleaf(l)),("SM_Conifer",conifer),
                       ("SM_DryShrub",lambda l:broadleaf(l,True)),("SM_Grass",lambda l:scanned_plant(l)),
                       ("SM_Fern",lambda l:scanned_plant(l,True)),("SM_Boulder",boulder)]:
        results[name]={"path":DIRECTORY+"/Meshes/"+name,"lods":[]}
        for lod in range(3):
            mesh=maker(lod);results[name]["slots"]=mesh.slots
            results[name]["lods"].append(mesh.save(name,lod))
        print(name, [v["triangles"] for v in results[name]["lods"]])
    (ART/"meshes.json").write_text(json.dumps(results,indent=2),encoding="utf-8")
    return results


def build_assets(materials_only=False):
    import unreal
    sources=json.loads((ART/"sources.json").read_text())
    geometry=json.loads((ART/"meshes.json").read_text())
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    editing=unreal.MaterialEditingLibrary
    mesh_tools=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    saved=[]

    def log(s):unreal.log("VOYAGER NATURE: "+s)
    def save(obj):
        if not assets.save_loaded_asset(obj,False):raise RuntimeError("Could not save "+obj.get_path_name())
        saved.append(obj.get_path_name())
    def node(mat,cls,**props):
        obj=editing.create_material_expression(mat,cls,0,0)
        for key,value in props.items():obj.set_editor_property(key,value)
        return obj
    def link(a,b,pin="",out=""):
        if not editing.connect_material_expressions(a,out,b,pin):raise RuntimeError("Bad material connection "+a.get_name()+" -> "+b.get_name()+"."+pin)
    def output(a,p,out=""):
        if not editing.connect_material_property(a,out,p):raise RuntimeError("Bad material output "+str(p))
    def binary(mat,cls,a,b=None,**props):
        n=node(mat,cls,**props);link(a,n,"A")
        if b is not None:link(b,n,"B")
        return n
    def scale(mat,a,s):return binary(mat,unreal.MaterialExpressionMultiply,a,const_b=s)
    def plus(mat,a,s):return binary(mat,unreal.MaterialExpressionAdd,a,const_b=s)
    def scalar(mat,name,value):return node(mat,unreal.MaterialExpressionScalarParameter,parameter_name=name,default_value=value)
    def vector(mat,name,value):return node(mat,unreal.MaterialExpressionVectorParameter,parameter_name=name,default_value=unreal.LinearColor(*value,1))
    def constant(mat,value):return node(mat,unreal.MaterialExpressionConstant,r=value)
    def unary(mat,cls,source,pin="",**props):
        n=node(mat,cls,**props);link(source,n,pin);return n
    def mask(mat,a,c):return unary(mat,unreal.MaterialExpressionComponentMask,a,r="r" in c,g="g" in c,b="b" in c,a=False)
    def blend(mat,a,b,t):
        n=node(mat,unreal.MaterialExpressionLinearInterpolate);link(a,n,"A");link(b,n,"B");link(t,n,"Alpha");return n
    def custom(mat,code,inputs):
        custom_inputs=[]
        for key in inputs:
            custom_input=unreal.CustomInput()
            custom_input.set_editor_property("input_name",key)
            custom_inputs.append(custom_input)
        n=node(mat,unreal.MaterialExpressionCustom,code=code,output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
               inputs=custom_inputs)
        for key,value in inputs.items():link(value,n,key)
        return n
    def material(name,foliage=False,masked=False):
        path=DIRECTORY+"/Materials/"+name
        mat=assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(name,DIRECTORY+"/Materials",unreal.Material,unreal.MaterialFactoryNew())
        editing.delete_all_material_expressions(mat)
        mat.set_editor_property("blend_mode",unreal.BlendMode.BLEND_MASKED if masked else unreal.BlendMode.BLEND_OPAQUE)
        mat.set_editor_property("shading_model",unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE if foliage else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        mat.set_editor_property("two_sided",foliage)
        mat.set_editor_property("dithered_lod_transition",True)
        mat.set_editor_property("tangent_space_normal",True)
        if masked:mat.set_editor_property("opacity_mask_clip_value",.32)
        editing.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
        return mat
    def finish(mat):
        editing.layout_material_expressions(mat)
        errors=editing.recompile_material(mat)
        if errors:raise RuntimeError(mat.get_name()+" shader errors: "+"; ".join(errors))
        save(mat);log("SAVED "+mat.get_name())

    textures={}
    for label,channels in sources["maps"].items():
        textures[label]={}
        for channel,relative in channels.items():
            name="T_"+label+"_"+channel;path=DIRECTORY+"/Textures/"+name
            if materials_only:
                tex=assets.load_asset(path)
                if not isinstance(tex,unreal.Texture2D):raise RuntimeError("Missing texture for material refresh "+path)
                textures[label][channel]=tex
                continue
            task=unreal.AssetImportTask()
            for key,value in {"filename":str(ROOT/relative),"destination_path":DIRECTORY+"/Textures","destination_name":name,
                              "automated":True,"replace_existing":True,"save":False,"factory":unreal.TextureFactory()}.items():task.set_editor_property(key,value)
            tools.import_asset_tasks([task]);tex=assets.load_asset(path)
            if not isinstance(tex,unreal.Texture2D):raise RuntimeError("Missing imported texture "+path)
            tex.set_editor_property("srgb",channel=="Color")
            tex.set_editor_property("compression_settings",unreal.TextureCompressionSettings.TC_NORMALMAP if channel=="Normal" else (unreal.TextureCompressionSettings.TC_MASKS if channel in ("Alpha","Roughness") else unreal.TextureCompressionSettings.TC_DEFAULT))
            tex.set_editor_property("lod_group",unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if channel=="Normal" else unreal.TextureGroup.TEXTUREGROUP_WORLD)
            tex.set_editor_property("mip_gen_settings",unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP)
            tex.set_editor_property("max_texture_size",2048)
            tex.set_editor_property("never_stream",False)
            tex.set_editor_property("address_x",unreal.TextureAddress.TA_WRAP);tex.set_editor_property("address_y",unreal.TextureAddress.TA_WRAP)
            save(tex);textures[label][channel]=tex

    def sample(mat,label,channel,uv=None):
        tex=textures[label][channel]
        sampler=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if channel=="Normal" else (unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if channel in ("Alpha","Roughness") else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        s=node(mat,unreal.MaterialExpressionTextureSample,texture=tex,sampler_type=sampler,sampler_source=unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
        if uv is not None:link(uv,s,"")
        return s

    materials={}
    palettes={"Bark":(1,1,1),"Leaves":(1,1,1),"DryLeaves":(.83,.64,.34),"Needles":(.90,1.02,.83),"Grass":(1,1,1),"Fern":(1,1,1),"Rock":(1,1,1)}
    for kind in palettes:
        label="Leaves" if kind=="DryLeaves" else kind
        foliage=kind in ("Leaves","DryLeaves","Needles","Grass","Fern")
        masked=foliage and kind!="Needles"
        mat=material("M_Nature"+kind,foliage,masked);materials[mat.get_name()]=mat
        color=binary(mat,unreal.MaterialExpressionMultiply,sample(mat,label,"Color"),vector(mat,"Tint",palettes[kind]))
        if foliage:
            variation=node(mat,unreal.MaterialExpressionPerInstanceRandom)
            color=binary(mat,unreal.MaterialExpressionMultiply,color,plus(mat,scale(mat,variation,.20),.90))
            output(scale(mat,color,.48),unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
        output(color,unreal.MaterialProperty.MP_BASE_COLOR)
        output(sample(mat,label,"Normal"),unreal.MaterialProperty.MP_NORMAL)
        output(sample(mat,label,"Roughness"),unreal.MaterialProperty.MP_ROUGHNESS,"R")
        output(constant(mat,.18 if foliage else .28),unreal.MaterialProperty.MP_SPECULAR)
        if masked:
            alpha=sample(mat,label,"Alpha")
            fade=node(mat,unreal.MaterialExpressionPerInstanceFadeAmount)
            dither_asset=assets.load_asset("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA")
            dither=node(mat,unreal.MaterialExpressionMaterialFunctionCall,material_function=dither_asset)
            link(fade,dither,"")
            opacity=binary(mat,unreal.MaterialExpressionMultiply,mask(mat,alpha,"r"),dither)
            output(opacity,unreal.MaterialProperty.MP_OPACITY_MASK)
        if foliage:
            world=node(mat,unreal.MaterialExpressionWorldPosition)
            local=unary(mat,unreal.MaterialExpressionTransformPosition,world,
                        transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
                        transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_INSTANCE)
            wind=custom(mat,"return float3(sin(T*.8+P.x*.023+P.y*.019),cos(T*.67+P.y*.031),.18*sin(T+P.x*.018))*A*saturate(P.z/H);",
                        {"T":node(mat,unreal.MaterialExpressionTime),"P":local,"A":scalar(mat,"WindAmplitude",2 if kind in ("Grass","Fern") else 2.8),"H":scalar(mat,"WindHeight",55 if kind in ("Grass","Fern") else 500)})
            wind=unary(mat,unreal.MaterialExpressionTransform,wind,
                       transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE,
                       transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
            output(wind,unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
        finish(mat)

    # Precision-safe spherical terrain, matching the runtime's existing4m
    # primitive-origin contract. Surface gradients keep flat input normals equal
    # to the geometric normal on every projection and both sides of the planet.
    mat=material("M_NatureTerrain");mat.set_editor_property("tangent_space_normal",False)
    world=node(mat,unreal.MaterialExpressionWorldPosition)
    relative=binary(mat,unreal.MaterialExpressionSubtract,world,vector(mat,"PlanetCenter",(0,0,0)))
    radial=unary(mat,unreal.MaterialExpressionNormalize,relative)
    normalized=binary(mat,unreal.MaterialExpressionDivide,relative,scalar(mat,"PlanetRadius",1000000))
    macro_coords=binary(mat,unreal.MaterialExpressionAdd,normalized,vector(mat,"SeedOffset",(0,0,0)))
    macro=unary(mat,unreal.MaterialExpressionNoise,macro_coords,noise_function=unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_TEX3D,
                scale=7.7,levels=2,quality=1,output_min=.2,output_max=.8,turbulence=False)
    local=unary(mat,unreal.MaterialExpressionTransformPosition,world,
                transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
                transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    origin=node(mat,unreal.MaterialExpressionVectorParameter,parameter_name="DetailOriginFraction",
                default_value=unreal.LinearColor(0,0,0,0),use_custom_primitive_data=True,primitive_data_index=0)
    coords=binary(mat,unreal.MaterialExpressionAdd,scale(mat,local,1/400),origin)
    normal=node(mat,unreal.MaterialExpressionVertexNormalWS)
    absnormal=unary(mat,unreal.MaterialExpressionAbs,normal)
    weights=unary(mat,unreal.MaterialExpressionPower,absnormal,"Base",const_exponent=4)
    weight_sum=custom(mat,"return (W.x+W.y+W.z).xxx;",{"W":weights})
    weights=binary(mat,unreal.MaterialExpressionDivide,weights,weight_sum)

    def triplanar(label,channel):
        samples=[sample(mat,label,channel,mask(mat,coords,plane)) for plane in ("gb","rb","rg")]
        if channel=="Normal":
            return custom(mat,"float3 g=0; g+=float3(0,X.x,X.y)/max(X.z,.2)*sign(N.x)*W.x; g+=float3(Y.x,0,Y.y)/max(Y.z,.2)*sign(N.y)*W.y; g+=float3(Z.x,Z.y,0)/max(Z.z,.2)*sign(N.z)*W.z; g-=N*dot(g,N); return normalize(N+g*S);",
                          {"X":samples[0],"Y":samples[1],"Z":samples[2],"N":normal,"W":weights,"S":scalar(mat,"NormalStrength",.65)})
        pieces=[binary(mat,unreal.MaterialExpressionMultiply,samples[i],mask(mat,weights,c)) for i,c in enumerate("rgb")]
        return binary(mat,unreal.MaterialExpressionAdd,binary(mat,unreal.MaterialExpressionAdd,pieces[0],pieces[1]),pieces[2])

    slope=binary(mat,unreal.MaterialExpressionDotProduct,normal,radial)
    slope=unary(mat,unreal.MaterialExpressionOneMinus,slope)
    rock=unary(mat,unreal.MaterialExpressionSmoothStep,slope,"Value",const_min=.075,const_max=.32)
    tint=blend(mat,vector(mat,"Tint",(.1,.20,.12)),vector(mat,"LandTint",(.18,.28,.15)),macro)
    # Photographed albedo owns the surface color. Biome accents gently modulate
    # it instead of doubling saturated channels from the legacy runtime palette.
    # Zero strength preserves the source scan; default stays within 0.902..1.098.
    modulation=custom(mat,"return lerp(float3(1,1,1),clamp(T*1.4+.55,.65,1.35),saturate(S));",
                      {"T":tint,"S":scalar(mat,"BiomeTintStrength",.28)})
    ground=binary(mat,unreal.MaterialExpressionMultiply,triplanar("Ground","Color"),modulation)
    # Orbital pixels represent whole plant communities, not a four-metre scan
    # averaged into beige. Blend only land albedo; rock slope and snow stay above.
    far_land=binary(mat,unreal.MaterialExpressionMultiply,vector(mat,"FarLandTint",(.045,.095,.025)),plus(mat,scale(mat,macro,.75),.55))
    far_blend=unary(mat,unreal.MaterialExpressionSmoothStep,node(mat,unreal.MaterialExpressionPixelDepth),"Value",const_min=50000,const_max=300000)
    ground=blend(mat,ground,far_land,far_blend)
    rocky=binary(mat,unreal.MaterialExpressionMultiply,triplanar("Rock","Color"),blend(mat,constant(mat,1),modulation,constant(mat,.45)))
    color=blend(mat,ground,rocky,rock)
    snow_latitude=unary(mat,unreal.MaterialExpressionAbs,mask(mat,radial,"b"))
    snow=unary(mat,unreal.MaterialExpressionSmoothStep,snow_latitude,"Value",const_min=.79,const_max=.99)
    snow=binary(mat,unreal.MaterialExpressionMultiply,snow,scalar(mat,"SnowCoverage",.10))
    snow=binary(mat,unreal.MaterialExpressionMultiply,snow,unary(mat,unreal.MaterialExpressionOneMinus,rock))
    color=blend(mat,color,vector(mat,"SnowTint",(.79,.84,.87)),snow)
    output(color,unreal.MaterialProperty.MP_BASE_COLOR)
    rough=blend(mat,triplanar("Ground","Roughness"),triplanar("Rock","Roughness"),rock)
    output(mask(mat,rough,"r"),unreal.MaterialProperty.MP_ROUGHNESS)
    detailnormal=blend(mat,triplanar("Ground","Normal"),triplanar("Rock","Normal"),rock)
    fade=unary(mat,unreal.MaterialExpressionSmoothStep,node(mat,unreal.MaterialExpressionPixelDepth),"Value",const_min=20000,const_max=70000)
    finalnormal=unary(mat,unreal.MaterialExpressionNormalize,blend(mat,detailnormal,normal,fade))
    output(finalnormal,unreal.MaterialProperty.MP_NORMAL)
    output(constant(mat,.18),unreal.MaterialProperty.MP_SPECULAR)
    finish(mat)

    terrain_contract=["Tint","LandTint","FarLandTint","BiomeTintStrength","PlanetCenter","PlanetRadius","SeedOffset","SnowCoverage","SnowTint","DetailOriginFraction","NormalStrength"]
    if materials_only:
        report=json.loads(REPORT.read_text())
        report["terrain_contract"]=terrain_contract
        report["material_refresh"]={"assets":saved,"biome_tint_strength":.28,
                                    "albedo_multiplier_range":[.902,1.098],
                                    "far_land_transition_cm":[50000,300000],
                                    "command_line":unreal.SystemLibrary.get_command_line()}
        REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
        log("MATERIAL REFRESH SUCCESS: 8 PBR materials saved; natural albedo tint strength 0.28")
        return

    imported=[]
    for name,record in geometry.items():
        log("IMPORT "+name)
        task=unreal.AssetImportTask();options=unreal.FbxImportUI()
        options.set_editor_property("import_mesh",True);options.set_editor_property("import_as_skeletal",False)
        options.set_editor_property("import_materials",False);options.set_editor_property("import_textures",False)
        options.set_editor_property("automated_import_should_detect_type",False)
        options.set_editor_property("mesh_type_to_import",unreal.FBXImportType.FBXIT_STATIC_MESH)
        data=options.get_editor_property("static_mesh_import_data")
        for key,value in {"combine_meshes":True,"generate_lightmap_u_vs":False,"auto_generate_collision":False,
                          "convert_scene":False,"convert_scene_unit":False,"force_front_x_axis":False,
                          "import_uniform_scale":1.0,"normal_import_method":unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS}.items():data.set_editor_property(key,value)
        for key,value in {"filename":str(ROOT/record["lods"][0]["file"]),"destination_path":DIRECTORY+"/Meshes","destination_name":name,
                          "automated":True,"replace_existing":True,"save":False,"factory":unreal.FbxFactory(),"options":options}.items():task.set_editor_property(key,value)
        tools.import_asset_tasks([task]);mesh=assets.load_asset(record["path"])
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError("Missing mesh "+record["path"])
        for index,slot in enumerate(record["slots"]):mesh.set_material(index,materials[slot])
        for lod in (1,2):
            if mesh_tools.import_lod(mesh,lod,str(ROOT/record["lods"][lod]["file"]))!=lod:raise RuntimeError("LOD import failed "+name)
        mesh_tools.set_lod_screen_sizes(mesh,[1.0,.35,.12] if name in ("SM_Broadleaf","SM_Conifer","SM_DryShrub") else [1.0,.30,.11])
        for lod in range(3):
            settings=mesh_tools.get_lod_build_settings(mesh,lod)
            settings.set_editor_property("generate_lightmap_u_vs",False)
            settings.set_editor_property("recompute_normals",False)
            settings.set_editor_property("recompute_tangents",True)
            settings.set_editor_property("use_full_precision_u_vs",True)
            mesh_tools.set_lod_build_settings(mesh,lod,settings)
        bounds=mesh.get_bounds();extent=bounds.box_extent
        expected=record["lods"][0]["bounds_cm"][2][1]
        if abs(extent.z*2-expected)>max(3,expected*.04):
            raise RuntimeError(name+" axis/scale import mismatch: height="+str(extent.z*2)+" expected="+str(expected))
        if name=="SM_Boulder":mesh_tools.add_simple_collisions(mesh,unreal.ScriptingCollisionShapeType.NDOP26)
        # Vegetation collision belongs to runtime trunks; convex whole-crown
        # collision would make invisible walls around the photographed leaves.
        save(mesh)
        entry={"name":name,"path":mesh.get_path_name(),"lod_count":mesh_tools.get_lod_count(mesh),
               "bounds_origin_cm":[bounds.origin.x,bounds.origin.y,bounds.origin.z],
               "dimensions_cm":[extent.x*2,extent.y*2,extent.z*2],"slots":record["slots"],
               "source_triangles":[x["triangles"] for x in record["lods"]]}
        if entry["lod_count"]!=3:raise RuntimeError("Expected3 saved LODs on "+name)
        imported.append(entry);log("READY "+name+" / 3 LODs / "+str(entry["dimensions_cm"]))
    report={"status":"success","assets":saved,"meshes":imported,"source_manifest":"Art/Nature/sources.json",
            "terrain_contract":terrain_contract}
    REPORT.parent.mkdir(parents=True,exist_ok=True);REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
    log("SUCCESS: natural vegetation meshes, PBR foliage and spherical terrain saved")


def refresh_lods():
    """Import only the corrected distant grass/conifer geometry, preserving LOD0."""
    import unreal
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    mesh_tools=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    geometry=json.loads((ART/"meshes.json").read_text())
    report=json.loads(REPORT.read_text())
    for name in ("SM_Grass","SM_Conifer"):
        record=geometry[name];mesh=assets.load_asset(record["path"])
        for lod in (1,2):
            if mesh_tools.import_lod(mesh,lod,str(ROOT/record["lods"][lod]["file"]))!=lod:
                raise RuntimeError("Silhouette LOD import failed "+name)
            settings=mesh_tools.get_lod_build_settings(mesh,lod)
            for key,value in {"generate_lightmap_u_vs":False,"recompute_normals":False,"recompute_tangents":True,"use_full_precision_u_vs":True}.items():
                settings.set_editor_property(key,value)
            mesh_tools.set_lod_build_settings(mesh,lod,settings)
        mesh_tools.set_lod_screen_sizes(mesh,[1,.35,.12] if name=="SM_Conifer" else [1,.30,.11])
        if not assets.save_loaded_asset(mesh,False):raise RuntimeError("Could not save corrected LODs on "+name)
        for entry in report["meshes"]:
            if entry["name"]==name:entry["source_triangles"]=[x["triangles"] for x in record["lods"]]
    report["lod_silhouette_refresh"]={"meshes":["SM_Grass","SM_Conifer"],"preserved_lod0":True}
    REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
    audit_assets()
    unreal.log("VOYAGER NATURE LOD REFRESH SUCCESS: dense grass silhouettes and fuller distant conifers; LOD0 preserved")


def audit_assets():
    """Read every imported LOD and compile materials without changing assets."""
    import unreal
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    mesh_tools=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    geometry=json.loads((ART/"meshes.json").read_text())
    report=json.loads(REPORT.read_text())
    audit=[]
    for name,record in geometry.items():
        mesh=assets.load_asset(record["path"])
        old_cpu=mesh.get_editor_property("allow_cpu_access")
        mesh.set_editor_property("allow_cpu_access",True)
        lod_info=[]
        for lod in range(mesh_tools.get_lod_count(mesh)):
            points=[];triangles=0
            for section in range(mesh.get_num_sections(lod)):
                data=unreal.ProceduralMeshLibrary.get_section_from_static_mesh(mesh,lod,section)
                points.extend(data[0]);triangles+=len(data[1])//3
            if not points:raise RuntimeError("LOD has no accessible geometry: "+name+" / "+str(lod))
            bounds=[[min(getattr(p,c) for p in points),max(getattr(p,c) for p in points)] for c in "xyz"]
            expected=record["lods"][lod]["bounds_cm"]
            # Importers may reflect Y to Unreal handedness; extents and Z-up
            # root/height must nevertheless match for every independent LOD.
            for axis in range(3):
                if abs((bounds[axis][1]-bounds[axis][0])-(expected[axis][1]-expected[axis][0]))>2:
                    raise RuntimeError(name+" LOD"+str(lod)+" changed axis/scale at import: "+str(bounds)+" vs "+str(expected))
            if abs(bounds[2][0])>2:raise RuntimeError(name+" LOD"+str(lod)+" root is not Z=0")
            if triangles!=record["lods"][lod]["triangles"]:raise RuntimeError(name+" LOD"+str(lod)+" triangle mismatch")
            lod_info.append({"lod":lod,"triangles":triangles,"bounds_cm":bounds})
        mesh.set_editor_property("allow_cpu_access",old_cpu)
        slots=[]
        for entry in mesh.get_editor_property("static_materials"):
            assigned=entry.get_editor_property("material_interface")
            slots.append({"slot":str(entry.get_editor_property("material_slot_name")),"material":assigned.get_path_name() if assigned else None})
        if len(slots)!=len(record["slots"]):raise RuntimeError(name+" contains unexpected material slots: "+str(slots))
        for i,slot in enumerate(slots):
            if not slot["material"] or record["slots"][i] not in slot["material"]:raise RuntimeError(name+" slot assignment mismatch")
        audit.append({"mesh":name,"lods":lod_info,"materials":slots,"screen_sizes":list(mesh_tools.get_lod_screen_sizes(mesh))})
        unreal.log("VOYAGER NATURE AUDIT: "+name+" / all3 LODs axis,scale,triangles,slots valid")
    compiled=[]
    for path in assets.list_assets(DIRECTORY+"/Materials",recursive=False,include_folder=False):
        mat=assets.load_asset(path)
        errors=unreal.MaterialEditingLibrary.recompile_material(mat)
        if errors:raise RuntimeError("Material compile failed "+str(path)+": "+"; ".join(errors))
        compiled.append(str(path))
    report["lod_audit"]=audit;report["compiled_materials"]=compiled
    report["audit_command_line"]=unreal.SystemLibrary.get_command_line()
    REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
    unreal.log("VOYAGER NATURE AUDIT SUCCESS: all18 imported LODs validated and8 materials compiled")


def main():
    if "--prepare" in sys.argv:
        prepare_sources();prepare_geometry();return
    if not (ART/"sources.json").is_file() or not (ART/"meshes.json").is_file():
        raise RuntimeError("Run python Scripts/bootstrap_voyager_nature.py --prepare first")
    import unreal
    try:
        if "-NatureLODRefresh" in unreal.SystemLibrary.get_command_line():refresh_lods()
        elif "-NatureAudit" in unreal.SystemLibrary.get_command_line():audit_assets()
        else:build_assets(materials_only="-NatureMaterialsOnly" in unreal.SystemLibrary.get_command_line())
    except Exception:
        import unreal
        unreal.log_error("VOYAGER NATURE FAILED:\n"+traceback.format_exc());raise


if __name__=="__main__":main()
