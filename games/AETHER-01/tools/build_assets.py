#!/usr/bin/env python3
"""Procedural AETHER-01 production-style static meshes; exports glTF 2.0 + binary buffers."""
import json, math, struct, uuid, hashlib
from pathlib import Path
from collections import defaultdict
ROOT=Path(__file__).resolve().parents[1]
class Asset:
 def __init__(self,name): self.name=name; self.meshes=[]; self.nodes=[]; self.mats=[]; self.buf=bytearray(); self.views=[]; self.acc=[]
 def mat(self,n,c,metal=0,rough=.6,emiss=None):
  m={'name':n,'pbrMetallicRoughness':{'baseColorFactor':[*c,1.0],'metallicFactor':metal,'roughnessFactor':rough}}
  if emiss: m['emissiveFactor']=emiss
  self.mats.append(m); return len(self.mats)-1
 def geom(self,n,verts,faces,mat):
  # Keep the source topology indexed. Per-vertex normals are area-weighted for smooth terrain and forms.
  normals=[[0.0,0.0,0.0] for _ in verts]; ix=[]
  for face in faces:
   a,b,c=[verts[i] for i in face[:3]]; u=[b[k]-a[k] for k in range(3)];v=[c[k]-a[k] for k in range(3)]
   norm=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
   for i in face[:3]:
    for k in range(3):normals[i][k]+=norm[k]
   ix.extend(face[:3])
  for nrm in normals:
   length=math.sqrt(sum(x*x for x in nrm)) or 1
   for k in range(3):nrm[k]/=length
  p=[q for xyz in verts for q in xyz]; no=[q for xyz in normals for q in xyz]
  off=len(self.buf); self.buf.extend(struct.pack('<%sf'%len(p),*p)); pv=len(self.views); self.views.append({'buffer':0,'byteOffset':off,'byteLength':len(self.buf)-off,'target':34962})
  po=len(self.buf); self.buf.extend(struct.pack('<%sf'%len(no),*no)); nv=len(self.views); self.views.append({'buffer':0,'byteOffset':po,'byteLength':len(self.buf)-po,'target':34962})
  io=len(self.buf); self.buf.extend(struct.pack('<%sH'%len(ix),*ix)); iv=len(self.views); self.views.append({'buffer':0,'byteOffset':io,'byteLength':len(self.buf)-io,'target':34963})
  while len(self.buf)%4:self.buf.append(0)
  mn=[min(xyz[k] for xyz in verts) for k in range(3)]; mx=[max(xyz[k] for xyz in verts) for k in range(3)]
  pi=len(self.acc); self.acc.append({'bufferView':pv,'componentType':5126,'count':len(verts),'type':'VEC3','min':mn,'max':mx}); ni=len(self.acc); self.acc.append({'bufferView':nv,'componentType':5126,'count':len(verts),'type':'VEC3'}); ii=len(self.acc); self.acc.append({'bufferView':iv,'componentType':5123,'count':len(ix),'type':'SCALAR'})
  mi=len(self.meshes); self.meshes.append({'name':n,'primitives':[{'attributes':{'POSITION':pi,'NORMAL':ni},'indices':ii,'material':mat,'mode':4}]}); self.nodes.append({'name':n,'mesh':mi})
 def box(self,n,c,s,mat,bevel=.08):
  x,y,z=c; a,b,d=[q/2 for q in s]; r=min(bevel,a*.35,b*.35,d*.35)
  # 6 bevelled face panels with corner chamfers and inset center
  v=[]; f=[]
  for axis,sg in ((0,1),(0,-1),(1,1),(1,-1),(2,1),(2,-1)):
   oth=[k for k in range(3) if k!=axis]; loop=[]
   for u,w in ((-1,-1),(1,-1),(1,1),(-1,1)):
    p=[x,y,z]; p[axis]=[x,y,z][axis]+sg*([a,b,d][axis]-r); p[oth[0]]=[x,y,z][oth[0]]+u*([a,b,d][oth[0]]-r); p[oth[1]]=[x,y,z][oth[1]]+w*([a,b,d][oth[1]]-r); loop.append(len(v)); v.append(p)
   ctr=len(v); p=[x,y,z]; p[axis]+=(sg*([a,b,d][axis]-r));v.append(p)
   for q in range(4): f.append((ctr,loop[q],loop[(q+1)%4]))
  self.geom(n,v,f,mat)
 def ellipsoid(self,n,c,radii,mat,seg=12,rings=8):
  x,y,z=c;rx,ry,rz=radii;v=[];f=[]
  for j in range(rings+1):
   th=math.pi*j/rings
   for i in range(seg):
    ph=2*math.pi*i/seg;v.append((x+rx*math.sin(th)*math.cos(ph),y+ry*math.cos(th),z+rz*math.sin(th)*math.sin(ph)))
  for j in range(rings):
   for i in range(seg):a=j*seg+i;b=j*seg+(i+1)%seg;c=(j+1)*seg+(i+1)%seg;d=(j+1)*seg+i;f.extend(((a,b,d),(b,c,d)))
  self.geom(n,v,f,mat)
 def cylinder(self,n,c,r,h,mat,seg=12,axis='y',r2=None):
  r2=r if r2 is None else r2;v=[];f=[];x,y,z=c
  for yy,rr in ((-h/2,r),(h/2,r2)):
   for i in range(seg):
    a=2*math.pi*i/seg; q=(math.cos(a)*rr,math.sin(a)*rr)
    v.append((x+q[0],y+yy,z+q[1]) if axis=='y' else (x+q[0],y+q[1],z+yy))
  for i in range(seg):a=i;b=(i+1)%seg;f.extend(((a,b,seg+a),(b,seg+b,seg+a)))
  self.geom(n,v,f,mat)
 def ring(self,n,c,r,minor,mat,seg=24,tube=6):
  x,y,z=c;v=[];f=[]
  for i in range(seg):
   a=2*math.pi*i/seg
   for j in range(tube):
    b=2*math.pi*j/tube;rr=r+minor*math.cos(b);v.append((x+rr*math.cos(a),y+minor*math.sin(b),z+rr*math.sin(a)))
  for i in range(seg):
   for j in range(tube):a=i*tube+j;b=i*tube+(j+1)%tube;c=((i+1)%seg)*tube+(j+1)%tube;d=((i+1)%seg)*tube+j;f.extend(((a,b,d),(b,c,d)))
  self.geom(n,v,f,mat)
 def save(self,path):
  path.parent.mkdir(parents=True,exist_ok=True); bpath=path.with_suffix('.bin'); bpath.write_bytes(self.buf)
  nodes=self.nodes
  roots=list(range(len(nodes)))
  if self.name=='Kael':
   nodes=[{'name':'Kael | gameplay root','children':roots,'translation':[0,0.16,0],'scale':[0.84,0.83,0.84]}]+nodes; roots=[0]
  doc={'asset':{'version':'2.0','generator':'AETHER-01 Asset Foundry 1.0'},'scene':0,'scenes':[{'nodes':roots}],'nodes':nodes,'meshes':self.meshes,'materials':self.mats,'buffers':[{'uri':bpath.name,'byteLength':len(self.buf)}],'bufferViews':self.views,'accessors':self.acc}
  raw=json.dumps(doc,indent=2,separators=(',',': ')).encode();path.write_bytes(raw)
  # stable engine AssetMeta preserving deterministic IDs across regeneration
  guid=str(uuid.uuid5(uuid.NAMESPACE_URL,'aether01:'+str(path.relative_to(ROOT))))
  def sub(k):return str(uuid.uuid5(uuid.NAMESPACE_URL,'aether01:'+str(path.relative_to(ROOT))+':'+k))
  meta={'$type':'AssetMeta','$v':1,'guid':guid,'importer':'Model','importer_version':1,'labels':['AETHER-01','production-assets'],'settings':{},'source_hash':'fnv1a64:'+format(fnv(raw), '016x'),'sub_assets':{f'material:{i}':{'guid':sub('material:'+str(i)),'importer':'Material'} for i in range(len(self.mats))}|{f'mesh:{i}':{'guid':sub('mesh:'+str(i)),'importer':'Mesh'} for i in range(len(self.meshes))}}
  path.with_name(path.name+'.ameta').write_text(json.dumps(meta,indent=2)+'\n')
def fnv(data):
 h=0xcbf29ce484222325
 for b in data:h=((h^b)*0x100000001b3)&0xffffffffffffffff
 return h
# Kael, last light recon operator: layered matte gunmetal plates, ceramic shoulder shells, amber optics.
a=Asset('Kael'); suit=a.mat('01 | carbon weave',(.045,.075,.09),.38,.72); armor=a.mat('02 | deep petrol ceramic',(.075,.19,.22),.62,.3); edge=a.mat('03 | brushed titanium',(.34,.43,.45),.78,.24); amber=a.mat('04 | ion amber',(.98,.39,.075),.45,.28,[1,.18,.025]); visor=a.mat('05 | smoked gold visor',(.36,.19,.07),.72,.16); cloth=a.mat('06 | graphite soft goods',(.075,.085,.09),.08,.88)
# body
for n,c,s,m in [('Torso pressure suit',(0,1.10,0),(.48,.67,.27),suit),('Chest cuirass',(0,1.35,-.015),(.54,.44,.34),armor),('Abdominal flex',(0,.91,.015),(.38,.28,.26),cloth),('Pelvis harness',(0,.73,0),(.5,.22,.3),suit),('Backplate',(0,1.22,.19),(.43,.68,.25),edge),('Life-support pack',(0,1.35,.34),(.48,.62,.25),armor),('Pack top beacon',(0,1.69,.34),(.15,.10,.14),amber)]:a.box(n,c,s,m)
# chest ribs & insignia
for i in range(3):a.box('Rib inlay '+str(i),((i-1)*.14,1.31,-.192),(.035,.25,.018),edge)
a.box('AETHER chest mark',(0,1.48,-.195),(.16,.045,.018),amber,.01)
# helmet with smooth ellipsoid and articulated visor
for n,c,r,m in [('Helmet shell',(0,1.83,0),(.24,.29,.23),armor),('Faceplate',(0,1.82,-.175),(.205,.17,.065),visor),('Left comms pod',(-.25,1.82,0),(.065,.12,.11),edge),('Right comms pod',(.25,1.82,0),(.065,.12,.11),edge),('Optic left',(-.085,1.85,-.238),(.035,.018,.012),amber),('Optic right',(.085,1.85,-.238),(.035,.018,.012),amber),('Helmet crown stripe',(0,2.09,0),(.045,.025,.19),edge)]:a.ellipsoid(n,c,r,m)
# shoulder plates, arm shells, elbows, gauntlets
for side,label in [(-1,'L'),(1,'R')]:
 for n,c,r,m in [('Shoulder pauldron',(side*.37,1.47,0),(.22,.20,.24),edge),('Shoulder outer shell',(side*.41,1.49,-.02),(.18,.13,.23),armor),('Upper arm',(side*.48,1.21,0),(.12,.26,.13),suit),('Elbow guard',(side*.49,.99,-.01),(.14,.11,.15),edge),('Forearm bracer',(side*.49,.81,-.035),(.14,.21,.15),armor),('Glove',(side*.48,.63,-.035),(.13,.10,.14),cloth),('Wrist light',(side*.50,.85,-.17),(.035,.02,.018),amber)]:a.ellipsoid(label+' '+n,c,r,m)
 a.box(label+' vambrace panel',(side*.49,.81,-.167),(.09,.14,.018),edge)
# legs, layered thigh/knee/shin/boot, all proportions fit 1.94
for side,label in [(-1,'L'),(1,'R')]:
 x=side*.15
 for n,c,s,m in [('Thigh undersuit',(x,.48,0),(.22,.48,.23),suit),('Thigh plate',(x,.50,-.06),(.24,.34,.25),armor),('Knee cup',(x,.255,-.04),(.22,.16,.24),edge),('Shin greave',(x,.07,-.01),(.20,.30,.22),armor),('Boot',(x,-.115,-.075),(.25,.17,.39),cloth),('Boot toe cap',(x,-.10,-.24),(.21,.10,.12),edge)]:a.box(label+' '+n,c,s,m)
 a.box(label+' thigh ID',(x,.48,-.193),(.035,.12,.02),amber,.012)
# tactical belts, pouches and antenna
for side in (-1,1):
 a.box('Hip utility pouch',(side*.30,.70,-.11),(.16,.21,.19),edge)
 a.box('Pouch amber clasp',(side*.30,.70,-.214),(.055,.055,.015),amber,.008)
a.cylinder('Pack antenna',(0,1.85,.43),.018,.34,edge,8)
a.save(ROOT/'Content/Characters/Kael/Kael_Recon.gltf')
kael_parts=len(a.meshes)
# AEGIS precision carbine: horizontal along Z, muzzle toward -Z, ergonomic stock at +Z.
w=Asset('AEGIS'); polymer=w.mat('01 | graphite polymer',(.065,.09,.10),.24,.62); metal=w.mat('02 | hard anodized titanium',(.30,.38,.40),.8,.25); ceramic=w.mat('03 | dark teal ceramic',(.055,.20,.22),.55,.29); glow=w.mat('04 | energized amber',(.98,.34,.045),.4,.23,[1,.12,.015]); glass=w.mat('05 | optic glass',(.12,.25,.27),.62,.13)
def wb(n,c,s,m,b=.04):w.box(n,c,s,m,b)
wb('Monolithic receiver',(0,0,0),(.21,.25,.56),ceramic);wb('Upper rail',(0,.15,-.035),(.14,.075,.79),metal);wb('Handguard',(0,-.005,-.38),(.20,.20,.43),polymer);wb('Barrel shroud',(0,0,-.70),(.115,.12,.34),metal);w.cylinder('Muzzle brake',(0,0,-.91),.077,.19,metal,12,'z',.09);w.ring('Muzzle ion ring',(0,0,-.81),.078,.014,glow,16,6)
wb('Adjustable stock',(0,.025,.52),(.17,.16,.37),polymer);wb('Recoil pad',(0,.02,.72),(.21,.19,.08),metal);wb('Pistol grip',(0,-.235,.11),(.14,.33,.17),polymer);wb('Grip checkering',(0,-.26,.015),(.145,.08,.16),metal)
wb('Magazine',(0,-.25,-.20),(.15,.35,.20),metal);wb('Magazine base',(0,-.43,-.20),(.17,.06,.22),ceramic);wb('Ammo counter',(0,-.27,-.31),(.08,.11,.018),glow)
wb('Optic housing',(0,.30,.03),(.16,.17,.26),polymer);w.cylinder('Optic lens bezel',(0,.30,-.105),.075,.04,metal,16,'z');w.cylinder('Optic aperture',(0,.30,-.13),.052,.012,glass,16,'z');wb('Rear sight',(0,.25,.30),(.10,.09,.10),metal)
for side in [-1,1]:
 wb(('L' if side<0 else 'R')+' receiver pin',(side*.112,.035,.10),(.018,.055,.07),glow,.008)
 wb(('L' if side<0 else 'R')+' heat vent',(side*.108,-.015,-.40),(.018,.07,.23),glow,.01)
wb('Charging handle',(.145,.085,.08),(.12,.045,.055),metal,.012);wb('Selector',(.12,-.07,.15),(.07,.035,.13),glow,.01);wb('Foregrip',(0,-.19,-.50),(.12,.24,.13),polymer)
w.save(ROOT/'Content/Weapons/AEGIS/AEGIS_PrecisionCarbine.gltf')
weapon_parts=len(w.meshes)
# Last Light: playable heightfield canyon, braided river ribbons, fractured basalt spires, ruin bridge and gate.
e=Asset('Last Light Valley'); basalt=e.mat('01 | obsidian basalt',(.055,.075,.09),.22,.92); stone=e.mat('02 | oxidized stone',(.21,.20,.17),.12,.96); sand=e.mat('03 | iron ochre strata',(.37,.19,.095),.08,.91); water=e.mat('04 | glacial teal',(.035,.28,.31),.38,.19,[.015,.075,.09]); ruin=e.mat('05 | ancient alloy',(.19,.26,.26),.72,.4); light=e.mat('06 | amber beacons',(.95,.28,.025),.35,.22,[1,.17,.015]); fog=e.mat('07 | pale mineral',(.47,.43,.34),.04,.88)
# terrain grid 65x65, 96m square. valley channel meanders into distant choke; height is strong perimeter uplift with mesas.
N=65; size=96; verts=[]; faces=[]
def height(x,z):
 nx=x/(size/2); nz=z/(size/2); wall=max(0,abs(nx)-.22)*18; ends=max(0,abs(nz)-.65)*8
 ridge=2.1*math.sin(x*.21+z*.13)+1.5*math.sin(x*.39-z*.12)+.7*math.sin(z*.29)
 channel=abs(x-(7*math.sin(z*.095)+3*math.sin(z*.21)))
 cut=max(0,8-channel)*.36
 return wall+ends+ridge-cut
for j in range(N):
 z=-size/2+size*j/(N-1)
 for i in range(N):
  x=-size/2+size*i/(N-1); verts.append((x,height(x,z),z))
for j in range(N-1):
 for i in range(N-1):a=j*N+i;b=a+1;c=a+N+1;d=a+N;faces.extend(((a,d,b),(b,d,c)))
e.geom('96m x 96m eroded canyon basin',verts,faces,sand)
# River ribbon meanders down valley with tapered reflective ribbons
rv=[]; rf=[]; M=140
for j in range(M):
 z=-size/2+size*j/(M-1); x=7*math.sin(z*.095)+3*math.sin(z*.21); y=height(x,z)+.16; width=1.8+1.0*(.5+.5*math.sin(z*.14))
 rv.extend(((x-width,y,z),(x+width,y,z)))
 if j:
  q=2*j;rf.extend(((q-2,q-1,q),(q-1,q+1,q)))
e.geom('Glacier meltwater',rv,rf,water)
# Random but deterministic rock formations with multi-tier tapered octagonal columns and debris field
import random
rng=random.Random(1331)
def spire(n,x,z,r,h,mat):
 y=height(x,z); sides=7; v=[]; f=[]
 # silhouette offset tiered rings, chunky faceted summit
 rings=[(0,r*1.18),(h*.12,r),(h*.40,r*.72),(h*.69,r*.51),(h*.88,r*.34),(h,r*.08)]
 wob=[rng.uniform(.84,1.17) for _ in range(sides)]
 for yy,rr in rings:
  ox=rng.uniform(-.13,.13)*r; oz=rng.uniform(-.13,.13)*r
  for i in range(sides):
   ang=math.tau*i/sides; v.append((x+ox+math.cos(ang)*rr*wob[i],y+yy,z+oz+math.sin(ang)*rr*wob[i]))
 for k in range(len(rings)-1):
  for i in range(sides):a=k*sides+i;b=k*sides+(i+1)%sides;c=(k+1)*sides+(i+1)%sides;d=(k+1)*sides+i; f.extend(((a,b,d),(b,c,d)))
 e.geom(n,v,f,mat)
for i in range(30):
 z=rng.uniform(-45,45); x=rng.uniform(-39,39)
 if abs(x-(7*math.sin(z*.095)+3*math.sin(z*.21)))<9: x+=19 if x<0 else -19
 spire('Basalt fang %02d'%i,x,z,rng.uniform(1.3,4.8),rng.uniform(7,23),basalt if i%3 else stone)
for i in range(65):
 z=rng.uniform(-44,44);x=rng.uniform(-44,44)
 if abs(x-(7*math.sin(z*.095)+3*math.sin(z*.21)))<4:continue
 e.ellipsoid('Talus shard %02d'%i,(x,height(x,z)+.35,z),(rng.uniform(.35,1.6),rng.uniform(.25,.8),rng.uniform(.4,1.6)),basalt,7,5)
# Ancient transit bridge spans canyon close to exit; tapered deck pylons and amber guide lights
for n,c,s,m in [('West abutment',(-10,7,29),(4,2,5),ruin),('East abutment',(10,7,29),(4,2,5),ruin),('Bridge deck',(0,9,29),(19,1.5,5),ruin),('Bridge crown',(0,10,29),(19,.18,.24),light),('Gate monolith L',(-8,12,37),(2.2,13,2.6),basalt),('Gate monolith R',(8,12,37),(2.2,13,2.6),basalt),('Gate header',(0,18,37),(18,2.1,3.2),ruin),('Gate lintel',(0,16.8,35.35),(12,.4,.22),light),('Checkpoint plinth',(0,2,8),(4,1,3),ruin)]:e.box(n,c,s,m)
for x in [-8,-4,0,4,8]:
 e.cylinder('Bridge pier',(x,4.3,29),.58,8,ruin,8); e.ring('Pylon signal',(x,11,29),.21,.045,light,12,5)
# layered cliff strata arcs, add recognizable distant industrial/civic citadel and towers
for i in range(6):
 z=35+i*1.2;y=height(0,z)+7+i*.8
 e.box('Citadel retaining terrace '+str(i),(0,y,z),(18-i*1.2,1.15,2.3),stone)
for side in [-1,1]:
 x=side*9.5
 e.box('Citadel spire',(x,20,45),(2,25,2),ruin)
 for k in range(5):e.box('Citadel crossbeam '+str(side)+' '+str(k),(x-side*.3,10+k*3,44),(3,.28,.35),light if k==4 else ruin)
# Amber signal obelisks at player route
for i,(x,z) in enumerate([(-4,-18),(3,-5),(-2,12)]):
 y=height(x,z);e.cylinder('Wayfinder amber obelisk '+str(i),(x,y+1.7,z),.14,3.4,ruin,8);e.ellipsoid('Signal crystal '+str(i),(x,y+3.45,z),(.20,.25,.20),light,8,5)
e.save(ROOT/'Content/Environments/LastLight/LastLight_Valley.gltf')
valley_parts=len(e.meshes)
# index and structure guide
(ROOT/'ASSET_CATALOG.json').write_text(json.dumps({'game':'AETHER-01: First Contact','schema':'glTF 2.0','assets':[{'id':'kael-recon','file':'Content/Characters/Kael/Kael_Recon.gltf','role':'player character static production mesh','height_m':2.0,'parts':kael_parts},{'id':'aegis-carbine','file':'Content/Weapons/AEGIS/AEGIS_PrecisionCarbine.gltf','role':'player weapon','parts':weapon_parts},{'id':'last-light-valley','file':'Content/Environments/LastLight/LastLight_Valley.gltf','role':'96m playable terrain map','parts':valley_parts,'triangles':'~11k terrain + modular set dressing'}]},indent=2)+'\n')
