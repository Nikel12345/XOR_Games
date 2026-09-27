import struct, sys, os, glob
import numpy as np

VFMT = "<11f"; VSZ = 44
SUBFMT = "<5I"; SUBSZ = 20

def load(vpath, ipath):
    with open(vpath, "rb") as f: data = f.read()
    n = struct.unpack_from("<I", data, 0)[0]
    subs = [list(struct.unpack_from(SUBFMT, data, 4 + i*SUBSZ)) for i in range(n)]
    vbase = 4 + n*SUBSZ
    nv = (len(data) - vbase) // VSZ
    verts = [list(struct.unpack_from(VFMT, data, vbase + i*VSZ)) for i in range(nv)]
    with open(ipath, "rb") as f: idata = f.read()
    idx = list(struct.unpack("<%dI" % (len(idata)//4), idata))
    return subs, verts, idx

def face_normal(p0, p1, p2):
    nn = np.cross(np.subtract(p1, p0), np.subtract(p2, p0))
    l = np.linalg.norm(nn)
    return (nn / l).tolist() if l > 1e-12 else [0.0, 1.0, 0.0]

def save(vpath, ipath, subs, verts, idx):
    with open(vpath, "wb") as f:
        f.write(struct.pack("<I", len(subs)))
        for s in subs: f.write(struct.pack(SUBFMT, *s))
        for v in verts: f.write(struct.pack(VFMT, *v))
    with open(ipath, "wb") as f:
        f.write(struct.pack("<%dI" % len(idx), *idx))

def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("usage: python fix_normals.py <verts.bin> [indices.bin] <submesh_idx ...>")

    vpath = args[0]
    if not vpath.lower().endswith(".bin"): vpath += ".bin"
    if not os.path.isfile(vpath):
        sys.exit(f"vertex file not found: {vpath}")

    ipath = None; targets = set()
    for a in args[1:]:
        if a.lower().endswith(".bin"): ipath = a
        else: targets.add(int(a))

    vstem = vpath[:-4]
    if ipath is None:
        for c in (vstem + "_i.bin", vstem + "i.bin", vstem + "_ind.bin", vstem + "_idx.bin"):
            if os.path.isfile(c): ipath = c; break
    if ipath is None or not os.path.isfile(ipath):
        d = os.path.dirname(vpath) or "."
        found = [os.path.basename(x) for x in glob.glob(os.path.join(d, "*.bin"))]
        sys.exit("index (.bin) file not found automatically.\n"
                 f"  .bin files in '{d}': {found}\n"
                 "  Pass it explicitly as 2nd arg: python fix_normals.py verts.bin THAT_index.bin 3")

    subs, verts, idx = load(vpath, ipath)
    print(f"loaded {os.path.basename(vpath)} + {os.path.basename(ipath)}: "
          f"{len(subs)} submeshes, {len(verts)} verts")
    if not targets:
        sys.exit("no submesh indices given, e.g.: ... 3")

    new_subs, new_verts, new_idx = [], [], []
    for si, (vo, io, vc, ic, mat) in enumerate(subs):
        voff, ioff = len(new_verts), len(new_idx)
        if si in targets:
            tris = ic // 3
            for t in range(tris):
                a, b, c = idx[io+3*t], idx[io+3*t+1], idx[io+3*t+2]
                va, vb, vcc = verts[vo+a], verts[vo+b], verts[vo+c]
                fn = face_normal(va[0:3], vb[0:3], vcc[0:3])
                for src in (va, vb, vcc):
                    w = list(src); w[5], w[6], w[7] = fn[0], fn[1], fn[2]
                    new_verts.append(w)
                new_idx += [3*t, 3*t+1, 3*t+2]
            new_subs.append([voff, ioff, 3*tris, 3*tris, mat])
            print(f"  sub {si}: flattened ({tris} tris)")
        else:
            for k in range(vc): new_verts.append(list(verts[vo+k]))
            for k in range(ic): new_idx.append(idx[io+k])
            new_subs.append([voff, ioff, vc, ic, mat])

    ov, oi = vstem + "_fixed.bin", vstem + "_fixed_i.bin"
    save(ov, oi, new_subs, new_verts, new_idx)
    print(f"saved -> {ov} / {oi}  ({len(new_verts)} verts)")

main()