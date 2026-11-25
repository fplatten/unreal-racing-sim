import math
W=[tuple(map(float,l.split())) for l in open('shaped.txt')]
n=len(W); L=7004.0; ds=L/n
def head(i): a=W[(i-2)%n]; b=W[(i+2)%n]; return math.atan2(b[1]-a[1],b[0]-a[0])
H=[head(i) for i in range(n)]
K=[-((H[(i+3)%n]-H[(i-3)%n]+math.pi)%(2*math.pi)-math.pi)/(6*ds) for i in range(n)]
# corners: contiguous regions with |K|>1/500 of same sign
reg=[];i=0
sg=lambda k: 0 if abs(k)<1/500 else (1 if k>0 else -1)
# start at a straight
start=next(j for j in range(n) if sg(K[j])==0)
j=start;cnt=0
cur=None
while cnt<n:
    s=sg(K[j%n])
    if s!=0 and (cur is None or cur['sign']!=s):
        if cur: reg.append(cur)
        cur={'sign':s,'a':j,'b':j,'turn':0,'peak':0}
    if s!=0:
        cur['b']=j; cur['turn']+=K[j%n]*ds; cur['peak']=max(cur['peak'],abs(K[j%n]))
    elif cur: reg.append(cur); cur=None
    j+=1;cnt+=1
if cur: reg.append(cur)
for r in reg:
    if abs(r['turn'])<math.radians(8): continue
    print('s %6.0f-%6.0f %s turn %6.1f deg  minR %6.1f  len %5.0f'%((r['a']%n)*ds,(r['b']%n)*ds,'L' if r['sign']>0 else 'R',math.degrees(r['turn']),1/r['peak'],(r['b']-r['a'])*ds))
