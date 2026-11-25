import math
pts=[tuple(map(float,l.split()[:2])) for l in open('pts.txt')]
start=(590.0,860.0); heading=math.atan2(990-860,300-590)
cur=min(pts,key=lambda p:(p[0]-start[0])**2+(p[1]-start[1])**2)
used=set(); path=[cur]
def near(p,q): return math.hypot(p[0]-q[0],p[1]-q[1])
def pick(maxd,maxa,wa):
    best=None;bc=1e9
    for i,p in enumerate(pts):
        if i in used: continue
        d=near(p,cur)
        if d<3 or d>maxd: continue
        ang=math.atan2(p[1]-cur[1],p[0]-cur[0])
        da=abs((ang-heading+math.pi)%(2*math.pi)-math.pi)
        if da>maxa: continue
        c=d*(1+wa*da)
        if c<bc: bc=c;best=i
    return best
for it in range(3000):
    best=pick(25,1.4,3)
    if best is None: best=pick(90,0.6,6)
    if best is None: print('stuck at',cur); break
    p=pts[best]
    for i,q in enumerate(pts):
        if near(q,cur)<7 or near(q,p)<5: used.add(i)
    nh=math.atan2(p[1]-cur[1],p[0]-cur[0])
    heading=heading+0.7*((nh-heading+math.pi)%(2*math.pi)-math.pi)
    cur=p; path.append(p)
    if len(path)>50 and near(p,path[0])<14: break
print(len(path), path[-1])
open('path.txt','w').write('\n'.join('%.1f %.1f'%p for p in path))
