import math
Q=[tuple(map(float,l.split())) for l in open('shaped.txt')]
n=len(Q); L=7004.0; ds=L/n
shift=190.0
i0=int(round(shift/ds))
Q=Q[i0:]+Q[:i0]
# downsample to 5 m
step=int(round(5.0/ds))
pts=Q[::step]
print(len(pts),'control points')
# overlay in px for checking (inverse of X=-py*s, Y=px*s)
sc=1.3271502016210106
open('final_px.txt','w').write('\n'.join('%.1f %.1f'%(q[1]/sc,-q[0]/sc) for q in pts))
lines=[]
for k in range(0,len(pts),6):
    lines.append('\t'+' '.join('{%.2ff, %.2ff},'%q for q in pts[k:k+6]))
open('RoadLayout.inl','w').write('\n'.join(lines)+'\n')
