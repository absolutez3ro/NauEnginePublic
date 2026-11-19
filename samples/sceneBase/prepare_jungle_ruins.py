# Copyright 2026 N-GINN LLC. All rights reserved.
"""Prepare a reproducible JungleRuins camera region for Nau's glTF reader.

Run with a Python containing pxr (OpenUSD), numpy, Pillow, tifffile and OpenEXR. No source assets
are modified. Generated assets are external data and must not be committed.
"""

import argparse
import contextlib
import hashlib
import json
import math
import shutil
from pathlib import Path

import numpy as np
from PIL import Image
from pxr import Gf, Usd, UsdGeom, UsdShade


def compact_resources(output, gltf, material_paths):
    # USD prototypes often repeat the same authored textures and material values.
    # Share them before Nau loads the scene, so density costs geometry rather than duplicate VRAM.
    textures, materials, remap, kept_paths = {}, {}, {}, []
    unused_files = set()
    for old_id, asset_path in enumerate(material_paths):
        path = output / "materials" / Path(asset_path).name
        material = json.loads(path.read_text(encoding="utf-8"))
        for pipeline in material["pipelines"].values():
            for name, value in pipeline["properties"].items():
                if not isinstance(value, str) or "/textures/" not in value:
                    continue
                texture = output / "textures" / Path(value).name
                digest = hashlib.sha256(texture.read_bytes()).hexdigest()
                canonical = textures.setdefault(digest, value)
                pipeline["properties"][name] = canonical
                if value != canonical:
                    unused_files.add(texture)
        key = json.dumps(material["pipelines"], sort_keys=True, separators=(",", ":"))
        if key in materials:
            remap[old_id] = materials[key]
            unused_files.add(path)
        else:
            remap[old_id] = len(kept_paths)
            materials[key] = remap[old_id]
            kept_paths.append(asset_path)
            path.write_text(json.dumps(material, indent=2), encoding="utf-8")
    for node_index, node in enumerate(gltf["nodes"]):
        old_id = int(node["name"].split("_m", 1)[1].split("_", 1)[0])
        node["name"] = f"JungleRuins_m{remap[old_id]}_{node_index}"
    for path in unused_files:
        path.unlink()
    return kept_paths


class Exporter:
    def __init__(self, source, output, radius, texture_size, vegetation_density):
        self.source = source
        self.output = output
        self.radius = radius
        self.texture_size = texture_size
        self.vegetation_density = vegetation_density
        self.stage = Usd.Stage.Open(str(source / "USD/JungleRuins_Karma.usda"))
        if not self.stage:
            raise RuntimeError("Cannot open JungleRuins_Karma.usda")
        self.time = Usd.TimeCode.Default()
        self.xforms = UsdGeom.XformCache(self.time)
        self.meters = UsdGeom.GetStageMetersPerUnit(self.stage)
        cameras = [p for p in self.stage.Traverse() if p.IsA(UsdGeom.Camera)]
        camera_world = np.asarray(self.xforms.GetLocalToWorldTransform(cameras[0]))
        self.origin = camera_world[3, :3] * self.meters
        self.origin[2] = 0
        # USD is Z-up; Nau/glTF is Y-up. This is a rotation, not a reflection.
        self.axis = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=np.float64)
        self.gltf = {
            "asset": {"version": "2.0", "generator": "Nau JungleRuins camera-region importer"},
            "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
            "buffers": [{"uri": "jungle_ruins.bin", "byteLength": 0}],
            "bufferViews": [], "accessors": []
        }
        self.materials = {}
        self.stats = {"triangles": 0, "vertices": 0, "maxMeshVertices": 0,
                      "vegetationInstances": {}, "vegetationCandidates": {},
                      "vegetationDensity": vegetation_density, "alphaCutoff": 0.5,
                      "sourceMeshes": [], "radiusMeters": radius,
                      "sourceMetersPerUnit": self.meters, "exportMetersPerUnit": 1.0}
        self.binary = (output / "jungle_ruins.bin").open("wb")

    def surface(self, material):
        if not material:
            return None
        source = material.ComputeSurfaceSource()
        return source[0] if source and source[0] else None

    def opaque(self, material):
        shader = self.surface(material)
        if not shader:
            return True
        opacity = shader.GetInput("opacity")
        if not opacity:
            return True
        value = opacity.Get()
        return not opacity.GetAttr().GetConnections() and (value is None or float(value) >= 0.999)

    def sample_input(self, shader, name, fallback):
        value = shader.GetInput(name) if shader else None
        if not value:
            return fallback
        connections = value.GetAttr().GetConnections()
        if not connections:
            result = value.Get()
            return result if result is not None else fallback
        attr = self.stage.GetAttributeAtPath(connections[0])
        texture = UsdShade.Shader(attr.GetPrim())
        file_input = texture.GetInput("file")
        if not file_input or not file_input.Get():
            raise RuntimeError(f"Unsupported material connection: {connections[0]}")
        asset = file_input.Get()
        path = Path(asset.resolvedPath) if asset.resolvedPath else None
        if not path or not path.is_file():
            # Published source sometimes contains the author's absolute paths.
            relative = str(asset.path).replace("\\", "/")
            marker = relative.lower().find("textures/")
            path = self.source / relative[marker:] if marker >= 0 else self.source / "textures" / Path(relative).name
        if not path.is_file():
            raise FileNotFoundError(path)
        if path.suffix.lower() == ".exr":
            # EXR normals/roughness are linear data; do not apply a display transform.
            import Imath
            import OpenEXR

            with contextlib.closing(OpenEXR.InputFile(str(path))) as texture_file:
                header = texture_file.header()
                window = header["dataWindow"]
                size = (window.max.y - window.min.y + 1, window.max.x - window.min.x + 1)
                channels = header["channels"]
                red = "R" if "R" in channels else next(iter(channels))
                values = [np.frombuffer(texture_file.channel(c if c in channels else red, Imath.PixelType(Imath.PixelType.FLOAT)), dtype=np.float32).reshape(size)
                          for c in ("R", "G", "B")]
                pixels = np.rint(np.clip(np.stack(values, axis=2), 0, 1) * 255).astype(np.uint8)
                image = Image.fromarray(pixels)
        elif path.suffix.lower() in (".tif", ".tiff"):
            # Source TIFFs label their fourth sample as unspecified, not alpha.
            # Pillow drops it; USD's outputs:a still refers to that fourth sample.
            import tifffile

            image = Image.fromarray(tifffile.imread(path, key=0))
        else:
            image = Image.open(path)
        with image:
            image = image.convert("RGBA")
            image.thumbnail((self.texture_size, self.texture_size), Image.Resampling.LANCZOS)
            array = np.asarray(image).copy()
        channel = str(attr.GetName()).split(":")[-1]
        if channel in ("r", "g", "b", "a"):
            array = array[:, :, {"r": 0, "g": 1, "b": 2, "a": 3}[channel]]
        else:
            array = array[:, :, :3]
        return array

    def material(self, material):
        shader = self.surface(material)
        # Deduplicate identical source materials referenced by many USD prims.
        # Instance proxy paths differ; the authored material spec identifies the resource.
        key = "|".join(str(s.layer.identifier) + str(s.path) for s in material.GetPrim().GetPrimStack()) if material else "default"
        if key in self.materials:
            return self.materials[key]["id"]
        name = (material.GetPrim().GetName() if material else "default") + "_" + hashlib.sha256(key.encode()).hexdigest()[:8]
        folder = self.output / "textures"
        folder.mkdir(exist_ok=True)
        albedo = self.sample_input(shader, "diffuseColor", (0.5, 0.5, 0.5))
        normal = self.sample_input(shader, "normal", (0.5, 0.5, 1.0))
        roughness = self.sample_input(shader, "roughness", 0.8)
        metallic = self.sample_input(shader, "metallic", 0.0)

        def rgb_image(value):
            if isinstance(value, np.ndarray):
                image = Image.fromarray(value)
                return image.convert("RGB")
            rgb = [round(float(x) * 255) for x in value]
            return Image.new("RGB", (4, 4), tuple(rgb))

        albedo_image = rgb_image(albedo)
        normal_image = rgb_image(normal)
        size = albedo_image.size

        def channel_image(value):
            if isinstance(value, np.ndarray):
                return Image.fromarray(value).convert("L").resize(size, Image.Resampling.BILINEAR)
            return Image.new("L", size, max(0, min(255, round(float(value) * 255))))

        orm = Image.merge("RGB", (Image.new("L", size, 255), channel_image(roughness), channel_image(metallic)))
        masked = not self.opaque(material)
        if masked:
            threshold = self.sample_input(shader, "opacityThreshold", 0.5)
            if abs(float(threshold) - 0.5) > 1e-6:
                raise RuntimeError(f"Unsupported alpha cutoff in {material.GetPath()}: {threshold}")
            albedo_image.putalpha(channel_image(self.sample_input(shader, "opacity", 1.0)))
        albedo_image.save(folder / (name + "_albedo.png"))
        normal_image.save(folder / (name + "_normal.png"))
        orm.save(folder / (name + "_orm.png"))
        properties = {"albedoTex": f"file:/content/scenes/jungle_ruins/textures/{name}_albedo.png",
                      "normalTex": f"file:/content/scenes/jungle_ruins/textures/{name}_normal.png",
                      "metalRoughnessAoTex": f"file:/content/scenes/jungle_ruins/textures/{name}_orm.png"}
        pipelines = {}
        for pipeline in ("default", "instanced"):
            permutation = ("maskedInstanced" if pipeline == "instanced" else "masked") if masked else ("instanced" if pipeline == "instanced" else "")
            prefix = "standard." + (permutation + "." if permutation else "")
            vertex = prefix + "vs.vsmain"
            settings = {"properties": dict(properties), "shaders": [
                f"file:/res/shaders/cache/shader_cache.nsbc+[{vertex}]",
                f"file:/res/shaders/cache/shader_cache.nsbc+[{prefix}ps.psmain]"],
                "cullMode": "None" if masked else "CounterClockwise", "depthMode": "Default", "blendMode": "Masked" if masked else "Opaque"}
            if pipeline == "instanced":
                settings["properties"]["instanceBaseID"] = [0.0, 0.0, 0.0, 0.0]
            pipelines[pipeline] = settings
        (self.output / "materials").mkdir(exist_ok=True)
        (self.output / "materials" / (name + ".nmat_json")).write_text(
            json.dumps({"name": name, "pipelines": pipelines}, indent=2), encoding="utf-8")
        identifier = len(self.materials)
        self.materials[key] = {"id": identifier, "path": f"file:/content/scenes/jungle_ruins/materials/{name}.nmat_json", "masked": masked}
        return identifier

    def accessor(self, data, element_type, component_type, target):
        padding = (-self.binary.tell()) % 4
        self.binary.write(bytes(padding))
        view = len(self.gltf["bufferViews"])
        self.gltf["bufferViews"].append({"buffer": 0, "byteOffset": self.binary.tell(), "byteLength": data.nbytes, "target": target})
        self.binary.write(data.tobytes())
        result = len(self.gltf["accessors"])
        self.gltf["accessors"].append({"bufferView": view, "componentType": component_type,
                                       "count": len(data), "type": element_type})
        return result

    def write_chunk(self, material_id, corners):
        # Index by position + normal + UV, preserving face-varying seams.
        vertices, indices = np.unique(corners.astype("<f4"), axis=0, return_inverse=True)
        if len(vertices) > 65535:
            raise RuntimeError("Internal error: chunk exceeds Nau's 16-bit vertex limit")
        positions = np.ascontiguousarray(vertices[:, :3])
        # Nau places the culling sphere at the node translation; center each chunk there.
        center = (positions.min(0).astype(np.float64) + positions.max(0)) * 0.5
        positions = np.ascontiguousarray(positions - center, dtype="<f4")
        normals = np.ascontiguousarray(vertices[:, 3:6])
        uvs = np.ascontiguousarray(vertices[:, 6:8])
        # Preserve USD st: the existing Nau standard shader performs the V flip.
        pos_id = self.accessor(positions, "VEC3", 5126, 34962)
        self.gltf["accessors"][pos_id].update(min=positions.min(0).tolist(), max=positions.max(0).tolist())
        normal_id = self.accessor(normals, "VEC3", 5126, 34962)
        uv_id = self.accessor(uvs, "VEC2", 5126, 34962)
        index_id = self.accessor(indices.astype("<u2"), "SCALAR", 5123, 34963)
        mesh = len(self.gltf["meshes"])
        self.gltf["meshes"].append({"primitives": [{"attributes": {"POSITION": pos_id, "NORMAL": normal_id, "TEXCOORD_0": uv_id}, "indices": index_id, "mode": 4}]})
        self.gltf["nodes"].append({"name": f"JungleRuins_m{material_id}_{mesh}", "mesh": mesh, "translation": center.tolist()})
        self.gltf["scenes"][0]["nodes"].append(len(self.gltf["nodes"]) - 1)
        self.stats["triangles"] += len(indices) // 3
        self.stats["vertices"] += len(vertices)
        self.stats["maxMeshVertices"] = max(self.stats["maxMeshVertices"], len(vertices))

    def mesh(self, prim, instance_world=None, prototype=False):
        mesh = UsdGeom.Mesh(prim)
        if mesh.ComputeVisibility(self.time) == "invisible":
            return
        points = np.asarray(mesh.GetPointsAttr().Get(self.time), dtype=np.float64)
        world = np.asarray(self.xforms.GetLocalToWorldTransform(prim)) if instance_world is None else instance_world
        points = ((points @ world[:3, :3] + world[3, :3]) * self.meters - (0 if prototype else self.origin)) @ self.axis
        if not prototype and (np.any(points[:, (0, 2)].min(0) > self.radius) or np.any(points[:, (0, 2)].max(0) < -self.radius)):
            return
        counts = np.asarray(mesh.GetFaceVertexCountsAttr().Get(self.time), dtype=np.int64)
        indices = np.asarray(mesh.GetFaceVertexIndicesAttr().Get(self.time), dtype=np.int64)
        starts = np.concatenate(([0], np.cumsum(counts)[:-1]))
        # Triangulate each polygon as a fan. Source terrain and ruins use triangles/quads.
        triangle_corners, triangle_faces = [], []
        for count in np.unique(counts):
            faces = np.flatnonzero(counts == count)
            for corner in range(1, int(count) - 1):
                triangle_corners.append(starts[faces, None] + np.array([0, corner, corner + 1]))
                triangle_faces.append(faces)
        corners = np.concatenate(triangle_corners)
        faces = np.concatenate(triangle_faces)
        # Conservative AABB clipping: retain triangles crossing the fixed camera-region boundary.
        positions = points[indices[corners]]
        low = positions[:, :, (0, 2)].min(1)
        high = positions[:, :, (0, 2)].max(1)
        nearest = np.maximum(np.maximum(low, -high), 0)
        keep = np.ones(len(corners), dtype=bool) if prototype else np.sum(nearest * nearest, axis=1) <= self.radius * self.radius
        corners, faces = corners[keep], faces[keep]
        if not len(corners):
            return
        normals = np.asarray(mesh.GetNormalsAttr().Get(self.time), dtype=np.float64)
        normal_interp = mesh.GetNormalsInterpolation()
        uv_primvar = UsdGeom.PrimvarsAPI(prim).GetPrimvar("st")
        uv = np.asarray(uv_primvar.ComputeFlattened(self.time), dtype=np.float32) if uv_primvar else None
        material = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
        assignments = np.full(len(counts), -1, dtype=np.int32)
        bound = [material]
        for subset in UsdShade.MaterialBindingAPI(prim).GetMaterialBindSubsets():
            bound.append(UsdShade.MaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial()[0])
            assignments[np.asarray(subset.GetIndicesAttr().Get(), dtype=np.int64)] = len(bound) - 1
        assignments[assignments == -1] = 0
        inverse_normal = np.linalg.inv(world[:3, :3]).T @ self.axis
        reverse = (mesh.GetOrientationAttr().Get() == "leftHanded") != (np.linalg.det(world[:3, :3]) < 0)
        for binding in np.unique(assignments[faces]):
            material = bound[int(binding)]
            selected = corners[assignments[faces] == binding]
            selected_faces = faces[assignments[faces] == binding]
            material_id = self.material(material)
            for start in range(0, len(selected), 20000):
                part = selected[start:start + 20000]
                face_ids = selected_faces[start:start + 20000]
                pos = points[indices[part]]
                if normals.size:
                    ni = part if normal_interp == "faceVarying" else indices[part] if normal_interp in ("vertex", "varying") else face_ids[:, None] if normal_interp == "uniform" else np.zeros_like(part)
                    nrm = normals[ni] @ inverse_normal
                    nrm = np.broadcast_to(nrm, pos.shape).copy()
                else:
                    nrm = np.broadcast_to(np.cross(pos[:, 1]-pos[:, 0], pos[:, 2]-pos[:, 0])[:, None, :], pos.shape).copy()
                missing = np.linalg.norm(nrm, axis=2) < 1e-20
                if np.any(missing):
                    geometric = np.cross(pos[:, 1] - pos[:, 0], pos[:, 2] - pos[:, 0])
                    if reverse:
                        geometric *= -1
                    geometric[np.linalg.norm(geometric, axis=1) < 1e-20] = (0, 1, 0)
                    nrm[missing] = np.broadcast_to(geometric[:, None, :], pos.shape)[missing]
                nrm /= np.maximum(np.linalg.norm(nrm, axis=2, keepdims=True), 1e-20)
                if uv is not None:
                    ui = part if uv_primvar.GetInterpolation() == "faceVarying" else indices[part]
                    tex = uv[ui]
                else:
                    tex = pos[:, :, (0, 2)]
                data = np.concatenate((pos, nrm, tex), axis=2)
                if reverse:
                    data = data[:, [0, 2, 1], :]
                self.write_chunk(material_id, data.reshape(-1, 8))
        self.stats["sourceMeshes"].append(str(prim.GetPath()))
        if instance_world is None:
            print(f"Converted {prim.GetPath()}: total {self.stats['triangles']:,} triangles", flush=True)

    def vegetation(self):
        # Keep a deterministic, bounded sample of each family, rather than millions of nodes.
        budgets = {"RiverForest": 24, "RiverSapling": 24, "QueenForest": 8,
                   "RiverSeedling": 192, "Grass": 2048, "Pyramid": 256,
                   "Shrub": 512, "Moss": 1024, "ShrubSorrel": 128,
                   "Nettle": 128, "Anthurium": 128}
        candidates = {}
        rng = np.random.default_rng(20261102)
        for prim in self.stage.Traverse():
            if not prim.IsA(UsdGeom.PointInstancer):
                continue
            family = prim.GetName().split("_")[0]
            budget = round(budgets.get(family, 0) * self.vegetation_density)
            if not budget:
                continue
            instancer = UsdGeom.PointInstancer(prim)
            if instancer.ComputeVisibility(self.time) == "invisible":
                continue
            positions = np.asarray(instancer.GetPositionsAttr().Get(self.time), dtype=np.float64)
            world = np.asarray(self.xforms.GetLocalToWorldTransform(prim))
            positions = ((positions @ world[:3, :3] + world[3, :3]) * self.meters - self.origin) @ self.axis
            distance_squared = np.sum(positions[:, (0, 2)] ** 2, axis=1)
            keep = distance_squared <= self.radius ** 2
            mask = instancer.ComputeMaskAtTime(self.time)
            if len(mask):
                keep &= np.asarray(mask, dtype=bool)
            ids = np.flatnonzero(keep)
            self.stats["vegetationCandidates"][family] = self.stats["vegetationCandidates"].get(family, 0) + len(ids)
            # Weighted reservoir sampling concentrates detail around the benchmark camera.
            scores = -np.log(np.maximum(rng.random(len(ids)), 1e-20)) * (1 + distance_squared[ids] / 400)
            if len(ids) > budget:
                selected = np.argpartition(scores, budget - 1)[:budget]
                ids, scores = ids[selected], scores[selected]
            entries = candidates.setdefault(family, [])
            entries.extend((float(score), str(prim.GetPath()), int(i)) for score, i in zip(scores, ids))
            if len(entries) > budget:
                entries.sort()
                del entries[budget:]

        prototype_groups = {}
        for family, entries in sorted(candidates.items()):
            entries.sort(key=lambda e: (e[1], e[2]))
            self.stats["vegetationInstances"][family] = len(entries)
            previous_path = None
            for _, path, index in entries:
                if path != previous_path:
                    prim = self.stage.GetPrimAtPath(path)
                    instancer = UsdGeom.PointInstancer(prim)
                    proto_indices = instancer.GetProtoIndicesAttr().Get(self.time)
                    prototypes = instancer.GetPrototypesRel().GetTargets()
                    positions = instancer.GetPositionsAttr().Get(self.time)
                    scales = instancer.GetScalesAttr().Get(self.time)
                    orientations = instancer.GetOrientationsAttr().Get(self.time)
                    parent = self.xforms.GetLocalToWorldTransform(prim)
                    previous_path = path
                prototype = self.stage.GetPrimAtPath(prototypes[int(proto_indices[index])])
                # USD uses row vectors: prototype-local * scale * rotation * translation * parent.
                position = positions[index]
                scale = Gf.Matrix4d().SetScale(Gf.Vec3d(scales[index])) if scales else Gf.Matrix4d(1)
                # Half-precision source quaternions drift from unit length. Normalize
                # before matrix conversion to avoid introducing shear into glTF TRS.
                rotation = Gf.Matrix4d().SetRotate(Gf.Quatd(orientations[index]).GetNormalized()) if orientations else Gf.Matrix4d(1)
                transform = scale * rotation * Gf.Matrix4d().SetTranslate(Gf.Vec3d(position))
                for child in Usd.PrimRange(prototype, Usd.TraverseInstanceProxies()):
                    if child.IsA(UsdGeom.Mesh):
                        relative = self.xforms.GetLocalToWorldTransform(child) * parent.GetInverse()
                        source = tuple((s.layer.identifier, str(s.path)) for s in child.GetPrimStack())
                        key = (source, np.round(np.asarray(relative), 8).tobytes())
                        group = prototype_groups.setdefault(key, {"prim": child, "relative": relative, "transforms": []})
                        source_world = np.asarray(transform * parent)
                        world = np.eye(4)
                        world[:3, :3] = self.axis.T @ source_world[:3, :3] @ self.axis
                        world[3, :3] = (source_world[3, :3] * self.meters - self.origin) @ self.axis
                        group["transforms"].append(world)
            print(f"Selected vegetation {family}: {len(entries)} instances", flush=True)

        for group in prototype_groups.values():
            # Share ordinary glTF meshes between nodes; Nau batches these as GPU instances.
            # Bake the largest selected scale into the prototype. Node transforms then only
            # contract it, so Nau's unscaled culling radius remains conservative after rotation.
            max_scale = max(np.linalg.svd(w[:3, :3], compute_uv=False)[0] for w in group["transforms"])
            first_node = len(self.gltf["nodes"])
            first_scene_node = len(self.gltf["scenes"][0]["nodes"])
            scaled = group["relative"] * Gf.Matrix4d().SetScale(float(max_scale))
            self.mesh(group["prim"], np.asarray(scaled), prototype=True)
            chunks = self.gltf["nodes"][first_node:]
            del self.gltf["nodes"][first_node:]
            del self.gltf["scenes"][0]["nodes"][first_scene_node:]
            for world in group["transforms"]:
                world = world.copy()
                world[:3, :3] /= max_scale
                for chunk in chunks:
                    centered = world.copy()
                    centered[3, :3] += np.asarray(chunk["translation"]) @ world[:3, :3]
                    # Preserve the source S*R order; Gf.Transform may return a scale
                    # orientation, which core glTF TRS cannot represent separately.
                    scale_values = np.linalg.norm(centered[:3, :3], axis=1)
                    rotation_matrix = np.eye(4)
                    rotation_matrix[:3, :3] = centered[:3, :3] / scale_values[:, None]
                    if np.linalg.det(rotation_matrix[:3, :3]) < 0:
                        scale_values[0] *= -1
                        rotation_matrix[0, :3] *= -1
                    scale = Gf.Vec3d(*scale_values)
                    rotation = Gf.Matrix4d(rotation_matrix.tolist()).ExtractRotationQuat()
                    translation = Gf.Vec3d(*centered[3, :3])
                    reconstructed = Gf.Matrix4d().SetScale(scale) * Gf.Matrix4d().SetRotate(rotation) * Gf.Matrix4d().SetTranslate(translation)
                    if not np.allclose(np.asarray(reconstructed), centered, atol=1e-6, rtol=1e-6):
                        error = np.abs(np.asarray(reconstructed) - centered).max()
                        raise RuntimeError(f"Vegetation contains unsupported shear in {group['prim'].GetPath()} (matrix error {error})")
                    node = {"name": chunk["name"], "mesh": chunk["mesh"], "translation": list(translation),
                            "rotation": list(rotation.GetImaginary()) + [rotation.GetReal()], "scale": list(scale)}
                    self.gltf["scenes"][0]["nodes"].append(len(self.gltf["nodes"]))
                    self.gltf["nodes"].append(node)
            print(f"Instanced {group['prim'].GetName()}: {len(group['transforms'])} placements, {len(chunks)} shared chunks", flush=True)
        self.stats["vegetationPrototypes"] = len(prototype_groups)

    def run(self):
        for prim in self.stage.Traverse():
            if prim.IsA(UsdGeom.Mesh) and "/Prototypes/" not in str(prim.GetPath()):
                self.mesh(prim)
        self.vegetation()
        self.gltf["buffers"][0]["byteLength"] = self.binary.tell()
        self.binary.close()
        if not self.gltf["meshes"]:
            raise RuntimeError("The selected region contains no geometry")
        material_paths = compact_resources(self.output, self.gltf, [x["path"] for x in self.materials.values()])
        self.stats["uniqueGeometryTriangles"] = self.stats["triangles"]
        self.stats["triangles"] = sum(self.gltf["accessors"][self.gltf["meshes"][n["mesh"]]["primitives"][0]["indices"]]["count"] // 3 for n in self.gltf["nodes"])
        (self.output / "jungle_ruins.gltf").write_text(json.dumps(self.gltf, separators=(",", ":")), encoding="utf-8")
        self.stats["sourceMeshes"] = sorted(set(self.stats["sourceMeshes"]))
        self.stats.update(meshes=len(self.gltf["meshes"]), meshInstances=len(self.gltf["nodes"]), originSourceMeters=self.origin.tolist(),
                          camera={"position": [0, 1.385, 0], "pitch": math.radians(-0.68), "yaw": math.radians(38.73), "fov": 50.0},
                          materials=material_paths,
                          maskedMaterials=sum(json.loads((self.output / "materials" / Path(p).name).read_text(encoding="utf-8"))["pipelines"]["default"]["blendMode"] == "Masked" for p in material_paths),
                          uvConvention="USD st; Nau standard shader performs the V flip",
                          vegetation="Deterministic seed 20261102, bounded camera-weighted sample; shared glTF prototype chunks and GPU instancing. Largest instance scale baked into each prototype for conservative culling without engine changes. Alpha cutoff 0.5 in all passes.")
        (self.output / "manifest.json").write_text(json.dumps(self.stats, indent=2), encoding="utf-8")
        shutil.copy2(self.source / "credits_license.txt", self.output / "credits_license.txt")
        print(json.dumps({k: v for k, v in self.stats.items() if k not in ("materials", "sourceMeshes")}, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Extracted JungleRuins folder")
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "content/scenes/jungle_ruins")
    parser.add_argument("--radius", type=float, default=80.0, help="Fixed camera-region radius in meters")
    parser.add_argument("--texture-size", type=int, default=2048, help="Maximum dimension of converted textures")
    parser.add_argument("--vegetation-density", type=float, default=0.0625, help="Multiplier for bounded plant-family budgets (default 0.0625; 0 disables vegetation)")
    args = parser.parse_args()
    if not math.isfinite(args.radius) or args.radius <= 0 or not 4 <= args.texture_size <= 16384:
        parser.error("Invalid radius or texture size")
    if not math.isfinite(args.vegetation_density) or not 0 <= args.vegetation_density <= 4:
        parser.error("Vegetation density must be between 0 and 4")
    if (args.output / "jungle_ruins.gltf").exists():
        parser.error("Output already exists; choose a new --output directory")
    args.output.mkdir(parents=True, exist_ok=True)
    Exporter(args.source.resolve(), args.output.resolve(), args.radius, args.texture_size, args.vegetation_density).run()


if __name__ == "__main__":
    main()
