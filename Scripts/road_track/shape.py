import math
W=[tuple(map(float,l.split())) for l in open('world.txt')]
n=len(W); L=7004.0; ds=L/n
# light smoothing of positions first
def gauss_pts(p,sig):
    r=int(3*sig); w=[math.exp(-0.5*(k/sig)**2) for k in range(-r,r+1)]; sw=sum(w)
    return [(sum(w[k+r]*p[(i+k)%n][0] for k in range(-r,r+1))/sw, sum(w[k+r]*p[(i+k)%n][1] for k in range(-r,r+1))/sw) for i in range(n)]
P=gauss_pts(W,3)
th=[]
for i in range(n):
    a=P[(i-1)%n]; b=P[(i+1)%n]; th.append(math.atan2(b[1]-a[1],b[0]-a[0]))
# unwrap
for i in range(1,n):
    while th[i]-th[i-1]>math.pi: th[i]-=2*math.pi
    while th[i]-th[i-1]<-math.pi: th[i]+=2*math.pi
total=th[-1]-th[0]+((th[0]-th[-1]+math.pi)%(2*math.pi)-math.pi)
print('total heading change',math.degrees(total))
phi=[th[i]-total*i/n for i in range(n)]
# sigma map (meters) by s ranges
SIG=[(0,390,40),(390,480,20),(480,1080,32),(1080,1320,27),(1320,2420,48),(2420,2750,20),(2750,3000,35),(3000,3200,18),(3200,3400,24),(3400,3790,45),(3790,4080,52),(4080,4500,30),(4500,4790,30),(4790,4940,35),(4940,5040,28),(5040,5140,40),(5140,5380,44),(5380,5960,48),(5960,6320,95),(6320,6690,45),(6690,6880,16),(6880,7004,40)]
def sig_at(s):
    s%=L
    # blend between ranges over 40 m
    for a,b,v in SIG:
        if a<=s<b:
            idx=SIG.index((a,b,v))
            prev=SIG[idx-1][2]; nxt=SIG[(idx+1)%len(SIG)][2]
            if s-a<30: t=(s-a)/30; return prev+(v-prev)*(0.5+0.5*t) if False else (prev*(1-(0.5+0.5*t))+v*(0.5+0.5*t))
            if b-s<30: t=(b-s)/30; return nxt*(1-(0.5+0.5*t))+v*(0.5+0.5*t)
            return v
    return 40
newphi=[]
for i in range(n):
    sg=sig_at(i*ds)/ds; r=int(3*sg)
    acc=0;sw=0
    for k in range(-r,r+1):
        w=math.exp(-0.5*(k/sg)**2); acc+=w*phi[(i+k)%n]; sw+=w
    newphi.append(acc/sw)
th2=[newphi[i]+total*i/n for i in range(n)]
# integrate
x=[0.0];y=[0.0]
for i in range(n):
    x.append(x[-1]+ds*math.cos(th2[i])); y.append(y[-1]+ds*math.sin(th2[i]))
gx=x[n]-x[0]; gy=y[n]-y[0]
print('closure gap',gx,gy)
Q=[(x[i]-gx*i/n, y[i]-gy*i/n) for i in range(n)]
# align with the original at s=0 region: translate so centroid matches
cx=sum(q[0] for q in Q)/n; cy=sum(q[1] for q in Q)/n; ox=sum(p[0] for p in W)/n; oy=sum(p[1] for p in W)/n
Q=[(q[0]-cx+ox,q[1]-cy+oy) for q in Q]
dev=max(math.hypot(Q[i][0]-W[i][0],Q[i][1]-W[i][1]) for i in range(n))
print('max deviation from trace (m)',dev)
open('shaped.txt','w').write('\n'.join('%.2f %.2f'%q for q in Q))
