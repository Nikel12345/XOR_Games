#!/usr/bin/env python3
"""Split an assimp-readable scene into one GLB per NODE (object), for convert_model.py.

Unlike blend_split.py (one object = one mesh, node transforms are pure layout and
get dropped), here an object is a NODE that owns several meshes — one per material
— and its node transform carries real placement and rotation, so it is BAKED.

Two rules that matter downstream:
  * Submeshes are ordered by assimp material index, so material_index in the .bin
    is stable across all objects: 0 = first material of the file, 1 = second, …
    The entity's `materials` array (BaseComponents.h:196) is indexed by it, so one
    и тот же список материалов подходит всем выходным моделям.
  * Each object is recentered on its own bbox center — the source has every object
    sitting at its place in the shared scene, and without this every model would
    carry that offset inside its vertices.

Usage: python node_split.py raw/scene.fbx raw/out_prefix [--scale 0.01]
       -> raw/out_prefix01.glb …  (по узлу, порядок = порядок в файле)
"""

import sys

import numpy as np
import trimesh

import assimp_py
from assimp_to_glb import drop_degenerate, repair_normals

FLAGS = (assimp_py.Process_Triangulate
         | assimp_py.Process_JoinIdenticalVertices
         | assimp_py.Process_GenSmoothNormals)


def collect(node, parent=np.eye(4), out=None):
    """(имя узла, мировая матрица, индексы мешей) для каждого узла с геометрией."""
    out = [] if out is None else out
    M = parent @ np.asarray(node.transformation, np.float64).reshape(4, 4)
    if node.mesh_indices:
        out.append((node.name, M, list(node.mesh_indices)))
    for c in node.children:
        collect(c, M, out)
    return out


def main():
    src, stem = sys.argv[1], sys.argv[2]
    scale = float(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1.0

    scene = assimp_py.import_file(src, FLAGS)
    nodes = collect(scene.root_node)
    print(f"Loaded {src}: {len(scene.meshes)} mesh(es), {len(scene.materials)} material(s), "
          f"{len(nodes)} object(s)")

    for name, M, mesh_ids in sorted(nodes, key=lambda n: n[0]):
        parts = []
        for mid in sorted(mesh_ids, key=lambda i: scene.meshes[i].material_index):
            m = scene.meshes[mid]
            v = np.asarray(m.vertices, np.float32).reshape(-1, 3)
            f = np.asarray(m.indices, np.uint32).reshape(-1, 3)
            nv = len(v)
            n = np.asarray(m.normals, np.float32).reshape(nv, 3) if m.normals else None
            uv = np.asarray(m.texcoords[0], np.float32).reshape(nv, -1)[:, :2] if m.texcoords else None

            v = (v @ M[:3, :3].T + M[:3, 3]).astype(np.float32) * scale   # запекаем узел
            if n is not None:
                n = (n @ M[:3, :3].T).astype(np.float32)                  # без масштаба: направление
            v, f, n, uv = drop_degenerate(v, f, n, uv)
            if n is not None:
                n = repair_normals(v, f, n)
            parts.append((m.material_index, v, f, n, uv))

        lo = np.min([p[1].min(0) for p in parts], axis=0)
        hi = np.max([p[1].max(0) for p in parts], axis=0)
        pivot = (lo + hi) * 0.5

        out = trimesh.Scene()
        for order, (mat, v, f, n, uv) in enumerate(parts):
            mesh = trimesh.Trimesh(vertices=v - pivot, faces=f, vertex_normals=n, process=False)
            if uv is not None:
                mesh.visual = trimesh.visual.TextureVisuals(uv=np.ascontiguousarray(uv))
            # geom_name задаёт порядок геометрий в GLB, а он же станет material_index в .bin
            out.add_geometry(mesh, geom_name=f"{order}_mat{mat}")

        dst = f"{stem}{name[-2:]}.glb"
        out.export(dst)
        size = (hi - lo) * scale
        print(f"  {name:<10} submeshes={[p[0] for p in parts]} "
              f"verts={sum(len(p[1]) for p in parts):<5} size={size.round(2).tolist()} -> {dst}")


if __name__ == "__main__":
    main()
