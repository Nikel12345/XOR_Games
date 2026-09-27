#!/usr/bin/env python3
"""Split a multi-object .blend into one GLB per object, for convert_model.py.

Why not the .blend bridge in convert_model.py: it shells out to an installed
Blender, and there is none on this machine. assimp reads the DNA of this file
(BLENDER-v291) directly.

Two fixes are applied per object, and both are the point of this script:
  * Z-up -> Y-up. Blender keeps height in Z; the engine (and every model already
    in game/models) keeps it in Y. (x, y, z) -> (x, z, -y) is a proper
    rotation, so the winding order survives.
  * Own pivot. The node transforms here are just the layout of the objects on
    the scene grid — baking them would move every tower away from its origin.
    We drop them and instead recenter each object on its own footprint: XZ to
    the bbox center (= the mast axis for a tower), Y so the base sits at 0.

Usage: python blend_split.py raw/telecom.blend raw/telecom
       -> raw/telecom1.glb .. raw/telecomN.glb (one per mesh, file order)
"""

import sys

import numpy as np
import trimesh

import assimp_py

FLAGS = (assimp_py.Process_Triangulate
         | assimp_py.Process_JoinIdenticalVertices
         | assimp_py.Process_GenSmoothNormals)


def main():
    src, stem = sys.argv[1], sys.argv[2]
    scene = assimp_py.import_file(src, FLAGS)
    print(f"Loaded {src}: {len(scene.meshes)} mesh(es)")

    for i, m in enumerate(scene.meshes, start=1):
        # assimp_py returns FLAT float arrays — reshape by vertex count
        v = np.asarray(m.vertices, np.float32).reshape(-1, 3)
        faces = np.asarray(m.indices, np.uint32).reshape(-1, 3)
        nv = len(v)
        n = np.asarray(m.normals, np.float32).reshape(nv, 3) if m.normals else None
        uv = np.asarray(m.texcoords[0], np.float32).reshape(nv, -1)[:, :2] if m.texcoords else None

        v = np.column_stack([v[:, 0], v[:, 2], -v[:, 1]])           # Z-up -> Y-up
        if n is not None:
            n = np.column_stack([n[:, 0], n[:, 2], -n[:, 1]])

        lo, hi = v.min(0), v.max(0)
        v -= np.array([(lo[0] + hi[0]) * 0.5, lo[1], (lo[2] + hi[2]) * 0.5], np.float32)

        mesh = trimesh.Trimesh(vertices=v, faces=faces, vertex_normals=n, process=False)
        if uv is not None:
            mesh.visual = trimesh.visual.TextureVisuals(uv=np.ascontiguousarray(uv))

        out = f"{stem}{i}.glb"
        trimesh.Scene({f"mesh_{i}": mesh}).export(out)
        size = (v.max(0) - v.min(0)).round(2)
        print(f"  [{i}] {m.name:<16} verts={nv:<6} faces={len(faces):<6} size(xyz)={size.tolist()} -> {out}")


if __name__ == "__main__":
    main()
