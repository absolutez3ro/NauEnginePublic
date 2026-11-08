#!/usr/bin/env python
# Copyright 2024 N-GINN LLC. All rights reserved.
# Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.
"""Converts the Intel "Jungle Ruins" USD scene into what SceneBaseSample loads (key 2): one glTF with its binary buffer,
Nau material instances and downscaled textures.

The source is made for offline rendering: 8.7 million instances and 1.1 trillion triangles. Only the surroundings of the
pyramid are taken, instances are thinned out and tree foliage is thinned (leaves are separate pieces of geometry: the kept
ones grow to preserve the covered area), so the frame gets a few million triangles. The plants get simplified levels of
detail (fewer, larger leaves; clustered trunks) that Nau switches by the distance from the camera.

    pip install --target <folder> usd-core pillow
    PYTHONPATH=<folder> python convert_jungle_ruins.py --usd <JungleRuins>/USD/JungleRuins_Karma.usda --density 1
    (the textures folder is found next to USD, the result goes to ../content/scenes/jungle_ruins)

--density scales the number of instances and can be changed without rebuilding the sample; --detail scales the kept foliage;
--texture-size limits the textures (the terrain gets twice as much, the normal maps half).

Coordinates: USD is Z-up in centimeters, Nau is Y-up in meters, the pyramid gets into the origin.
"""

import argparse
import hashlib
import json
import math
import os
import re
import struct
import time

import numpy as np
from PIL import Image
from pxr import Gf, Usd, UsdGeom, UsdShade

Image.MAX_IMAGE_PIXELS = None

FOCUS_CM = np.array([54350.0, 80960.0, 0.0])

# How many instances of a family are taken at --density 1 and how far from the pyramid (meters). boost enlarges the
# instances of the small plants: millions of them cover the ground in the source, a few thousand have to do it here.
# The levels of detail made 2.5 times more plants affordable than in the first version (--density 0.4 gives that one).
FAMILIES = [
    # pattern of the prototype name, instances, radius, boost
    (r"^RiverForest_", 300, 120.0, 1.0),
    (r"^QueenForest_", 150, 120.0, 1.0),
    (r"^RiverSapling_", 500, 90.0, 1.0),
    (r"^RiverSeedling_", 750, 70.0, 1.3),
    (r"^Shrub_\d", 375, 80.0, 1.2),
    (r"^Grass_A_", 1000, 70.0, 2.0),
    (r"^Grass_B_", 750, 70.0, 2.0),
    (r"^Moss_", 1250, 40.0, 6.0),
]

# Nothing tall stands closer to the pyramid than this (meters): its half diagonal, and the view of it stays open.
TALL_FAMILIES = re.compile(r"^(RiverForest|QueenForest|RiverSapling)_")
TALL_MIN_DISTANCE = 32.0

# The terrain textures of the source are nearly black (it lies under the forest): the shadows have to be visible on it.
TERRAIN_GAIN = 3.0

# Plain meshes of the source: the terrain tiles of the central region and the pyramid. The creek and the river are left
# out, they need water shading; the banyan alone has 5 million triangles and stands 350 m away.
PLAIN_MESHES = r"^/root/(M_\dx\d_\d\d|Pyramid_v1a_DT)/"

# Kept share of the geometry of a part, by its material name. Everything not listed is kept whole.
DETAIL = [
    (r"Berries", 0.03),
    (r"FruitBranches", 0.15),
    (r"(Dead|Dried|Old)", 0.2),
    (r"QueenPalm_Leaves", 0.2),
    (r"RiverFamily_Leaves", 0.25),
    (r"RiverFamily_Branches", 0.2),
    (r"Shrub", 0.5),
    (r"QueenPalm_Branches", 0.5),
]

# Parts that are made of leaves: the kept pieces grow, so that the crown does not get transparent.
LEAF_PART = re.compile(r"(Leaves|Leaf|Shrub|Grass|Moss)")
MAX_LEAF_GROWTH = 2.5

# Levels of detail of the plants (the extras "nau_lod_meshes" / "nau_lod_distances" of a glTF mesh, read by Nau):
# share of the pieces of level 0 that a level keeps (leaves, twigs, blades), cells of the vertex clustering along the
# diameter of the prototype (trunks and other solid parts), switch distance in radii of the prototype.
LODS = [
    (0.35, 40, 1.5),
    (0.10, 14, 4.0),
]
MAX_LOD_LEAF_GROWTH = 6.0  # the far crowns keep most of their covered area
LOD_MIN_TRIANGLES = 600  # smaller parts are drawn whole at any distance

DEFAULT_NORMAL = "file:/content/textures/default_normal_8x8.png"
MASTER_OPAQUE = "file:/res/materials/embedded/standard_opaque.nmat_json"
MASTER_MASKED = "file:/res/materials/embedded/standard_masked.nmat_json"
CONTENT_ROOT = "file:/content/scenes/jungle_ruins/"


def to_nau_points(points_cm):
    """USD world centimeters (Z-up) -> Nau meters (Y-up), the pyramid in the origin."""
    p = (points_cm - FOCUS_CM) * 0.01
    return np.stack([p[:, 0], p[:, 2], -p[:, 1]], axis=1)


def to_nau_directions(directions):
    return np.stack([directions[:, 0], directions[:, 2], -directions[:, 1]], axis=1)


# Rotation that turns Z-up into Y-up, applied on top of the rotation of an instance (Nau quaternion x, y, z, w).
Z_UP_TO_Y_UP = Gf.Quatd(math.sqrt(0.5), Gf.Vec3d(-math.sqrt(0.5), 0.0, 0.0))


class Mesh:
    """Triangles of a USD mesh: per-corner point index, normal and texture coordinates."""

    def __init__(self, prim):
        usd_mesh = UsdGeom.Mesh(prim)
        self.points = np.array(usd_mesh.GetPointsAttr().Get(), dtype=np.float32)
        counts = np.array(usd_mesh.GetFaceVertexCountsAttr().Get(), dtype=np.int64)
        indices = np.array(usd_mesh.GetFaceVertexIndicesAttr().Get(), dtype=np.int64)

        # Fans: corner offsets of the triangles of every face.
        tris_per_face = counts - 2
        face_starts = np.cumsum(counts) - counts
        self.tri_face = np.repeat(np.arange(len(counts)), tris_per_face)
        first_tri = np.cumsum(tris_per_face) - tris_per_face
        local = np.arange(len(self.tri_face)) - np.repeat(first_tri, tris_per_face)
        start = face_starts[self.tri_face]
        self.corners = np.stack([start, start + local + 1, start + local + 2], axis=1)
        self.point_index = indices[self.corners]

        normals = usd_mesh.GetNormalsAttr().Get()
        normals = np.array(normals, dtype=np.float32) if normals else None
        interpolation = usd_mesh.GetNormalsInterpolation()
        if normals is None:
            self.normals = self._flat_normals()
        elif interpolation == UsdGeom.Tokens.faceVarying:
            self.normals = normals[self.corners]
        elif interpolation == UsdGeom.Tokens.uniform:
            self.normals = normals[self.tri_face][:, None, :].repeat(3, axis=1)
        else:
            self.normals = normals[self.point_index]

        primvars = UsdGeom.PrimvarsAPI(prim)
        st = primvars.GetPrimvar("st")
        if st and st.ComputeFlattened() is not None:
            values = np.array(st.ComputeFlattened(), dtype=np.float32)
            if st.GetInterpolation() == UsdGeom.Tokens.faceVarying:
                self.uvs = values[self.corners]
            else:
                self.uvs = values[self.point_index]
        else:
            self.uvs = np.zeros(self.corners.shape + (2,), dtype=np.float32)

        # Parts by material: GeomSubsets, or the whole mesh. The children of a class are "abstract", GetAllChildren sees them.
        self.parts = []
        taken = np.zeros(len(counts), dtype=bool)
        for child in prim.GetAllChildren():
            if child.GetTypeName() != "GeomSubset":
                continue
            faces = np.array(UsdGeom.Subset(child).GetIndicesAttr().Get(), dtype=np.int64)
            material = UsdShade.MaterialBindingAPI(child).ComputeBoundMaterial()[0]
            if material and len(faces) != 0:
                self.parts.append((material, faces))
                taken[faces] = True
        if not self.parts or not taken.all():
            material = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
            if material:
                self.parts.append((material, np.nonzero(~taken)[0]))

    def _flat_normals(self):
        tri = self.points[self.point_index]
        n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        return n[:, None, :].repeat(3, axis=1)

    def part_triangles(self, faces):
        mask = np.zeros(self.tri_face.max() + 1, dtype=bool)
        mask[faces] = True
        return np.nonzero(mask[self.tri_face])[0]


def triangle_components(point_index):
    """Connected pieces of the triangles (a leaf, a twig): label per triangle."""
    points, inverse = np.unique(point_index, return_inverse=True)
    inverse = inverse.reshape(point_index.shape)
    parent = list(range(len(points)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for a, b, c in inverse.tolist():
        ra, rb, rc = find(a), find(b), find(c)
        if ra != rb:
            parent[rb] = ra
        if ra != rc and find(rc) != ra:
            parent[rc] = ra
    return np.array([find(i) for i in inverse[:, 0].tolist()])


def thin_out(mesh, triangles, keeps, grow, rng, max_growths):
    """Keeps a share of the pieces of a part, for every share in keeps (the levels of detail). One random draw per piece:
    a level with a smaller share keeps a subset of the pieces of the previous one. Returns [(corner positions, the kept
    triangles)] per share; all the pieces where a part has too few of them to thin out."""
    positions = mesh.points[mesh.point_index[triangles]]
    if all(keep >= 1.0 for keep in keeps):
        return [(positions, triangles)] * len(keeps)

    labels = triangle_components(mesh.point_index[triangles])
    unique, inverse = np.unique(labels, return_inverse=True)
    if len(unique) < 8:
        return [(positions, triangles)] * len(keeps)

    draws = rng.random(len(unique))
    levels = []
    for keep, max_growth in zip(keeps, max_growths):
        if keep >= 1.0:
            levels.append((positions, triangles))
            continue
        kept_pieces = draws < keep
        if not kept_pieces.any():
            kept_pieces[np.argmin(draws)] = True
        selected = kept_pieces[inverse]
        kept_positions = positions[selected]

        if grow:
            # Every kept piece grows around its center.
            growth = min(math.sqrt(1.0 / keep), max_growth)
            piece = inverse[selected]
            centers = np.zeros((len(unique), 3))
            weights = np.zeros(len(unique))
            np.add.at(centers, piece, kept_positions.mean(axis=1))
            np.add.at(weights, piece, 1.0)
            centers = (centers / np.maximum(weights, 1.0)[:, None])[piece][:, None, :]
            kept_positions = (centers + (kept_positions - centers) * growth).astype(np.float32)
        levels.append((kept_positions, triangles[selected]))
    return levels


def cluster(vertices, normals, uvs, indices, cell):
    """Vertex clustering on a grid: the simplified level of a solid part (a trunk). The texture coordinates of a cluster
    are those of one of its vertices: seams stretch, which a distant level can afford. A cluster whose normals cancel out
    (opposite sides of a thin part) takes the normal of one vertex: a zero normal is NaN in the shading. The result has
    cracks and flat sheets, so it is drawn two-sided (see Scene.material)."""
    keys = np.floor(vertices / cell).astype(np.int64)
    _, cluster_of, counts = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
    cluster_of = cluster_of.reshape(-1)
    clusters = len(counts)

    positions = np.zeros((clusters, 3))
    np.add.at(positions, cluster_of, vertices)
    positions /= counts[:, None]
    first = np.zeros(clusters, dtype=np.int64)
    first[cluster_of[::-1]] = np.arange(len(cluster_of))[::-1]
    cluster_uvs = uvs[first]
    summed_normals = np.zeros((clusters, 3))
    np.add.at(summed_normals, cluster_of, normals)
    lengths = np.linalg.norm(summed_normals, axis=1, keepdims=True)
    degenerate = lengths[:, 0] < 1e-3
    summed_normals[degenerate] = normals[first[degenerate]]
    lengths[degenerate] = np.linalg.norm(summed_normals[degenerate], axis=1, keepdims=True)
    summed_normals /= np.maximum(lengths, 1e-8)

    triangles = cluster_of[indices.reshape(-1, 3)]
    triangles = triangles[(triangles[:, 0] != triangles[:, 1]) & (triangles[:, 1] != triangles[:, 2]) & (triangles[:, 0] != triangles[:, 2])]
    if len(triangles) == 0:
        return None
    _, unique_rows = np.unique(np.sort(triangles, axis=1), axis=0, return_index=True)
    triangles = triangles[np.sort(unique_rows)]

    used, remap = np.unique(triangles.reshape(-1), return_inverse=True)
    return (positions[used].astype(np.float32), summed_normals[used].astype(np.float32), cluster_uvs[used].astype(np.float32),
            remap.reshape(-1).astype(np.uint32))


def weld(positions, normals, uvs):
    """Per-corner attributes -> vertices and indices."""
    stacked = np.concatenate([positions.reshape(-1, 3), normals.reshape(-1, 3), uvs.reshape(-1, 2)], axis=1).astype(np.float32)
    view = stacked.view(np.dtype((np.void, stacked.dtype.itemsize * stacked.shape[1])))
    _, first, inverse = np.unique(view, return_index=True, return_inverse=True)
    vertices = stacked[first]
    return vertices[:, :3], vertices[:, 3:6], vertices[:, 6:8], inverse.reshape(-1).astype(np.uint32)


class Textures:
    def __init__(self, out_dir, base_size):
        self.dir = os.path.join(out_dir, "textures")
        os.makedirs(self.dir, exist_ok=True)
        self.base_size = base_size
        self.cache = {}

    @staticmethod
    def _load(path):
        if not path or not os.path.exists(path) or path.lower().endswith(".exr"):
            return None  # Pillow does not read EXR: the constant of the input is used instead
        return Image.open(path)

    def _save(self, name, image, quality=88):
        path = os.path.join(self.dir, name)
        if not os.path.exists(path):
            if name.endswith(".jpg"):
                image.convert("RGB").save(path, quality=quality)
            else:
                image.save(path)
        return CONTENT_ROOT + "textures/" + name

    def _sized(self, image, size):
        scale = size / max(image.size)
        if scale < 1.0:
            image = image.resize((max(4, int(image.width * scale)), max(4, int(image.height * scale))), Image.LANCZOS)
        return image

    def albedo(self, source, opacity, size, gain=1.0):
        key = ("albedo", str(source), str(opacity), size, gain)
        if key in self.cache:
            return self.cache[key]

        stem = stem_of(source["path"] if isinstance(source, dict) else "solid_%s" % digest(source))
        if gain != 1.0:
            stem += "_gain%g" % gain
        if isinstance(source, dict):
            image = self._load(source["path"])
        else:
            image = None
        if image is None:
            color = source if not isinstance(source, dict) else (0.3, 0.3, 0.3)
            rgb = tuple(int(255 * max(0.0, min(1.0, c)) ** (1 / 2.2)) for c in color)
            image = Image.new("RGB", (4, 4), rgb)
        image = self._sized(image, size)
        if gain != 1.0:
            image = image.convert("RGB").point(lambda value: min(255, int(value * gain)))

        if opacity is None:
            result = self._save("%s_%d.jpg" % (stem, size), image)
        else:
            mask = None
            if opacity["channel"] == "a" and opacity["path"] == (source.get("path") if isinstance(source, dict) else None):
                mask = image.convert("RGBA").getchannel("A")
            else:
                loaded = self._load(opacity["path"])
                if loaded is not None:
                    channel = {"r": 0, "g": 1, "b": 2}.get(opacity["channel"], 0)
                    bands = loaded.convert("RGBA").split() if opacity["channel"] == "a" else loaded.convert("RGB").split()
                    mask = (bands[3] if opacity["channel"] == "a" else bands[channel]).resize(image.size, Image.LANCZOS)
            rgba = image.convert("RGB")
            rgba.putalpha(mask if mask is not None else Image.new("L", image.size, 255))
            result = self._save("%s_%s_%d.png" % (stem, digest(opacity["path"])[:6], size), rgba)
        self.cache[key] = result
        return result

    def normal(self, source, size):
        if not isinstance(source, dict):
            return DEFAULT_NORMAL
        key = ("normal", source["path"], size)
        if key in self.cache:
            return self.cache[key]
        image = self._load(source["path"])
        result = DEFAULT_NORMAL if image is None else self._save("%s_%d.jpg" % (stem_of(source["path"]), size), self._sized(image.convert("RGB"), size))
        self.cache[key] = result
        return result

    def orm(self, roughness, metallic, size):
        """Nau's metalRoughnessAoTex: r = ambient occlusion, g = roughness, b = metalness."""
        key = ("orm", str(roughness), str(metallic), size)
        if key in self.cache:
            return self.cache[key]

        def channel(source, fallback):
            if isinstance(source, dict):
                image = self._load(source["path"])
                if image is not None:
                    image = self._sized(image, size)
                    bands = image.convert("RGB").split()
                    return bands[{"r": 0, "g": 1, "b": 2}.get(source["channel"], 0)]
                return fallback
            return float(source) if source is not None else fallback

        r = channel(roughness, 0.8)
        m = channel(metallic, 0.0)
        shape = None
        for value in (r, m):
            if isinstance(value, Image.Image):
                shape = value.size if shape is None else shape
        shape = shape or (4, 4)

        def band(value):
            if isinstance(value, Image.Image):
                return value.resize(shape)
            return Image.new("L", shape, int(255 * max(0.0, min(1.0, value))))

        image = Image.merge("RGB", (Image.new("L", shape, 255), band(r), band(m)))
        result = self._save("orm_%s.jpg" % digest(key), image)
        self.cache[key] = result
        return result


def digest(value):
    return hashlib.md5(str(value).encode("utf-8")).hexdigest()[:10]


def stem_of(path):
    return re.sub(r"[^A-Za-z0-9_]", "_", os.path.splitext(os.path.basename(path))[0])


def material_inputs(material, usd_dir):
    """Inputs of the UsdPreviewSurface: constant, or {"path": file, "channel": r/g/b/a/rgb}."""
    surface = None
    for prim in Usd.PrimRange(material.GetPrim(), Usd.PrimAllPrimsPredicate):
        if prim.GetTypeName() == "Shader" and UsdShade.Shader(prim).GetIdAttr().Get() == "UsdPreviewSurface":
            surface = UsdShade.Shader(prim)
            break

    inputs = {}
    if surface is None:
        return inputs
    for shader_input in surface.GetInputs():
        name = shader_input.GetBaseName()
        sources = shader_input.GetConnectedSources()
        if sources and sources[0]:
            source = UsdShade.Shader(sources[0][0].source.GetPrim())
            file_input = source.GetInput("file")
            asset = file_input.Get() if file_input else None
            if asset is not None:
                # The texture paths are relative to the layer that defines the shader: find the file by the anchored path,
                # or by its place below the shared textures folder.
                path = asset.resolvedPath
                if not path:
                    relative = asset.path.replace("\\", "/")
                    marker = relative.rfind("textures/")
                    path = os.path.normpath(os.path.join(usd_dir, "..", relative[marker:])) if marker >= 0 else ""
                inputs[name] = {"path": path, "channel": sources[0][0].sourceName}
                continue
        inputs[name] = shader_input.Get()
    return inputs


class Scene:
    def __init__(self, out_dir, base_texture_size):
        self.out_dir = out_dir
        self.textures = Textures(out_dir, base_texture_size)
        self.base_texture_size = base_texture_size
        self.materials = {}
        self.material_dir = os.path.join(out_dir, "materials")
        os.makedirs(self.material_dir, exist_ok=True)
        # The material files of an earlier run are not used any more (their names carry a digest of the settings).
        for stale in os.listdir(self.material_dir):
            if stale.endswith(".nmat_inst_json"):
                os.remove(os.path.join(self.material_dir, stale))
        self.binary = bytearray()
        self.gltf = {
            "asset": {"version": "2.0", "generator": "convert_jungle_ruins.py"},
            "scene": 0,
            "scenes": [{"name": "JungleRuins", "nodes": []}],
            "nodes": [],
            "meshes": [],
            "accessors": [],
            "bufferViews": [],
            "buffers": [{"uri": "jungle_ruins.bin", "byteLength": 0}],
        }
        self.triangles_in_scene = 0
        self.far_triangles_in_scene = 0  # the same at the last level of detail of every plant

    def material(self, material, usd_dir, terrain, two_sided=False):
        """two_sided: the part has clustered levels of detail. Clustering leaves cracks and flat sheets, through which the
        inner side shows; Nau's depth prepass culls the other winding than the opaque master, so an inner side would be a
        black hole. The masked master is two-sided in both passes (its alpha is 1 here, nothing is cut)."""
        path = material.GetPath().pathString
        name = material.GetPath().name
        inputs = material_inputs(material, usd_dir)
        opacity = inputs.get("opacity")
        masked = isinstance(opacity, dict)
        size = self.base_texture_size
        if terrain:
            size *= 2
        key = (name, str(inputs.get("diffuseColor")), str(opacity), str(inputs.get("normal")), str(inputs.get("roughness")), size, two_sided)
        if key in self.materials:
            return self.materials[key]

        file_name = re.sub(r"[^A-Za-z0-9_]", "_", name) + "_" + digest(key)[:6]
        albedo = self.textures.albedo(inputs.get("diffuseColor", (0.3, 0.3, 0.3)), opacity if masked else None, size, TERRAIN_GAIN if terrain else 1.0)
        normal = self.textures.normal(inputs.get("normal"), max(size // 2, 256))
        orm = self.textures.orm(inputs.get("roughness", 0.8), inputs.get("metallic", 0.0), 256)

        properties = {"albedoTex": albedo, "normalTex": normal, "metalRoughnessAoTex": orm}
        pipelines = {pipeline: {"properties": properties} for pipeline in ("default", "instanced")}
        document = {
            "name": file_name,
            "master": MASTER_MASKED if masked or two_sided else MASTER_OPAQUE,
            "pipelines": pipelines,
        }
        with open(os.path.join(self.material_dir, file_name + ".nmat_inst_json"), "w", encoding="utf-8") as f:
            json.dump(document, f, indent=2)

        self.materials[key] = file_name
        return file_name

    def _view(self, data, target=None):
        while len(self.binary) % 4 != 0:
            self.binary.append(0)
        view = {"buffer": 0, "byteOffset": len(self.binary), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.binary.extend(data)
        self.gltf["bufferViews"].append(view)
        return len(self.gltf["bufferViews"]) - 1

    def _accessor(self, data, component, kind, count, target=None, bounds=None):
        accessor = {"bufferView": self._view(data, target), "byteOffset": 0, "componentType": component, "count": count, "type": kind}
        if bounds is not None:
            accessor["min"] = [float(v) for v in bounds[0]]
            accessor["max"] = [float(v) for v in bounds[1]]
        self.gltf["accessors"].append(accessor)
        return len(self.gltf["accessors"]) - 1

    def add_mesh(self, name, positions, normals, uvs, indices):
        """A mesh with one primitive: Nau overrides the material of slot 0 of a mesh."""
        if indices.max() < 65535:
            index_data = indices.astype(np.uint16).tobytes()
            index_component = 5123
        else:
            index_data = indices.astype(np.uint32).tobytes()
            index_component = 5125
        primitive = {
            "attributes": {
                "POSITION": self._accessor(positions.astype(np.float32).tobytes(), 5126, "VEC3", len(positions), 34962,
                                           (positions.min(axis=0), positions.max(axis=0))),
                "NORMAL": self._accessor(normals.astype(np.float32).tobytes(), 5126, "VEC3", len(normals), 34962),
                "TEXCOORD_0": self._accessor(uvs.astype(np.float32).tobytes(), 5126, "VEC2", len(uvs), 34962),
            },
            "indices": self._accessor(index_data, index_component, "SCALAR", len(indices), 34963),
        }
        self.gltf["meshes"].append({"name": name, "primitives": [primitive]})
        return len(self.gltf["meshes"]) - 1

    def set_lods(self, mesh_index, lod_meshes, distances):
        self.gltf["meshes"][mesh_index]["extras"] = {"nau_lod_meshes": lod_meshes, "nau_lod_distances": [float(d) for d in distances]}

    def add_node(self, mesh_index, material_name, translation=None, rotation=None, scale=None):
        # The sample assigns the material by the node name.
        node = {"name": material_name, "mesh": mesh_index}
        if translation is not None:
            node["translation"] = [float(v) for v in translation]
        if rotation is not None:
            node["rotation"] = [float(v) for v in rotation]
        if scale is not None:
            node["scale"] = [float(v) for v in scale]
        self.gltf["nodes"].append(node)
        self.gltf["scenes"][0]["nodes"].append(len(self.gltf["nodes"]) - 1)

    def write(self):
        self.gltf["buffers"][0]["byteLength"] = len(self.binary)
        with open(os.path.join(self.out_dir, "jungle_ruins.gltf"), "w", encoding="utf-8") as f:
            json.dump(self.gltf, f, separators=(",", ":"))
        with open(os.path.join(self.out_dir, "jungle_ruins.bin"), "wb") as f:
            f.write(self.binary)


def keep_share(material_name, detail):
    for pattern, share in DETAIL:
        if re.search(pattern, material_name):
            return min(1.0, share * detail)
    return 1.0


def convert_prototype(scene, class_prim, usd_dir, detail, rng):
    """Meshes of a prototype, in its own coordinates: [(mesh index, material, triangles of every level)]."""
    meshes = [Mesh(prim) for prim in Usd.PrimRange(class_prim, Usd.PrimAllPrimsPredicate) if prim.GetTypeName() == "Mesh"]
    # Nau measures the distance of a level from the origin of an instance: the radius is taken around the origin, the same
    # for all the parts, so that the trunk and the crown switch together.
    radius = max([float(np.linalg.norm(m.points, axis=1).max()) for m in meshes if len(m.points)] + [0.01])
    distances = [radius * level[2] for level in LODS]

    result = []
    for mesh in meshes:
        for material, faces in mesh.parts:
            if len(faces) == 0:
                continue
            triangles = mesh.part_triangles(faces)
            name = material.GetPath().name
            keep = keep_share(name, detail)
            leaf = bool(LEAF_PART.search(name))
            pieces = keep < 1.0 or leaf
            keeps = [keep] + ([keep * level[0] for level in LODS] if pieces else [])
            growths = [MAX_LEAF_GROWTH] + [MAX_LOD_LEAF_GROWTH] * len(LODS)
            levels = thin_out(mesh, triangles, keeps, leaf, rng, growths)

            positions, kept = levels[0]
            vertices, normals, uvs, indices = weld(positions, mesh.normals[kept], mesh.uvs[kept])
            clustered = False
            base_name = "%s_%s" % (class_prim.GetName(), name)
            mesh_index = scene.add_mesh(base_name, vertices, normals, uvs, indices)
            counts = [len(indices) // 3]

            if counts[0] >= LOD_MIN_TRIANGLES:
                lod_meshes, lod_distances = [], []
                for level, (_, cells, _) in enumerate(LODS, start=1):
                    simplified = None
                    if pieces and level < len(levels) and len(levels[level][1]) < 0.9 * len(levels[0][1]):
                        level_positions, level_kept = levels[level]
                        simplified = weld(level_positions, mesh.normals[level_kept], mesh.uvs[level_kept])
                    else:
                        simplified = cluster(vertices, normals, uvs, indices, 2.0 * radius / cells)
                        clustered = simplified is not None
                    # A level is worth it only if it is clearly lighter than the previous one.
                    if simplified is None or len(simplified[3]) // 3 > 0.8 * counts[-1]:
                        break
                    lod_meshes.append(scene.add_mesh("%s_lod%d" % (base_name, level), *simplified))
                    lod_distances.append(distances[level - 1])
                    counts.append(len(simplified[3]) // 3)
                if lod_meshes:
                    scene.set_lods(mesh_index, lod_meshes, lod_distances)
                else:
                    clustered = False
            material_name = scene.material(material, usd_dir, terrain=False, two_sided=clustered)
            result.append((mesh_index, material_name, counts))
    return result


def convert_plain_meshes(scene, stage, usd_dir):
    pattern = re.compile(PLAIN_MESHES)
    for prim in stage.Traverse():
        if prim.GetTypeName() != "Mesh" or not pattern.match(prim.GetPath().pathString + "/"):
            continue
        mesh = Mesh(prim)
        world = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(0)
        matrix = np.array(world, dtype=np.float64)  # row-vector convention of Gf
        rotation = matrix[:3, :3]
        rotation_unit = rotation / np.cbrt(max(abs(np.linalg.det(rotation)), 1e-12))
        points_world = mesh.points @ rotation + matrix[3, :3]
        terrain = "/M_" in prim.GetPath().pathString
        for material, faces in mesh.parts:
            triangles = mesh.part_triangles(faces)
            corners = mesh.point_index[triangles]
            positions = to_nau_points(points_world[corners.reshape(-1)]).reshape(-1, 3)
            normals = to_nau_directions((mesh.normals[triangles].reshape(-1, 3) @ rotation_unit))
            vertices, normals, uvs, indices = weld(positions, normals, mesh.uvs[triangles])
            material_name = scene.material(material, usd_dir, terrain)
            mesh_index = scene.add_mesh(prim.GetName() + "_" + material.GetPath().name, vertices, normals, uvs, indices)
            scene.add_node(mesh_index, material_name)
            scene.triangles_in_scene += len(indices) // 3
            scene.far_triangles_in_scene += len(indices) // 3


def collect_instances(stage):
    """All instancers: prototype name -> arrays of positions, orientations and scales."""
    found = {}
    for prim in stage.Traverse():
        if prim.GetTypeName() != "PointInstancer":
            continue
        instancer = UsdGeom.PointInstancer(prim)
        targets = instancer.GetPrototypesRel().GetTargets()
        positions = instancer.GetPositionsAttr().Get(0) or instancer.GetPositionsAttr().Get()
        ids = instancer.GetProtoIndicesAttr().Get(0) or instancer.GetProtoIndicesAttr().Get()
        if not positions or not ids or not targets:
            continue
        orientations = instancer.GetOrientationsAttr().Get(0) or instancer.GetOrientationsAttr().Get()
        scales = instancer.GetScalesAttr().Get(0) or instancer.GetScalesAttr().Get()
        positions = np.array(positions, dtype=np.float32)
        ids = np.array(ids)
        distance = np.linalg.norm((positions[:, :2] - FOCUS_CM[:2]) * 0.01, axis=1)
        near = np.nonzero(distance < 300.0)[0]
        for index in near.tolist():
            name = targets[int(ids[index])].name
            entry = found.setdefault(name, [])
            q = orientations[index] if orientations else Gf.Quath(1.0)
            s = scales[index] if scales else (100.0, 100.0, 100.0)
            entry.append((float(distance[index]), positions[index], (q.GetReal(), *q.GetImaginary()), tuple(s)))
    return found


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--usd", required=True, help="JungleRuins_Karma.usda")
    parser.add_argument("--out", default=os.path.join(here, "..", "content", "scenes", "jungle_ruins"))
    parser.add_argument("--density", type=float, default=1.0, help="scale of the number of instances")
    parser.add_argument("--detail", type=float, default=1.0, help="scale of the kept share of the foliage")
    parser.add_argument("--texture-size", type=int, default=2048)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    started = time.time()
    out_dir = os.path.normpath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    usd_dir = os.path.dirname(os.path.abspath(args.usd))
    rng = np.random.default_rng(args.seed)

    stage = Usd.Stage.Open(args.usd)
    print("stage opened, %.0f s" % (time.time() - started))

    scene = Scene(out_dir, args.texture_size)
    convert_plain_meshes(scene, stage, usd_dir)
    print("terrain and pyramid: %d triangles, %.0f s" % (scene.triangles_in_scene, time.time() - started))

    instances = collect_instances(stage)
    print("instances near the pyramid: %d of %d prototypes, %.0f s" % (sum(len(v) for v in instances.values()), len(instances), time.time() - started))

    report = []
    for pattern, wanted, radius, boost in FAMILIES:
        regex = re.compile(pattern)
        names = sorted(n for n in instances if regex.match(n))
        minimum = TALL_MIN_DISTANCE if TALL_FAMILIES.match(pattern.lstrip("^")) else 0.0
        candidates = [(name, entry) for name in names for entry in instances[name] if minimum <= entry[0] < radius]
        count = min(int(wanted * args.density), len(candidates))
        if count == 0:
            report.append((pattern, 0, 0))
            continue
        picked = rng.choice(len(candidates), size=count, replace=False)
        prototypes = {}
        family_triangles = 0
        family_far_triangles = 0
        for pick in picked.tolist():
            name, (_, position, quaternion, source_scale) = candidates[pick]
            if name not in prototypes:
                class_prim = stage.GetPrimAtPath("/_class_/" + name)
                prototypes[name] = convert_prototype(scene, class_prim, usd_dir, args.detail, rng) if class_prim else []
            translation = to_nau_points(np.array([position], dtype=np.float64))[0]
            rotation = Z_UP_TO_Y_UP * Gf.Quatd(quaternion[0], Gf.Vec3d(quaternion[1], quaternion[2], quaternion[3]))
            imaginary = rotation.GetImaginary()
            quaternion_nau = (imaginary[0], imaginary[1], imaginary[2], rotation.GetReal())
            # The prototypes are in meters and the source scales them to centimeters.
            scale = [s * 0.01 * boost for s in source_scale]
            for mesh_index, material_name, triangles in prototypes[name]:
                scene.add_node(mesh_index, material_name, translation, quaternion_nau, scale)
                family_triangles += triangles[0]
                family_far_triangles += triangles[-1]
        scene.triangles_in_scene += family_triangles
        scene.far_triangles_in_scene += family_far_triangles
        report.append((pattern, count, family_triangles))
        print("%-18s %5d instances of %6d candidates, %10d triangles, %9d at the last level, %.0f s"
              % (pattern, count, len(candidates), family_triangles, family_far_triangles, time.time() - started))

    scene.write()

    size = sum(os.path.getsize(os.path.join(folder, f)) for folder, _, files in os.walk(out_dir) for f in files)
    print("nodes %d, meshes %d, materials %d, triangles in the scene %d (%d with every plant at its last level of detail)"
          % (len(scene.gltf["nodes"]), len(scene.gltf["meshes"]), len(scene.materials), scene.triangles_in_scene, scene.far_triangles_in_scene))
    print("binary %.0f MB, whole folder %.0f MB, %.0f s" % (len(scene.binary) / 1e6, size / 1e6, time.time() - started))


if __name__ == "__main__":
    main()
