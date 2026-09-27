import struct, sys
d = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<I", d, 0)[0]
subs = [struct.unpack_from("<5I", d, 4 + i*20)[:3] for i in range(n)]  # (vo, io, vc)
vbase = 4 + n*20
V = lambda i: struct.unpack_from("<11f", d, vbase + i*44)
print("submeshes:", n)
for si,(vo,io,vc) in enumerate(subs):
    sx=sy=sz=0; horiz=0
    bb=[1e9,-1e9,1e9,-1e9,1e9,-1e9]  # xmin xmax ymin ymax zmin zmax
    for k in range(vc):
        v=V(vo+k); sx+=v[5]; sy+=v[6]; sz+=v[7]
        if abs(v[6])<0.5: horiz+=1
        bb[0]=min(bb[0],v[0]); bb[1]=max(bb[1],v[0])
        bb[2]=min(bb[2],v[1]); bb[3]=max(bb[3],v[1])
        bb[4]=min(bb[4],v[2]); bb[5]=max(bb[5],v[2])
    print(f"sub {si}: vc={vc} | X[{bb[0]:.1f},{bb[1]:.1f}] Y[{bb[2]:.2f},{bb[3]:.2f}] Z[{bb[4]:.1f},{bb[5]:.1f}]"
          f" | mean N=({sx/vc:+.2f},{sy/vc:+.2f},{sz/vc:+.2f}) | horiz={horiz}/{vc}")