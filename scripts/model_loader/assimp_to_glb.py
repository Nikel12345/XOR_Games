#!/usr/bin/env python3
"""assimp -> GLB bridge for convert_model.py (FBX, OBJ, … — anything assimp reads).

trimesh cannot read FBX at all, and Blender (the .blend bridge in
convert_model.py) is not installed here. assimp reads binary FBX, so we import
with it and re-emit a GLB that trimesh understands — the binary layout is still
produced by convert_model.py alone.

Node transforms are baked by assimp (PreTransformVertices), same reason
extract_meshes() bakes them for glTF: the FBX keeps the object transform on the
node, and unbaked submeshes drift apart.

FBX authoring units are usually centimeters while the engine works in meters
(generate.py: FLOOR_H = 3.5 units per floor) — pass --scale 0.01 for such a file.

--base shifts the result so the lowest point sits at y = 0: an asset authored
around its own center hangs half-underground otherwise, and every other model in
game/models stands on its base.

Usage: python assimp_to_glb.py raw/model.fbx raw/model.glb [--scale 0.01] [--base]
"""

import sys

import numpy as np
import trimesh

import assimp_py

FLAGS = (assimp_py.Process_Triangulate
         | assimp_py.Process_GenSmoothNormals
         | assimp_py.Process_PreTransformVertices
         | assimp_py.Process_JoinIdenticalVertices)


def drop_degenerate(verts, faces, normals, uv):
    """Remove zero-area faces and any vertex left unreferenced.

    They draw nothing, but they are not harmless: trimesh drops the loaded
    normals as soon as convert_model.py bakes a transform and recomputes them
    from the faces — a vertex whose every face is degenerate then gets a ZERO
    normal, and normalize(0) in the TBN is NaN, not just dark. This OBJ ships 142
    such faces.
    """
    tri = verts[faces]
    area = np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
    keep = area > 1e-9
    if keep.all():
        return verts, faces, normals, uv

    faces = faces[keep]
    used = np.zeros(len(verts), bool)
    used[faces.ravel()] = True
    remap = np.full(len(verts), -1, np.int64)
    remap[used] = np.arange(used.sum())
    print(f"      dropped {int((~keep).sum())} degenerate face(s), {int((~used).sum())} orphan vert(s)")
    return (verts[used], remap[faces].astype(np.uint32),
            None if normals is None else normals[used],
            None if uv is None else uv[used])


def repair_normals(verts, faces, normals):
    """Fill zero-length vertex normals from the adjacent faces.

    This OBJ ships 154 of them (degenerate faces in the source). A zero normal is
    not merely dark: the shader does normalize(worldNormal) inside the TBN, so it
    becomes NaN and paints the pixel garbage. Everything else is left untouched —
    the authored normals carry the smoothing groups.
    """
    bad = np.linalg.norm(normals, axis=1) < 1e-6
    if not bad.any():
        return normals

    tri = verts[faces]
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])   # площадь-взвешенные
    acc = np.zeros_like(normals)
    for c in range(3):
        np.add.at(acc, faces[:, c], fn)
    lens = np.linalg.norm(acc, axis=1, keepdims=True)
    fixed = np.where(lens > 1e-12, acc / np.maximum(lens, 1e-12), np.array([0.0, 1.0, 0.0], np.float32))

    normals = normals.copy()
    normals[bad] = fixed[bad]
    print(f"      repaired {int(bad.sum())} zero-length normal(s)")
    return normals


def main():
    src, dst = sys.argv[1], sys.argv[2]
    scale = float(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1.0
    base = "--base" in sys.argv
    scene = assimp_py.import_file(src, FLAGS)
    print(f"Loaded {src}: {len(scene.meshes)} mesh(es)")

    out = trimesh.Scene()
    for i, m in enumerate(scene.meshes):
        # assimp_py returns FLAT float arrays, not (N,k) — reshape by vertex count
        verts = np.asarray(m.vertices, np.float32).reshape(-1, 3) * scale
        if base:
            verts[:, 1] -= verts[:, 1].min()
        faces = np.asarray(m.indices, np.uint32).reshape(-1, 3)
        nv = len(verts)
        normals = np.asarray(m.normals, np.float32).reshape(nv, 3) if m.normals else None
        # texcoords is a list of UV sets; a set is 2 or 3 components per vertex
        uv = None
        if m.texcoords:
            tc = np.asarray(m.texcoords[0], np.float32).reshape(nv, -1)
            uv = np.ascontiguousarray(tc[:, :2])

        verts, faces, normals, uv = drop_degenerate(verts, faces, normals, uv)
        if normals is not None:
            normals = repair_normals(verts, faces, normals)

        mesh = trimesh.Trimesh(vertices=verts, faces=faces,
                               vertex_normals=normals, process=False)
        if uv is not None:
            mesh.visual = trimesh.visual.TextureVisuals(uv=uv)
        print(f"  [{i}] verts={len(verts)} faces={len(faces)} uv={'yes' if uv is not None else 'NO'}")
        out.add_geometry(mesh, geom_name=f"mesh_{i}")

    out.export(dst)
    print(f"Wrote {dst}")


if __name__ == "__main__":
    main()
