import math
raw=[tuple(map(float,l.split())) for l in open('path.txt') if l.strip()]
# drop the closing duplicate
def dist(a,b): return math.hypot(a[0]-b[0],a[1]-b[1])
# uniform resample (closed)
def resample(p,step):
    n=len(p); seg=[dist(p[i],p[(i+1)%n]) for i in range(n)]; L=sum(seg)
    m=int(round(L/step)); step=L/m; out=[]; i=0; acc=0.0
    for k in range(m):
        t=k*step
        while acc+seg[i]<t: acc+=seg[i]; i+=1
        u=(t-acc)/seg[i] if seg[i]>0 else 0
        a=p[i];b=p[(i+1)%n]; out.append((a[0]+(b[0]-a[0])*u,a[1]+(b[1]-a[1])*u))
    return out,L
p,L=resample(raw,1.0)
def gauss(p,sig):
    n=len(p); r=int(3*sig); w=[math.exp(-0.5*(k/sig)**2) for k in range(-r,r+1)]; sw=sum(w)
    return [(sum(w[k+r]*p[(i+k)%n][0] for k in range(-r,r+1))/sw, sum(w[k+r]*p[(i+k)%n][1] for k in range(-r,r+1))/sw) for i in range(n)]
p=gauss(p,5.0)
p,L=resample(p,1.0)
scale=7004.0/L
print('px length',L,'m/px',scale)
n=len(p)
# curvature (signed, image coords y down -> flip to world: X=-py, Y=px)
W=[(-q[1]*scale, q[0]*scale) for q in p]
def head(i): a=W[(i-3)%n]; b=W[(i+3)%n]; return math.atan2(b[1]-a[1],b[0]-a[0])
H=[head(i) for i in range(n)]
ds=7004.0/n
K=[]
for i in range(n):
    dh=(H[(i+4)%n]-H[(i-4)%n]+math.pi)%(2*math.pi)-math.pi
    K.append(-dh/(8*ds))   # + = left (heading grows to the right)
labels={1:(279,1025),2:(595,606),3:(551,488),4:(708,514),5:(1420,35),6:(1475,157),7:(1588,47),8:(1770,355),9:(1611,275),10:(1290,405),11:(1301,505),12:(1539,675),13:(1621,747),14:(1793,863),15:(1680,1052),16:(1255,756),17:(1055,685),18:(649,819),19:(669,701)}
for t,(lx,ly) in labels.items():
    i=min(range(n),key=lambda j:dist(p[j],(lx,ly)))
    # tightest radius within +-60 m
    w=int(60/ds); best=min(range(i-w,i+w),key=lambda j:-abs(K[j%n])); j=best%n
    print('T%02d s=%7.1f  tightest at s=%7.1f R=%6.1f m %s'%(t,i*ds,j*ds,1/max(abs(K[j]),1e-6),'L' if K[j]>0 else 'R'))
# turning total
print('total turning deg', sum(K)*ds*180/math.pi)
open('world.txt','w').write('\n'.join('%.2f %.2f'%q for q in W))
