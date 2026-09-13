"""Original Kestrel spacecraft source and Unreal import pipeline; no downloads.

python Scripts/bootstrap_voyager_ships.py --prepare
UnrealEditor-Cmd Riftbound.uproject -unattended -NullRHI -ExecutePythonScript=<file>

Authored centimetre geometry, material slots, panel maps, and three geometric LODs
are reproducible without Blender or third-party Python packages. Static meshes
are visual only: existing flight collision and movement remain authoritative.
"""
from pathlib import Path
import hashlib
import json
import math
import struct
import sys
import traceback

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "Art" / "Ships"
GEO = ART / "Meshes"
TEX = ART / "Textures"
DIRECTORY = "/Game/Ships"
REPORT = ROOT / "Saved" / "VoyagerShipsReport.json"
TAU = math.tau


def add(a, b): return tuple(x+y for x,y in zip(a,b))
def sub(a, b): return tuple(x-y for x,y in zip(a,b))
def mul(a, s): return tuple(x*s for x in a)
def dot(a, b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def norm(a): return mul(a,1/max(math.sqrt(dot(a,a)),1e-12))
def lerp(a,b,t): return add(mul(a,1-t),mul(b,t))


class Mesh:
    def __init__(self, slots):
        self.slots=slots;self.vertices=[];self.normals=[];self.uvs=[];self.faces=[]

    def v(self,p,n,uv=None):
        self.vertices.append(p);self.normals.append(norm(n))
        self.uvs.append(uv if uv is not None else (p[0]/160,p[1]/160))
        return len(self.vertices)

    def tri(self,a,b,c,slot):
        p,q,r=(self.vertices[x-1] for x in (a,b,c))
        face=cross(sub(q,p),sub(r,p))
        if dot(face,face)<1e-12:return
        expected=add(add(self.normals[a-1],self.normals[b-1]),self.normals[c-1])
        if dot(face,expected)<0:b,c=c,b
        self.faces.append((a,b,c,slot))

    def quad(self,points,slot,normal=None):
        n=normal or norm(cross(sub(points[1],points[0]),sub(points[2],points[0])))
        # Each panel gets local planar UVs; the same physical map scale on wings,
        # vertical fins, and nose avoids stretched inspection-sized seams.
        axis=norm(sub(points[1],points[0]));other=norm(cross(n,axis))
        indices=[self.v(p,n,(dot(p,axis)/160,dot(p,other)/160)) for p in points]
        for i in range(1,len(indices)-1):self.tri(indices[0],indices[i],indices[i+1],slot)

    def loft(self,rings,sides,slot,cap=True):
        """Elliptical fuselage/nacelle surfaces with explicit longitudinal normals."""
        first_dx=rings[1][0]-rings[0][0]
        if any((rings[i+1][0]-rings[i][0])*first_dx<0 for i in range(len(rings)-1)):
            # Turned nozzle collars double back into their hollow throat. Give
            # those machined folds separate longitudinal normals and arc-length
            # UVs; treating the fold as one X-monotonic loft cancelled tangents.
            distance=0.;orientation=1 if first_dx>0 else -1
            for previous,following in zip(rings,rings[1:]):
                dx,dy,dz,dry,drz=sub(following,previous)
                segment_length=math.sqrt(dx*dx+dy*dy+dz*dz+dry*dry+drz*drz)
                rows=[]
                for j,(x,y,z,ry,rz) in enumerate((previous,following)):
                    row=[]
                    for k in range(sides+1):
                        t=TAU*k/sides;c=math.cos(t);s=math.sin(t)
                        tangent=(dx,dy+dry*c,dz+drz*s);around=(0,-ry*s,rz*c)
                        normal=mul(norm(cross(around,tangent)),orientation)
                        row.append(self.v((x,y+ry*c,z+rz*s),normal,((distance+j*segment_length)/160,t*max(ry,rz)/160)))
                    rows.append(row)
                for k in range(sides):
                    a,b,c,d=rows[0][k],rows[0][k+1],rows[1][k+1],rows[1][k]
                    self.tri(a,b,c,slot);self.tri(a,c,d,slot)
                distance+=segment_length
            return
        rows=[]
        for i,(x,y,z,ry,rz) in enumerate(rings):
            previous=rings[max(0,i-1)];following=rings[min(len(rings)-1,i+1)]
            dx=following[0]-previous[0]
            denominator=dx if abs(dx)>.001 else .001
            dry=(following[3]-previous[3])/denominator
            drz=(following[4]-previous[4])/denominator
            row=[]
            for k in range(sides+1):
                t=TAU*k/sides;c=math.cos(t);s=math.sin(t)
                n=norm((-(dry*c*c+drz*s*s),c/max(ry,.1)*max(ry,rz),s/max(rz,.1)*max(ry,rz)))
                row.append(self.v((x,y+ry*c,z+rz*s),n,(x/160,t*max(ry,rz)/160)))
            rows.append(row)
        for i in range(len(rows)-1):
            for k in range(sides):
                a,b,c,d=rows[i][k],rows[i][k+1],rows[i+1][k+1],rows[i+1][k]
                self.tri(a,b,c,slot);self.tri(a,c,d,slot)
        if cap:
            for i,normal in ((0,(-1,0,0)),(-1,(1,0,0))):
                x,y,z,ry,rz=rings[i]
                self.quad([(x,y+ry*math.cos(TAU*k/sides),z+rz*math.sin(TAU*k/sides)) for k in range(sides)],slot,normal)

    def tube(self,points,radius,sides,slot):
        rows=[]
        for i,p in enumerate(points):
            along=norm(sub(points[min(i+1,len(points)-1)],points[max(i-1,0)]))
            x=norm(cross(along,(0,0,1) if abs(along[2])<.9 else (0,1,0)));y=cross(along,x)
            rows.append([self.v(add(p,mul(add(mul(x,math.cos(TAU*k/sides)),mul(y,math.sin(TAU*k/sides))),radius)),
                                add(mul(x,math.cos(TAU*k/sides)),mul(y,math.sin(TAU*k/sides))),
                                (k/sides,i/2)) for k in range(sides+1)])
        for i in range(len(rows)-1):
            for k in range(sides):
                a,b,c,d=rows[i][k],rows[i][k+1],rows[i+1][k+1],rows[i+1][k]
                self.tri(a,b,c,slot);self.tri(a,c,d,slot)
        for i,normal in ((0,norm(sub(points[0],points[1]))),(-1,norm(sub(points[-1],points[-2])))):
            axis=norm(cross(normal,(0,0,1) if abs(normal[2])<.9 else (0,1,0)))
            other=cross(normal,axis);ids=[]
            for source in rows[i]:
                p=self.vertices[source-1];relative=sub(p,points[i])
                ids.append(self.v(p,normal,(.5+dot(relative,axis)/(2*radius),.5+dot(relative,other)/(2*radius))))
            center=self.v(points[i],normal,(.5,.5))
            for k in range(sides):self.tri(center,ids[k],ids[k+1],slot)

    def panel(self,outline,depth,slot):
        """Closed custom polygon extrusion (fins, hatch plates, skid soles)."""
        n=norm(cross(sub(outline[1],outline[0]),sub(outline[2],outline[0])))
        top=[add(p,mul(n,depth*.5)) for p in outline];bottom=[sub(p,mul(n,depth*.5)) for p in outline]
        self.quad(top,slot,n);self.quad(bottom,slot,mul(n,-1))
        for i in range(len(outline)):
            j=(i+1)%len(outline);self.quad([bottom[i],bottom[j],top[j],top[i]],slot)

    def chamfer_box(self,center,size,bevel,slot):
        x,y,z=center;sx,sy,sz=mul(size,.5);b=min(bevel,sx*.8,sy*.8,sz*.8)
        # Three rounded-octagonal cross-sections form a bevelled machined casing.
        outline=[(-sy+b,-sz), (sy-b,-sz),(sy,-sz+b),(sy,sz-b),(sy-b,sz),(-sy+b,sz),(-sy,sz-b),(-sy,-sz+b)]
        rings=[]
        for xx,scale in ((-sx,.88),(-sx+b,1),(sx-b,1),(sx,.88)):
            rings.append([(x+xx,y+yy*scale,z+zz*scale) for yy,zz in outline])
        for i in range(3):
            for j in range(8):self.quad([rings[i][j],rings[i][(j+1)%8],rings[i+1][(j+1)%8],rings[i+1][j]],slot)
        self.quad(rings[0],slot,(-1,0,0));self.quad(rings[-1],slot,(1,0,0))

    def write(self,name,lod):
        path=GEO/(name+"_LOD"+str(lod)+".obj")
        with path.open("w",encoding="ascii",newline="\n") as out:
            out.write("# Original Kestrel spacecraft. Centimetres; +X forward; +Z up.\nmtllib ship_slots.mtl\n")
            for p in self.vertices:out.write("v %.6f %.6f %.6f\n"%p)
            for p in self.uvs:out.write("vt %.7f %.7f\n"%p)
            for n in self.normals:out.write("vn %.7f %.7f %.7f\n"%n)
            for slot in self.slots:
                out.write("usemtl "+slot+"\n")
                for a,b,c,s in self.faces:
                    if s==slot:out.write("f "+" ".join(f"{i}/{i}/{i}" for i in (a,b,c))+"\n")
        if any(not math.isfinite(c) for p in self.vertices for c in p):raise RuntimeError("Nonfinite geometry")
        if set(s for a,b,c,s in self.faces)!=set(self.slots):raise RuntimeError("Empty or unknown slot "+name)
        return {"file":str(path.relative_to(ROOT)).replace("\\","/"),"triangles":len(self.faces),
                "vertices":len(self.vertices),"bounds_cm":[[min(p[i] for p in self.vertices),max(p[i] for p in self.vertices)] for i in range(3)],
                "sha256":hashlib.sha256(path.read_bytes()).hexdigest()}


HULL_SLOTS=["HullPaint","DarkPanels","BrushedMetal","Livery","Interior","Instrument","Thruster"]


def hull(lod):
    m=Mesh(HULL_SLOTS);sides=(40,24,12)[lod];detail=(12,8,4)[lod]
    # A continuous lifting body. Oval keel blends into broad shoulders and a
    # flattened pointed nose instead of a collection of engine basic shapes.
    sections=[(-315,0,-10,62,35),(-285,0,-2,94,58),(-220,0,0,113,69),(-120,0,0,120,76),
              (0,0,-2,115,70),(110,0,-8,95,55),(210,0,-15,65,40),(300,0,-22,32,19),(368,0,-26,3,3)]
    m.loft(sections,sides,"HullPaint")
    # Layered ventral thermal shield with a tight silhouette distinct from paint.
    m.loft([(-278,0,-36,66,27),(-150,0,-40,95,32),(20,0,-43,88,23),(200,0,-34,46,12),(312,0,-28,12,3)],sides,"DarkPanels")
    for side in (-1,1):
        stations=[(72,200,-280,24),(140,120,-287,19),(230,-15,-295,12),(330,-152,-302,7),(392,-240,-304,3)]
        for upper in (True,False):
            rows=[]
            for y,front,back,t in stations:
                row=[]
                for k in range(detail+1):
                    u=k/detail;x=front+(back-front)*u
                    thickness=t*(math.sin(math.pi*u)**.63)
                    z=(7 if upper else 0)+(thickness if upper else -thickness*.52)+(y-72)*.022
                    slope=t*.63*math.pi*math.cos(math.pi*u)*max(math.sin(math.pi*u),.035)**(-.37)/(back-front)
                    n=(-slope,side*.022,1) if upper else (-slope*.52,-side*.022,-1)
                    row.append(m.v((x,side*y,z),n,(x/160,y/160)))
                rows.append(row)
            for j in range(len(rows)-1):
                for k in range(detail):
                    a,b,c,d=rows[j][k],rows[j][k+1],rows[j+1][k+1],rows[j+1][k]
                    m.tri(a,b,c,"HullPaint");m.tri(a,c,d,"HullPaint")
        # Folded wing tips, inlaid livery chevrons and a physical trailing flap.
        m.panel([(-200,side*275,18),(-249,side*352,17),(-287,side*369,15),(-260,side*278,20)],1.4,"Livery")
        m.panel([(-241,side*145,16),(-276,side*265,14),(-298,side*283,12),(-282,side*142,15)],2.2,"DarkPanels")
        # Long nacelles share the wing root. Open concentric rear collars expose
        # recessed nozzles and a luminous throat, without exterior cone exhausts.
        y=side*179
        m.loft([(-356,y,-5,33,32),(-330,y,-3,44,42),(-273,y,0,48,46),(-100,y,2,44,42),(-18,y,4,31,30),(15,y,4,21,20)],sides,"DarkPanels",False)
        m.loft([(-333,y,-3,45,43),(-305,y,-1,48,46),(-135,y,2,46,44),(-102,y,2,45,43)],sides,"HullPaint",False)
        m.loft([(-357,y,-5,33,32),(-362,y,-5,34,33),(-365,y,-5,28,27),(-338,y,-5,22,21)],sides,"BrushedMetal",False)
        m.loft([(-337,y,-5,22,21),(-329,y,-5,18,17)],sides,"Thruster")
        m.loft([(-15,y,4,31,30),(18,y,4,21,20),(20,y,4,15,14),(-10,y,4,19,18)],sides,"BrushedMetal",False)
        m.loft([(-11,y,4,19,18),(-12,y,4,18,17)],sides,"DarkPanels")
        # Canted aft fins have a separate airfoil thickness and capped edges.
        fin=[(-290,side*106,56),(-121,side*109,64),(-241,side*166,228),(-290,side*176,231)]
        m.panel(fin,9,"HullPaint")
        m.panel([(-287,side*147,165),(-244,side*140,168),(-268,side*164,218),(-287,side*169,219)],1.4,"Livery")
        # Shoulder service access plates, inset cooling grille, and small fasteners.
        m.panel([(-164,side*65,68),(-84,side*67,72),(-84,side*100,51),(-164,side*100,48)],1.5,"DarkPanels")
        for k in range((11,7,3)[lod]):
            x=-158+k*6
            m.tube([(x,side*69,70),(x,side*94,54)],1.5,4,"BrushedMetal")
        for k in range((8,4,2)[lod]):
            x=-245+k*42
            m.tube([(x,side*110,24),(x+5,side*110,24)],1.5,6,"BrushedMetal")
        # Paired weapons retain existing actor-space muzzle coordinates.
        m.loft([(28,side*192,-9,10,9),(91,side*192,-9,8,7),(133,side*192,-9,6,5),(145,side*192,-9,7,6)],12 if lod<2 else 8,"BrushedMetal")
        m.loft([(145.1,side*192,-9,4.5,3.5),(145.5,side*192,-9,4,3)],12 if lod<2 else 8,"Thruster")
        # Nose livery traces the hull shoulder, separated from structural paneling.
        m.tube([(299,side*19,-10),(227,side*42,10),(169,side*59,24)],3.2,6,"Livery")
    # Deep cockpit well, seats, console, and framing are visible through glass.
    m.chamfer_box((63,0,64),(172,119,22),6,"Interior")
    for side in (-1,1):
        m.chamfer_box((24,side*28,85),(42,40,15),5,"Interior")
        m.chamfer_box((3,side*28,110),(13,38,54),4,"Interior")
        m.chamfer_box((3,side*28,139),(15,26,18),3,"Interior")
        m.chamfer_box((98,side*34,86),(57,12,28),3,"DarkPanels")
        m.tube([(80,side*30,79),(78,side*30,94),(84,side*30,101)],2.8,8 if lod<2 else 4,"BrushedMetal")
        m.panel([(115,side*10,100),(115,side*54,100),(137,side*54,83),(137,side*10,83)],2,"Instrument")
    # Five connected canopy frames follow the same loft as the glass.
    canopy_sections=[(-53,48,79),(0,72,147),(94,67,155),(180,45,101),(203,21,52)]
    for side in (-1,1):
        m.tube([(x,side*y*.82,z*.91) for x,y,z in canopy_sections],3.5,(10,8,5)[lod],"BrushedMetal")
        m.tube([(-59,side*55,65),(7,side*81,69),(129,side*69,50),(206,side*20,40)],4,(10,8,5)[lod],"DarkPanels")
    for i in (1,2):
        x,y,z=canopy_sections[i]
        m.tube([(x,math.cos(math.pi*k/8)*y,z-30+math.sin(math.pi*k/8)*30) for k in range(9)],3.3,(8,6,4)[lod],"BrushedMetal")
    return m


def canopy(lod):
    m=Mesh(["CanopyGlass"]);rows=[];segments=(20,12,6)[lod]
    for x,y,z,bottom in [(-53,48,79,67),(0,72,147,70),(94,67,155,62),(180,45,101,45),(203,21,52,39)]:
        row=[]
        for k in range(segments+1):
            t=math.pi*k/segments
            row.append(m.v((x,y*math.cos(t),bottom+(z-bottom)*math.sin(t)),(0,math.cos(t),math.sin(t)),(x/250,k/segments)))
        rows.append(row)
    for i in range(4):
        for j in range(segments):
            a,b,c,d=rows[i][j],rows[i][j+1],rows[i+1][j+1],rows[i+1][j]
            m.tri(a,b,c,"CanopyGlass");m.tri(a,c,d,"CanopyGlass")
    return m


def gear(lod):
    m=Mesh(["BrushedMetal","DarkPanels","HullPaint"]);sides=(12,8,6)[lod]
    for x,y in ((183,0),(-182,-118),(-182,118)):
        foot=(x-18,y,-145)
        m.tube([(x,y,-42),(x-2,y,-80)],8,sides,"DarkPanels")
        m.tube([(x-2,y,-76),(x-18,y,-137)],4.8,sides,"BrushedMetal")
        m.tube([(x-42,y,-58),(x-18,y,-127)],3.8,sides,"BrushedMetal")
        m.chamfer_box(foot,(66,34,15),5,"DarkPanels")
        m.chamfer_box((x-19,y,-136),(39,20,5),2,"BrushedMetal")
        m.panel([(x+28,y-25,-48),(x-43,y-25,-59),(x-43,y+25,-59),(x+28,y+25,-48)],3,"HullPaint")
    return m


def exhaust(lod):
    m=Mesh(["Thruster"])
    m.loft([(-2,0,0,22,21),(-14,0,0,20,19),(-32,0,0,16,15),(-66,0,0,8,7),(-102,0,0,.5,.5)],(24,16,8)[lod],"Thruster")
    return m


def beacon(lod):
    m=Mesh(["Beacon"])
    m.chamfer_box((0,0,0),(22,12,8),3,"Beacon")
    return m


def write_tga(path,width,height,pixels):
    path.write_bytes(struct.pack("<BBBHHBHHHHBB",0,0,2,0,0,0,0,0,width,height,24,32)+pixels)


def prepare_textures():
    size=512;color=bytearray();rough=bytearray();normal=bytearray()
    def height(x,y):
        x%=size;y%=size
        # Alternate 80cm rectangular maintenance panels; shallow stamped seams.
        seam=min(x%128,127-x%128,y%256,255-y%256)
        rivet=any((x%128-a)**2+(y%256-b)**2<6 for a in (7,120) for b in (7,248))
        return 0 if seam<2 else (.38 if seam<3 else (.58 if rivet else 1))
    for y in range(size):
        for x in range(size):
            h=height(x,y);noise=((x*73856093^y*19349663)&255)/255-.5
            panel=((x//128)*13+(y//256)*7)%9
            v=int(max(0,min(255,211+panel+noise*6-(1-h)*120)))
            color.extend((v,v,v))
            r=int(max(0,min(255,112+noise*15+(1-h)*58)))
            rough.extend((r,r,r))
            dx=height(x+1,y)-height(x-1,y);dy=height(x,y+1)-height(x,y-1)
            nx,ny,nz=norm((-dx*.32,dy*.32,1))
            normal.extend((int((nz*.5+.5)*255),int((ny*.5+.5)*255),int((nx*.5+.5)*255)))
    for name,pixels in (("T_KestrelPanels",color),("T_KestrelRoughness",rough),("T_KestrelNormal",normal)):
        write_tga(TEX/(name+".tga"),size,size,pixels)


def prepare():
    GEO.mkdir(parents=True,exist_ok=True);TEX.mkdir(parents=True,exist_ok=True)
    allslots=HULL_SLOTS+["CanopyGlass","Beacon"]
    (GEO/"ship_slots.mtl").write_text("\n".join("newmtl "+s+"\nKd 0.6 0.6 0.6\n" for s in allslots),encoding="ascii")
    records={}
    for name,builder in (("SM_KestrelHull",hull),("SM_KestrelCanopy",canopy),("SM_KestrelGear",gear),("SM_KestrelExhaust",exhaust),("SM_KestrelBeacon",beacon)):
        meshes=[builder(lod) for lod in range(3)]
        records[name]={"path":DIRECTORY+"/Meshes/"+name,"slots":meshes[0].slots,"lods":[m.write(name,lod) for lod,m in enumerate(meshes)]}
        print(name,[len(m.faces) for m in meshes])
    prepare_textures()
    (ART/"meshes.json").write_text(json.dumps(records,indent=2),encoding="utf-8")
    (ART/"PROVENANCE.md").write_text("# Kestrel spacecraft\n\nOriginal geometry and panel textures authored in `Scripts/bootstrap_voyager_ships.py` for this game. No source meshes, models, scanned surfaces, brand designs, or external textures were downloaded. Source OBJ/MTL files and TGA maps are reproducible with Python's standard library. Units are centimetres, +X forward, +Z up. Three LODs preserve the hull silhouette.\n\nThe explorer, patrol, and raider liveries share the original airframe and use runtime material tint parameters. The geometry is decorative and does not replace gameplay collision.\n",encoding="utf-8")
    print("VOYAGER SHIPS PREPARE SUCCESS")


def preview():
    """Optional source-geometry review, not a substitute for an Unreal render."""
    import numpy as np
    import zlib
    width,height=1200,800
    frame=np.zeros((height,width,3),dtype=np.float64);frame[:]=(.055,.068,.085)
    depth=np.full((height,width),-1e20)
    toward=np.array(norm((1.2,-1.6,1.05)));right=np.array(norm(cross((0,0,1),toward)));up=np.cross(toward,right)
    key=np.array(norm((.7,-.9,1.4)));fill=np.array(norm((-.8,.8,.6)))
    colors={"HullPaint":(.57,.61,.60),"DarkPanels":(.042,.055,.065),"BrushedMetal":(.37,.42,.47),
            "Livery":(.018,.16,.25),"Interior":(.028,.035,.042),"Instrument":(.03,.55,.75),
            "Thruster":(.12,.64,1),"CanopyGlass":(.13,.31,.39)}
    for mesh in (hull(0),gear(0),canopy(0)):
        vertices=np.array(mesh.vertices);normals=np.array(mesh.normals)
        projected=np.column_stack((vertices@right*1.12+width*.5,height*.54-vertices@up*1.12,vertices@toward))
        for a,b,c,slot in mesh.faces:
            ids=np.array((a-1,b-1,c-1));p=projected[ids];n=normals[ids].mean(axis=0)
            if np.dot(n,toward)<-.05 and slot!="CanopyGlass":continue
            x0=max(0,int(np.floor(p[:,0].min())));x1=min(width-1,int(np.ceil(p[:,0].max())))
            y0=max(0,int(np.floor(p[:,1].min())));y1=min(height-1,int(np.ceil(p[:,1].max())))
            if x0>x1 or y0>y1:continue
            den=(p[1,1]-p[2,1])*(p[0,0]-p[2,0])+(p[2,0]-p[1,0])*(p[0,1]-p[2,1])
            if abs(den)<1e-7:continue
            yy,xx=np.mgrid[y0:y1+1,x0:x1+1]
            aa=((p[1,1]-p[2,1])*(xx-p[2,0])+(p[2,0]-p[1,0])*(yy-p[2,1]))/den
            bb=((p[2,1]-p[0,1])*(xx-p[2,0])+(p[0,0]-p[2,0])*(yy-p[2,1]))/den;cc=1-aa-bb
            z=aa*p[0,2]+bb*p[1,2]+cc*p[2,2]
            visible=(aa>=0)&(bb>=0)&(cc>=0)&(z>depth[y0:y1+1,x0:x1+1])
            brightness=.20+.67*max(0,np.dot(n,key))+.22*max(0,np.dot(n,fill))
            color=np.array(colors[slot])*brightness
            if slot in ("Instrument","Thruster"):color=np.array(colors[slot])
            crop=frame[y0:y1+1,x0:x1+1]
            if slot=="CanopyGlass":crop[visible]=crop[visible]*.70+color*.30
            else:crop[visible]=color;depth[y0:y1+1,x0:x1+1][visible]=z[visible]
    rgb=(np.clip(frame,0,1)**(1/1.7)*255).astype(np.uint8)
    raw=b"".join(b"\0"+rgb[y].tobytes() for y in range(height))
    def chunk(kind,data):return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data)&0xffffffff)
    image=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",width,height,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(raw,6))+chunk(b"IEND",b"")
    (ART/"Kestrel-SourcePreview.png").write_bytes(image)
    print("Source mesh preview saved; this image does not validate Unreal materials or lighting.")


def build_assets(hull_only=False):
    import unreal
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    mesh_tools=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    editing=unreal.MaterialEditingLibrary
    saved=[];materials={};textures={}
    records=json.loads((ART/"meshes.json").read_text())
    if hull_only:
        records={name:records[name] for name in ("SM_KestrelHull","SM_KestrelGear")}
        materials={slot:assets.load_asset(DIRECTORY+"/Materials/M_Kestrel"+slot) for slot in HULL_SLOTS}
        if not all(materials.values()):raise RuntimeError("Hull refresh requires existing ship materials")
    def save(obj):
        if not assets.save_loaded_asset(obj,False):raise RuntimeError("Could not save "+obj.get_path_name())
        saved.append(obj.get_path_name())
    def node(mat,cls,**props):
        n=editing.create_material_expression(mat,cls,0,0)
        for key,value in props.items():n.set_editor_property(key,value)
        return n
    def link(a,b,pin="",out=""):
        if not editing.connect_material_expressions(a,out,b,pin):raise RuntimeError("Bad material link "+pin)
    def output(n,prop):
        if not editing.connect_material_property(n,"",prop):raise RuntimeError("Bad output")
    def scalar(mat,name,value):return node(mat,unreal.MaterialExpressionScalarParameter,parameter_name=name,default_value=value)
    def vector(mat,name,value):return node(mat,unreal.MaterialExpressionVectorParameter,parameter_name=name,default_value=unreal.LinearColor(*value,1))
    def multiply(mat,a,b):
        n=node(mat,unreal.MaterialExpressionMultiply);link(a,n,"A");link(b,n,"B");return n
    for name in (() if hull_only else ("T_KestrelPanels","T_KestrelRoughness","T_KestrelNormal")):
        task=unreal.AssetImportTask()
        for key,value in {"filename":str(TEX/(name+".tga")),"destination_path":DIRECTORY+"/Textures","destination_name":name,
                          "automated":True,"replace_existing":True,"save":False,"factory":unreal.TextureFactory()}.items():task.set_editor_property(key,value)
        tools.import_asset_tasks([task]);tex=assets.load_asset(DIRECTORY+"/Textures/"+name)
        if not isinstance(tex,unreal.Texture2D):raise RuntimeError("Missing texture "+name)
        tex.set_editor_property("srgb",name=="T_KestrelPanels")
        tex.set_editor_property("compression_settings",unreal.TextureCompressionSettings.TC_NORMALMAP if "Normal" in name else unreal.TextureCompressionSettings.TC_DEFAULT)
        tex.set_editor_property("max_texture_size",512);save(tex);textures[name]=tex
    palettes={"HullPaint":((.56,.60,.59),.25,.42),"DarkPanels":((.036,.049,.055),.65,.38),
              "BrushedMetal":((.32,.36,.39),.88,.25),"Livery":((.012,.12,.20),.20,.36),
              "Interior":((.028,.032,.034),.05,.70),"Instrument":((.025,.27,.38),.1,.3),
              "Thruster":((.10,.58,1),.05,.25),"CanopyGlass":((.08,.16,.19),.08,.09),"Beacon":((.025,.24,1),0,.2)}
    for slot,(color,metal,roughness) in ({} if hull_only else palettes).items():
        name="M_Kestrel"+slot;path=DIRECTORY+"/Materials/"+name
        mat=assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(name,DIRECTORY+"/Materials",unreal.Material,unreal.MaterialFactoryNew())
        editing.delete_all_material_expressions(mat)
        mat.set_editor_property("blend_mode",unreal.BlendMode.BLEND_TRANSLUCENT if slot=="CanopyGlass" else unreal.BlendMode.BLEND_OPAQUE)
        mat.set_editor_property("shading_model",unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        mat.set_editor_property("two_sided",slot=="CanopyGlass")
        mat.set_editor_property("dithered_lod_transition",slot!="CanopyGlass")
        tint=vector(mat,"Tint",color);base=tint
        if slot in ("HullPaint","DarkPanels","BrushedMetal","Livery"):
            sample=node(mat,unreal.MaterialExpressionTextureSample,texture=textures["T_KestrelPanels"],sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
            base=multiply(mat,tint,sample)
            normals=node(mat,unreal.MaterialExpressionTextureSample,texture=textures["T_KestrelNormal"],sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
            output(normals,unreal.MaterialProperty.MP_NORMAL)
        output(base,unreal.MaterialProperty.MP_BASE_COLOR)
        output(scalar(mat,"Metallic",metal),unreal.MaterialProperty.MP_METALLIC)
        r=scalar(mat,"Roughness",roughness)
        if slot in ("HullPaint","DarkPanels","BrushedMetal","Livery"):
            sample=node(mat,unreal.MaterialExpressionTextureSample,texture=textures["T_KestrelRoughness"],sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            boost=multiply(mat,sample,scalar(mat,"RoughnessTextureScale",2.0));r=multiply(mat,r,boost)
        output(r,unreal.MaterialProperty.MP_ROUGHNESS)
        output(scalar(mat,"Specular",.5),unreal.MaterialProperty.MP_SPECULAR)
        if slot in ("Instrument","Thruster","Beacon"):
            output(multiply(mat,tint,scalar(mat,"Glow",1.2 if slot=="Instrument" else 5)),unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if slot=="CanopyGlass":
            mat.set_editor_property("translucency_lighting_mode",unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
            output(scalar(mat,"Opacity",.24),unreal.MaterialProperty.MP_OPACITY)
        editing.layout_material_expressions(mat)
        errors=editing.recompile_material(mat)
        if errors:raise RuntimeError("Material compile failed "+name+": "+str(errors))
        save(mat);materials[slot]=mat
    imported=[]
    for name,record in records.items():
        unreal.log("VOYAGER SHIPS IMPORT "+name)
        task=unreal.AssetImportTask();options=unreal.FbxImportUI()
        for key,value in {"import_mesh":True,"import_as_skeletal":False,"import_materials":False,"import_textures":False,
                          "automated_import_should_detect_type":False,"mesh_type_to_import":unreal.FBXImportType.FBXIT_STATIC_MESH}.items():options.set_editor_property(key,value)
        data=options.get_editor_property("static_mesh_import_data")
        for key,value in {"combine_meshes":True,"generate_lightmap_u_vs":False,"auto_generate_collision":False,
                          "convert_scene":False,"convert_scene_unit":False,"force_front_x_axis":False,"import_uniform_scale":1.,
                          "normal_import_method":unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS}.items():data.set_editor_property(key,value)
        for key,value in {"filename":str(ROOT/record["lods"][0]["file"]),"destination_path":DIRECTORY+"/Meshes","destination_name":name,
                          "automated":True,"replace_existing":True,"save":False,"factory":unreal.FbxFactory(),"options":options}.items():task.set_editor_property(key,value)
        tools.import_asset_tasks([task]);mesh=assets.load_asset(record["path"])
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError("Missing imported mesh "+name)
        for i,slot in enumerate(record["slots"]):mesh.set_material(i,materials[slot])
        for lod in (1,2):
            if mesh_tools.import_lod(mesh,lod,str(ROOT/record["lods"][lod]["file"]))!=lod:raise RuntimeError("LOD import failed "+name)
        mesh_tools.set_lod_screen_sizes(mesh,[1,.38,.13])
        for lod in range(3):
            settings=mesh_tools.get_lod_build_settings(mesh,lod)
            for key,value in {"generate_lightmap_u_vs":False,"recompute_normals":False,"recompute_tangents":True,"use_full_precision_u_vs":True}.items():settings.set_editor_property(key,value)
            mesh_tools.set_lod_build_settings(mesh,lod,settings)
        mesh.set_editor_property("allow_cpu_access",True)
        # Inspect actual imported vertex buffers, rather than relying on the
        # importer reporting success with a rotated, reduced, or empty result.
        actual=[]
        for lod in range(3):
            vertices=[];triangles=0
            for section in range(mesh.get_num_sections(lod)):
                buffers=unreal.ProceduralMeshLibrary.get_section_from_static_mesh(mesh,lod,section)
                vertices.extend(buffers[0]);triangles+=len(buffers[1])//3
            if not vertices:raise RuntimeError("Empty imported LOD "+name)
            bounds=[[min(getattr(p,c) for p in vertices),max(getattr(p,c) for p in vertices)] for c in "xyz"]
            expected=record["lods"][lod]["bounds_cm"]
            if triangles!=record["lods"][lod]["triangles"]:raise RuntimeError("Triangle mismatch "+name)
            if any(abs((bounds[i][1]-bounds[i][0])-(expected[i][1]-expected[i][0]))>1 for i in range(3)):raise RuntimeError("Axis or scale mismatch "+name)
            actual.append({"lod":lod,"triangles":triangles,"bounds_cm":bounds})
        mesh.set_editor_property("allow_cpu_access",False);save(mesh)
        imported.append({"path":mesh.get_path_name(),"slots":record["slots"],"lods":actual})
        unreal.log("VOYAGER SHIPS READY "+name+" triangles="+str([x["triangles"] for x in actual]))
    REPORT.parent.mkdir(parents=True,exist_ok=True)
    if hull_only:
        report=json.loads(REPORT.read_text())
        replacements={item["path"]:item for item in imported}
        report["meshes"]=[replacements.get(item["path"],item) for item in report["meshes"]]
        report["hull_tangent_refresh"]="Machined return-fold collars use separate normals and arc-length UVs; strut caps use proper planar normals and UVs."
    else:report={"status":"success","assets":saved,"meshes":imported,"source":"Art/Ships/PROVENANCE.md", "physics_modified":False}
    REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
    unreal.log("VOYAGER SHIPS SUCCESS: "+("hull tangent refresh saved" if hull_only else "original Kestrel meshes, 15 inspected LODs, PBR materials saved"))


def audit_assets():
    """Read final material assignments and tangent frames after mesh rebuild."""
    import unreal
    assets=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    records=json.loads((ART/"meshes.json").read_text())
    report=json.loads(REPORT.read_text());audit=[]
    for name,record in records.items():
        mesh=assets.load_asset(record["path"])
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError("Missing saved mesh "+name)
        actual=[]
        for slot in mesh.get_editor_property("static_materials"):
            mat=slot.get_editor_property("material_interface")
            actual.append(mat.get_path_name() if mat else None)
        expected=[DIRECTORY+"/Materials/M_Kestrel"+s+".M_Kestrel"+s for s in record["slots"]]
        if actual!=expected:raise RuntimeError("Material slot mismatch "+name+": "+str(actual))
        old_cpu=mesh.get_editor_property("allow_cpu_access");mesh.set_editor_property("allow_cpu_access",True)
        total=0;bad=0;smallest=1.0
        for lod in range(3):
            for section in range(mesh.get_num_sections(lod)):
                buffers=unreal.ProceduralMeshLibrary.get_section_from_static_mesh(mesh,lod,section)
                for n,t in zip(buffers[2],buffers[4]):
                    tx=t.get_editor_property("tangent_x")
                    n=(n.x,n.y,n.z);t=(tx.x,tx.y,tx.z)
                    area=math.sqrt(dot(cross(n,t),cross(n,t)))
                    if not math.isfinite(area) or area<.1:bad+=1
                    smallest=min(smallest,area);total+=1
        mesh.set_editor_property("allow_cpu_access",old_cpu)
        if total==0 or bad:raise RuntimeError("Invalid final tangent frames "+name+": "+str(bad)+" / "+str(total))
        audit.append({"mesh":name,"materials":actual,"tangent_vertices":total,"invalid_tangent_frames":bad,"minimum_bitangent_length":smallest})
        unreal.log("VOYAGER SHIPS AUDIT "+name+" material_slots="+str(len(actual))+" valid_tangent_vertices="+str(total))
    report["saved_asset_audit"]=audit;report["status"]="success";report.pop("last_error",None)
    REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
    unreal.log("VOYAGER SHIPS AUDIT SUCCESS: saved material slots and final tangent frames valid")


if __name__=="__main__":
    if "--prepare" in sys.argv:prepare()
    elif "--preview" in sys.argv:preview()
    else:
        try:
            import unreal
            if "-ShipAssetAudit" in unreal.SystemLibrary.get_command_line():audit_assets()
            else:
                build_assets(hull_only="-ShipHullRefresh" in unreal.SystemLibrary.get_command_line())
                audit_assets()
        except Exception as error:
            import unreal
            report=json.loads(REPORT.read_text()) if REPORT.is_file() else {}
            report["status"]="failed";report["last_error"]=str(error)
            REPORT.parent.mkdir(parents=True,exist_ok=True)
            REPORT.write_text(json.dumps(report,indent=2),encoding="utf-8")
            unreal.log_error("VOYAGER SHIPS FAILED:\n"+traceback.format_exc());raise
