# Copyright 2026 N-GINN LLC. All rights reserved.
"""Prepare a reproducible, opaque JungleRuins camera region for Nau's glTF reader.

Run with a Python containing pxr (OpenUSD), numpy and Pillow. No source assets
are modified. Generated assets are external data and must not be committed.
"""

import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path

import numpy as np
from PIL import Image
from pxr import Usd, UsdGeom, UsdShade


class Exporter:
    def __init__(self, source, output, radius, texture_size):
        self.source = source
        self.output = output
        self.radius = radius
        self.texture_size = texture_size
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
                      "excludedInstancedVegetation": 0, "excludedAlphaMaterials": [],
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
        with Image.open(path) as image:
            image = image.convert("RGB")
            image.thumbnail((self.texture_size, self.texture_size), Image.Resampling.LANCZOS)
            array = np.asarray(image).copy()
        channel = str(attr.GetName()).split(":")[-1]
        if channel in ("r", "g", "b", "a"):
            array = array[:, :, {"r": 0, "g": 1, "b": 2, "a": 0}[channel]]
        return array

    def material(self, material):
        shader = self.surface(material)
        # Deduplicate identical source materials referenced by many USD prims.
        key = str(material.GetPath()) if material else "default"
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
        albedo_image.save(folder / (name + "_albedo.png"))
        normal_image.save(folder / (name + "_normal.png"))
        orm.save(folder / (name + "_orm.png"))
        properties = {"albedoTex": f"file:/content/scenes/jungle_ruins/textures/{name}_albedo.png",
                      "normalTex": f"file:/content/scenes/jungle_ruins/textures/{name}_normal.png",
                      "metalRoughnessAoTex": f"file:/content/scenes/jungle_ruins/textures/{name}_orm.png"}
        pipelines = {}
        for pipeline in ("default", "instanced"):
            vertex = "standard.instanced.vs.vsmain" if pipeline == "instanced" else "standard.vs.vsmain"
            settings = {"properties": dict(properties), "shaders": [
                f"file:/res/shaders/cache/shader_cache.nsbc+[{vertex}]",
                "file:/res/shaders/cache/shader_cache.nsbc+[standard.ps.psmain]"],
                "cullMode": "CounterClockwise", "depthMode": "Default", "blendMode": "Opaque"}
            if pipeline == "instanced":
                settings["properties"]["instanceBaseID"] = [0.0, 0.0, 0.0, 0.0]
            pipelines[pipeline] = settings
        (self.output / "materials").mkdir(exist_ok=True)
        (self.output / "materials" / (name + ".nmat_json")).write_text(
            json.dumps({"name": name, "pipelines": pipelines}, indent=2), encoding="utf-8")
        identifier = len(self.materials)
        self.materials[key] = {"id": identifier, "path": f"file:/content/scenes/jungle_ruins/materials/{name}.nmat_json"}
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
        self.gltf["scenes"][0]["nodes"].append(mesh)
        self.stats["triangles"] += len(indices) // 3
        self.stats["vertices"] += len(vertices)
        self.stats["maxMeshVertices"] = max(self.stats["maxMeshVertices"], len(vertices))

    def mesh(self, prim):
        mesh = UsdGeom.Mesh(prim)
        if mesh.ComputeVisibility(self.time) == "invisible":
            return
        points = np.asarray(mesh.GetPointsAttr().Get(self.time), dtype=np.float64)
        world = np.asarray(self.xforms.GetLocalToWorldTransform(prim))
        points = ((points @ world[:3, :3] + world[3, :3]) * self.meters - self.origin) @ self.axis
        if np.any(points[:, (0, 2)].min(0) > self.radius) or np.any(points[:, (0, 2)].max(0) < -self.radius):
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
        keep = np.sum(nearest * nearest, axis=1) <= self.radius * self.radius
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
            if not self.opaque(material):
                self.stats["excludedAlphaMaterials"].append(str(material.GetPath()))
                continue
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
        print(f"Converted {prim.GetPath()}: total {self.stats['triangles']:,} triangles", flush=True)

    def run(self):
        for prim in self.stage.Traverse():
            if prim.IsA(UsdGeom.PointInstancer):
                self.stats["excludedInstancedVegetation"] += len(UsdGeom.PointInstancer(prim).GetPositionsAttr().Get(self.time))
            elif prim.IsA(UsdGeom.Mesh) and "/Prototypes/" not in str(prim.GetPath()):
                self.mesh(prim)
        self.gltf["buffers"][0]["byteLength"] = self.binary.tell()
        self.binary.close()
        if not self.gltf["meshes"]:
            raise RuntimeError("The selected region contains no opaque geometry")
        (self.output / "jungle_ruins.gltf").write_text(json.dumps(self.gltf, separators=(",", ":")), encoding="utf-8")
        self.stats["excludedAlphaMaterials"] = sorted(set(self.stats["excludedAlphaMaterials"]))
        self.stats["sourceMeshes"] = sorted(set(self.stats["sourceMeshes"]))
        self.stats.update(meshes=len(self.gltf["meshes"]), originSourceMeters=self.origin.tolist(),
                          camera={"position": [0, 1.385, 0], "pitch": math.radians(-0.68), "yaw": math.radians(38.73), "fov": 50.0},
                          materials=[x["path"] for x in self.materials.values()],
                          uvConvention="USD st; Nau standard shader performs the V flip",
                          vegetation="Point-instanced vegetation and alpha-masked subsets excluded; opaque terrain, ruins and any baked trunks inside the region retained.")
        (self.output / "manifest.json").write_text(json.dumps(self.stats, indent=2), encoding="utf-8")
        shutil.copy2(self.source / "credits_license.txt", self.output / "credits_license.txt")
        print(json.dumps({k: v for k, v in self.stats.items() if k not in ("materials", "sourceMeshes", "excludedAlphaMaterials")}, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Extracted JungleRuins folder")
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "content/scenes/jungle_ruins")
    parser.add_argument("--radius", type=float, default=80.0, help="Fixed camera-region radius in meters")
    parser.add_argument("--texture-size", type=int, default=2048, help="Maximum dimension of converted textures")
    args = parser.parse_args()
    if not math.isfinite(args.radius) or args.radius <= 0 or not 4 <= args.texture_size <= 16384:
        parser.error("Invalid radius or texture size")
    if (args.output / "jungle_ruins.gltf").exists():
        parser.error("Output already exists; choose a new --output directory")
    args.output.mkdir(parents=True, exist_ok=True)
    Exporter(args.source.resolve(), args.output.resolve(), args.radius, args.texture_size).run()


if __name__ == "__main__":
    main()
