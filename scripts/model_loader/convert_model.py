#!/usr/bin/env python3
"""
Model Converter — converts 3D files to binary vertex/index buffers.

Supported formats (via trimesh):
  OBJ, GLTF/GLB, PLY, STL, 3MF, OFF, DAE, and more.

BLEND (Blender) is supported by shelling out to an installed Blender, which
exports the scene to a temporary GLB that is then read by trimesh — this is
exactly how Unity ingests .blend files under the hood. Set the BLENDER env var
if blender.exe is not auto-detected.

Output vertex struct (44 bytes per vertex):
  float x,  y,  z   — position
  float u,  v        — UV
  float nx, ny, nz   — normal
  float tx, ty, tz   — tangent

Usage:
  python convert_model.py model.obj
  python convert_model.py model.glb -o my_mesh
  python convert_model.py scene.glb --mesh 1 --flip-uv
  python convert_model.py new_car.obj --mesh 0 1

Install:
  pip install trimesh numpy
  pip install trimesh[easy]   # for DAE, FBX and other extras
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

try:
    import trimesh
except ImportError:
    sys.exit(
        "[ERROR] trimesh not found.\n"
        "Install: pip install trimesh numpy\n"
        "For extra format support: pip install trimesh[easy]"
    )

# ──────────────────────────────────────────────────────────────────────────────
# Constants
# ──────────────────────────────────────────────────────────────────────────────

VERTEX_STRUCT = "fffffffffff"          # 11 floats × 4 bytes = 44
VERTEX_SIZE   = struct.calcsize(VERTEX_STRUCT)
INDEX_STRUCT  = "I"
INDEX_SIZE    = struct.calcsize(INDEX_STRUCT)

# ──────────────────────────────────────────────────────────────────────────────
# Math helpers
# ──────────────────────────────────────────────────────────────────────────────

def _normalize_rows(arr: np.ndarray) -> np.ndarray:
    """Normalize each row of (N,3) array; zero-length rows become (0,0,1)."""
    lens = np.linalg.norm(arr, axis=1, keepdims=True)
    lens = np.where(lens < 1e-10, 1.0, lens)
    return arr / lens


def _fallback_tangent(n: np.ndarray) -> np.ndarray:
    """Arbitrary unit tangent perpendicular to n."""
    up = np.array([0.0, 1.0, 0.0], dtype=np.float32)
    if abs(float(np.dot(n, up))) > 0.99:
        up = np.array([1.0, 0.0, 0.0], dtype=np.float32)
    t = np.cross(up, n).astype(np.float32)
    return t / (np.linalg.norm(t) + 1e-12)


def compute_tangents(positions: np.ndarray,
                     uvs:       np.ndarray,
                     normals:   np.ndarray,
                     indices:   np.ndarray) -> np.ndarray:
    """
    MikkTSpace-style tangent accumulation.
    positions/uvs/normals: (N,3), (N,2), (N,3) — per vertex
    indices: (M,) flat triangle list
    Returns (N,3) tangents.
    """
    nv      = len(positions)
    tan_acc = np.zeros((nv, 3), dtype=np.float64)

    i0 = indices[0::3]
    i1 = indices[1::3]
    i2 = indices[2::3]

    e1  = positions[i1] - positions[i0]
    e2  = positions[i2] - positions[i0]
    du1 = uvs[i1, 0] - uvs[i0, 0]
    dv1 = uvs[i1, 1] - uvs[i0, 1]
    du2 = uvs[i2, 0] - uvs[i0, 0]
    dv2 = uvs[i2, 1] - uvs[i0, 1]

    det   = du1 * dv2 - du2 * dv1
    valid = np.abs(det) > 1e-10
    r     = np.where(valid, 1.0 / np.where(valid, det, 1.0), 0.0)[:, None]

    tangent = (e1 * dv2[:, None] - e2 * dv1[:, None]) * r
    tangent[~valid] = [1.0, 0.0, 0.0]   # degenerate faces → X axis

    np.add.at(tan_acc, i0, tangent)
    np.add.at(tan_acc, i1, tangent)
    np.add.at(tan_acc, i2, tangent)

    # Gram-Schmidt per vertex
    n    = normals.astype(np.float64)
    t    = tan_acc
    t    = t - (np.einsum("ij,ij->i", t, n))[:, None] * n
    lens = np.linalg.norm(t, axis=1, keepdims=True)
    bad  = (lens < 1e-8).flatten()
    t    = np.where(lens < 1e-8, 0.0, t / np.where(lens < 1e-8, 1.0, lens))

    result = t.astype(np.float32)
    for i in np.where(bad)[0]:
        result[i] = _fallback_tangent(normals[i])
    return result

# ──────────────────────────────────────────────────────────────────────────────
# Vertex deduplication
# ──────────────────────────────────────────────────────────────────────────────

def dedup_vertices(positions, uvs, normals, tangents, indices):
    """
    Merges identical vertices, rebuilds index buffer.
    Returns (positions, uvs, normals, tangents, indices).
    """
    data    = np.concatenate([positions, uvs, normals, tangents], axis=1)  # (N, 11)
    rounded = np.round(data, decimals=5)
    _, inv  = np.unique(rounded, axis=0, return_inverse=True)

    order    = np.unique(inv, return_index=True)[1]   # first occurrence of each unique
    new_pos  = positions[order]
    new_uv   = uvs[order]
    new_norm = normals[order]
    new_tan  = tangents[order]
    # Remap triangle index buffer: old vertex idx → deduplicated idx
    new_idx  = inv[indices].astype(np.uint32)
    return new_pos, new_uv, new_norm, new_tan, new_idx

# ──────────────────────────────────────────────────────────────────────────────
# Mesh extraction from trimesh objects
# ──────────────────────────────────────────────────────────────────────────────

def extract_meshes(loaded) -> list:
    """Flatten whatever trimesh returns into a list of Trimesh objects.

    Для Scene ОБЯЗАТЕЛЬНО запекаем node-трансформации из графа сцены в вершины:
    glTF/GLB (а значит и наш .blend-мост) кладёт неприменённый transform объекта
    (поворот/неравномерный масштаб/сдвиг) на УЗЕЛ, а geometry.values() отдаёт
    локальные координаты. Без этого сабмеши искажаются и разъезжаются («один
    повёрнут, другие сжаты»). OBJ такого бага не даёт — там transform уже в вершинах.
    """
    meshes = []
    if isinstance(loaded, trimesh.Trimesh):
        meshes = [loaded]
    elif isinstance(loaded, trimesh.Scene):
        # Геометрия может инстанцироваться несколькими узлами с разными матрицами —
        # собираем трансформы по имени геометрии, сохраняя порядок geometry (стабильный
        # material_index). nodes_geometry даёт по узлу на инстанс.
        geom_transforms = {}
        for node_name in loaded.graph.nodes_geometry:
            transform, geom_name = loaded.graph[node_name]
            geom_transforms.setdefault(geom_name, []).append(transform)
        for geom_name, geom in loaded.geometry.items():
            if not (isinstance(geom, trimesh.Trimesh) and len(geom.faces) > 0):
                continue
            for T in geom_transforms.get(geom_name, [np.eye(4)]):
                g = geom.copy()
                g.apply_transform(T)          # запекаем мировой transform в вершины+нормали
                meshes.append(g)
    elif isinstance(loaded, dict):
        for v in loaded.values():
            meshes.extend(extract_meshes(v))
    elif isinstance(loaded, (list, tuple)):
        for item in loaded:
            meshes.extend(extract_meshes(item))
    return meshes


def mesh_uvs(mesh: trimesh.Trimesh, nv: int) -> np.ndarray:
    """Return (nv,2) UV array; zeros if the mesh has none."""
    if hasattr(mesh, "visual"):
        # TextureVisuals stores uv directly
        if hasattr(mesh.visual, "uv") and mesh.visual.uv is not None:
            try:
                uv = np.asarray(mesh.visual.uv, dtype=np.float32)
                if uv.ndim == 2 and uv.shape == (nv, 2):
                    return uv
            except Exception:
                pass
        # ColorVisuals or other — try to_color fallback
        try:
            col = mesh.visual.to_color()
            if hasattr(col, "uv") and col.uv is not None:
                uv = np.asarray(col.uv, dtype=np.float32)
                if uv.ndim == 2 and uv.shape == (nv, 2):
                    return uv
        except Exception:
            pass
    return np.zeros((nv, 2), dtype=np.float32)

# ──────────────────────────────────────────────────────────────────────────────
# Per-format loading
# ──────────────────────────────────────────────────────────────────────────────

def _load_trimesh(full_path: str, process: bool):
    """Generic path: let trimesh pick the parser by extension."""
    return trimesh.load(full_path, process=process)


def _sanitize_dae_text(text: str):
    """
    Fix Blender-exported COLLADA tags whose ELEMENT NAME contains spaces, e.g.
    `<Follow Path_constraint>` (from a constraint named "Follow Path"). Spaces in
    tag names are invalid XML and crash the parser ("not well-formed").

    Эвристика разделения валидного и битого тега: настоящий тег с атрибутами всегда
    содержит '=' (attr="value"), а валидный простой тег не имеет пробелов внутри.
    Поэтому правим только теги, где ЕСТЬ пробел и НЕТ '=', схлопывая пробелы в имени
    в '_'. Матчатся и открывающий, и закрывающий тег → баланс сохраняется.

    Returns (fixed_text, num_fixes).
    """
    tag_re = re.compile(r'<(/?)([^<>!?][^<>]*?)(/?)>')
    count = [0]

    def repl(m):
        slash, body, selfclose = m.group(1), m.group(2), m.group(3)
        if '=' in body or not re.search(r'\s', body):
            return m.group(0)                      # валидный тег — не трогаем
        fixed = re.sub(r'\s+', '_', body.strip())  # "Follow Path_constraint" → "Follow_Path_constraint"
        count[0] += 1
        return f'<{slash}{fixed}{selfclose}>'

    return tag_re.sub(repl, text), count[0]


def load_dae(full_path: str, process: bool):
    """COLLADA: пред-скан на битые токены (баг экспорта Blender) и их починка
    до передачи файла в trimesh/pycollada."""
    with open(full_path, 'r', encoding='utf-8', errors='replace') as f:
        raw = f.read()

    fixed, n = _sanitize_dae_text(raw)
    if n == 0:
        return _load_trimesh(full_path, process)   # чинить нечего — обычный путь

    print(f"  [dae] fixed {n} malformed tag(s) with spaces in names (Blender export)")
    fd, tmp = tempfile.mkstemp(suffix='.dae')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            out.write(fixed)
        return trimesh.load(tmp, process=process)
    finally:
        os.remove(tmp)


# ── Blender (.blend) ──────────────────────────────────────────────────────────
# .blend — проприетарный формат авторинга Blender; распарсить его без самого
# Blender нельзя (структуры завязаны на версию). Поэтому дёргаем установленный
# Blender в фоне — ровно как Unity под капотом — и читаем его экспорт в .glb.

# Кандидаты на blender.exe: env-var → PATH → типовые пути установки.
_BLENDER_CANDIDATES = [
    r"C:\NotSys\Blender\blender.exe",
    r"C:\Program Files\Blender Foundation\*\blender.exe",
    r"C:\Program Files (x86)\Blender Foundation\*\blender.exe",
]

# Скрипт-экспортёр, исполняемый ВНУТРИ Blender (его встроенный Python).
# Путь вывода берёт из argv (после '--'); пишет GLB всей сцены.
_BLENDER_EXPORT_PY = (
    "import bpy, sys\n"
    "try:\n"
    "    import addon_utils\n"
    "    addon_utils.enable('io_scene_gltf2', default_set=False, persistent=True)\n"
    "except Exception:\n"
    "    pass\n"
    "out = sys.argv[-1]\n"
    # export_apply=True — ЗАПЕКАЕТ модификаторы (Mirror/Subsurf/Solidify/Array...).
    # Без него зеркальная половина модели и пр. геометрия модификаторов теряется.
    "bpy.ops.export_scene.gltf(filepath=out, export_format='GLB',\n"
    "                          use_selection=False, export_apply=True)\n"
)


def _find_blender() -> str:
    """Найти blender.exe. Переопределяется переменной окружения BLENDER / BLENDER_PATH."""
    env = os.environ.get("BLENDER") or os.environ.get("BLENDER_PATH")
    if env and os.path.isfile(env):
        return env
    found = shutil.which("blender")
    if found:
        return found
    for pat in _BLENDER_CANDIDATES:
        matches = sorted(glob.glob(pat))
        if matches:
            return matches[-1]          # самая свежая версия по сортировке имён
    sys.exit(
        "[ERROR] Blender executable not found.\n"
        "Install Blender or point the BLENDER env var at blender.exe:\n"
        "  PowerShell:  $env:BLENDER = 'C:\\path\\to\\blender.exe'"
    )


def load_blend(full_path: str, process: bool):
    """Экспорт .blend → временный .glb через фоновый Blender, затем чтение trimesh."""
    blender   = _find_blender()
    abs_blend = os.path.abspath(full_path)

    fd_glb, tmp_glb = tempfile.mkstemp(suffix=".glb")
    os.close(fd_glb)
    fd_py, tmp_py = tempfile.mkstemp(suffix=".py")
    with os.fdopen(fd_py, "w", encoding="utf-8") as f:
        f.write(_BLENDER_EXPORT_PY)

    try:
        print(f"  [blend] exporting via Blender ({blender}) -> temp .glb")
        proc = subprocess.run(
            [blender, "--background", "--factory-startup",
             abs_blend, "--python", tmp_py, "--", tmp_glb],
            capture_output=True, text=True,
        )
        if proc.returncode != 0 or os.path.getsize(tmp_glb) == 0:
            raise RuntimeError(
                f"Blender export failed (exit {proc.returncode}).\n"
                f"--- stdout (tail) ---\n{proc.stdout[-2000:]}\n"
                f"--- stderr (tail) ---\n{proc.stderr[-2000:]}"
            )
        return trimesh.load(tmp_glb, process=process)
    finally:
        for p in (tmp_glb, tmp_py):
            try:
                os.remove(p)
            except OSError:
                pass


# Остальные форматы — ничего особенного, просто делегируют в trimesh.
def load_obj(full_path, process):  return _load_trimesh(full_path, process)
def load_glb(full_path, process):  return _load_trimesh(full_path, process)
def load_gltf(full_path, process): return _load_trimesh(full_path, process)
def load_ply(full_path, process):  return _load_trimesh(full_path, process)
def load_stl(full_path, process):  return _load_trimesh(full_path, process)
def load_off(full_path, process):  return _load_trimesh(full_path, process)
def load_3mf(full_path, process):  return _load_trimesh(full_path, process)


LOADERS = {
    '.dae':   load_dae,
    '.blend': load_blend,
    '.obj':  load_obj,
    '.glb':  load_glb,
    '.gltf': load_gltf,
    '.ply':  load_ply,
    '.stl':  load_stl,
    '.off':  load_off,
    '.3mf':  load_3mf,
}


def load_model(full_path: str, process: bool):
    """Диспетчер по расширению; неизвестное → общий trimesh."""
    ext = os.path.splitext(full_path)[1].lower()
    return LOADERS.get(ext, _load_trimesh)(full_path, process)


# ──────────────────────────────────────────────────────────────────────────────
# Core conversion
# ──────────────────────────────────────────────────────────────────────────────

def convert(input_path: str,
            output_stem: str,
            mesh_indices: list = None,   # None = all meshes
            flip_uv: bool = False,
            no_dedup: bool = False,
            fix_normals: bool = False,
            verbose: bool = False):

    print(f"Loading: {input_path}")
    full_path = f"raw/{input_path}"
    try:
        loaded = load_model(full_path, fix_normals)
        if fix_normals:
            # Per-mesh: fix winding order so all normals point outward
            for m in extract_meshes(loaded):
                trimesh.repair.fix_normals(m)
                trimesh.repair.fix_winding(m)
    except Exception as e:
        sys.exit(f"[ERROR] Failed to load file: {e}")

    all_meshes = extract_meshes(loaded)
    if not all_meshes:
        sys.exit("[ERROR] No triangle meshes found in file.")

    print(f"  Meshes found: {len(all_meshes)}")
    for i, m in enumerate(all_meshes):
        print(f"    [{i}] verts={len(m.vertices)}  faces={len(m.faces)}")

    # ── Select meshes ─────────────────────────────────────────────────────────
    if mesh_indices is not None:
        # Validate all requested indices up front
        invalid = [idx for idx in mesh_indices if idx >= len(all_meshes) or idx < 0]
        if invalid:
            sys.exit(
                f"[ERROR] Mesh index(es) out of range: {invalid} "
                f"(file has {len(all_meshes)} meshes, valid range: 0–{len(all_meshes)-1})."
            )
        # Preserve requested order, deduplicate while keeping order
        seen = set()
        selected_indices = [i for i in mesh_indices if not (i in seen or seen.add(i))]
        selected = [all_meshes[i] for i in selected_indices]
        print(f"  Selected mesh(es): {selected_indices}")
    else:
        selected = all_meshes

    # ── Accumulate ────────────────────────────────────────────────────────────
    acc_pos, acc_uv, acc_norm, acc_tan, acc_idx = [], [], [], [], []
    submesh_entries = []  # (v_offset, i_offset, v_count, i_count, material_index)
    vert_offset = 0
    idx_offset = 0

    for mat_idx, mesh in enumerate(selected):
        if not isinstance(mesh, trimesh.Trimesh):
            continue
        nv = len(mesh.vertices)
        nf = len(mesh.faces)
        if nv == 0 or nf == 0:
            continue

        positions = np.array(mesh.vertices, dtype=np.float32)
        indices = np.array(mesh.faces, dtype=np.uint32).flatten()
        normals = _normalize_rows(np.array(mesh.vertex_normals, dtype=np.float32))

        uvs = mesh_uvs(mesh, nv)
        if flip_uv:
            uvs = uvs.copy()
            uvs[:, 1] = 1.0 - uvs[:, 1]

        if verbose and np.all(uvs == 0):
            print(f"  [warn] Mesh [{mat_idx}] has no UVs — tangents may be arbitrary")

        tangents = compute_tangents(positions, uvs, normals, indices)

        acc_pos.append(positions)
        acc_uv.append(uvs)
        acc_norm.append(normals)
        acc_tan.append(tangents)
        acc_idx.append(indices)

        submesh_entries.append((
            vert_offset,  # vertex_offset (локальный)
            idx_offset,  # index_offset  (локальный)
            nv,  # vertex_count
            len(indices),  # index_count
            mat_idx  # material_index
        ))

        vert_offset += nv
        idx_offset += len(indices)

    if not acc_pos:
        sys.exit("[ERROR] No exportable meshes after filtering.")

    positions = np.concatenate(acc_pos)
    uvs = np.concatenate(acc_uv)
    normals = np.concatenate(acc_norm)
    tangents = np.concatenate(acc_tan)
    indices = np.concatenate(acc_idx).astype(np.uint32)

    # ── Deduplication ─────────────────────────────────────────────────────────
    # (без изменений)

    # ── Write binary files ────────────────────────────────────────────────────
    SUBMESH_ENTRY_STRUCT = "IIIII"  # 5 × uint32

    out_verts = output_stem + ".bin"
    out_indices = output_stem + "_i.bin"

    with open(out_verts, "wb") as fv:
        # Заголовок: количество сабмешей
        fv.write(struct.pack("I", len(submesh_entries)))
        # Таблица сабмешей
        for entry in submesh_entries:
            fv.write(struct.pack(SUBMESH_ENTRY_STRUCT, *entry))
        # Вершины
        for i in range(len(positions)):
            fv.write(struct.pack(
                VERTEX_STRUCT,
                positions[i][0], positions[i][1], positions[i][2],
                uvs[i][0], uvs[i][1],
                normals[i][0], normals[i][1], normals[i][2],
                tangents[i][0], tangents[i][1], tangents[i][2],
            ))

    with open(out_indices, "wb") as fi:
        fi.write(indices.astype(np.uint32).tobytes())

    vb_kb = os.path.getsize(out_verts) / 1024
    ib_kb = os.path.getsize(out_indices) / 1024
    print(f"\n  Submeshes: {len(submesh_entries)}")
    for i, e in enumerate(submesh_entries):
        print(f"    [{i}] verts={e[2]}  indices={e[3]}  material_index={e[4]}")
    print(f"  Vertices : {len(positions):>8}   →  {out_verts}  ({vb_kb:.1f} KB)")
    print(f"  Indices  : {len(indices):>8}   →  {out_indices}  ({ib_kb:.1f} KB)")
    print(f"\n✓ Done.")

# ──────────────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Convert 3D models to binary vertex+index buffers.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python convert_model.py hero.obj
  python convert_model.py building.glb -o building
  python convert_model.py scene.glb   --mesh 2 --flip-uv
  python convert_model.py new_car.obj --mesh 0 1
  python convert_model.py tank.stl    --no-dedup -v
        """
    )
    parser.add_argument("input",
                        help="Input model file (OBJ, GLTF/GLB, PLY, STL, DAE, BLEND, 3MF, OFF, ...)")
    parser.add_argument("-o", "--output",
                        default=None,
                        help="Output file stem (default: same name as input, no extension)")
    parser.add_argument("--mesh",
                        type=int,
                        nargs="+",
                        default=None,
                        metavar="INDEX",
                        help="Export only mesh(es) at INDEX(es), merged into one buffer "
                             "(default: merge all meshes). Example: --mesh 0 1")
    parser.add_argument("--flip-uv",
                        action="store_true",
                        help="Flip UV Y axis (DirectX ↔ OpenGL convention)")
    parser.add_argument("--no-dedup",
                        action="store_true",
                        help="Skip vertex deduplication")
    parser.add_argument("--fix-normals",
                        action="store_true",
                        help="Fix inverted normals and winding order (use for broken models)")
    parser.add_argument("-v", "--verbose",
                        action="store_true",
                        help="Print extra diagnostics")
    args = parser.parse_args()

    if not os.path.isfile(f"raw/{args.input}"):
        sys.exit(f"[ERROR] File not found: {args.input}")

    output_stem = args.output or str(Path(args.input).with_suffix(""))

    convert(
        input_path   = args.input,
        output_stem  = output_stem,
        mesh_indices = args.mesh,      # None or list of ints
        flip_uv      = args.flip_uv,
        no_dedup     = args.no_dedup,
        fix_normals  = args.fix_normals,
        verbose      = args.verbose,
    )


if __name__ == "__main__":
    main()