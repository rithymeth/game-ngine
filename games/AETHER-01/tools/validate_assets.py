#!/usr/bin/env python3
"""Validate the AETHER-01 glTF buffers, stable metadata, and scene wiring."""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODELS = [
    ROOT / "Content/Characters/Kael/Kael_Recon.gltf",
    ROOT / "Content/Weapons/AEGIS/AEGIS_PrecisionCarbine.gltf",
    ROOT / "Content/Environments/LastLight/LastLight_Valley.gltf",
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_model(path, guids):
    doc = json.loads(path.read_text())
    require(doc.get("asset", {}).get("version") == "2.0", f"{path}: not glTF 2.0")
    require(len(doc.get("scenes", [])) > 0, f"{path}: missing scene")
    require(len(doc.get("meshes", [])) > 0, f"{path}: missing meshes")
    require(len(doc.get("materials", [])) > 0, f"{path}: missing materials")
    meta = json.loads(path.with_name(path.name + ".ameta").read_text())
    require(meta.get("$type") == "AssetMeta", f"{path}: invalid metadata type")
    require(meta.get("guid") not in guids, f"{path}: duplicate asset GUID")
    guids.add(meta["guid"])

    for buffer in doc.get("buffers", []):
        binary = path.parent / buffer["uri"]
        require(binary.is_file(), f"{path}: missing buffer {binary.name}")
        require(binary.stat().st_size == buffer["byteLength"], f"{path}: buffer length mismatch")
    for view in doc.get("bufferViews", []):
        require(view["buffer"] < len(doc["buffers"]), f"{path}: invalid bufferView buffer")
        end = view.get("byteOffset", 0) + view["byteLength"]
        require(end <= doc["buffers"][view["buffer"]]["byteLength"], f"{path}: bufferView out of bounds")
    for accessor in doc.get("accessors", []):
        require(accessor["bufferView"] < len(doc["bufferViews"]), f"{path}: invalid accessor view")
        view = doc["bufferViews"][accessor["bufferView"]]
        size = {5123: 2, 5125: 4, 5126: 4}.get(accessor["componentType"])
        arity = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}.get(accessor["type"])
        require(size and arity, f"{path}: unsupported accessor format")
        stride = view.get("byteStride", size * arity)
        offset = accessor.get("byteOffset", 0)
        require(offset + (accessor["count"] - 1) * stride + size * arity <= view["byteLength"], f"{path}: accessor out of bounds")
    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            require(primitive.get("mode", 4) == 4, f"{path}: expected triangle primitives")
            require(primitive["material"] < len(doc["materials"]), f"{path}: invalid material index")
            indices = doc["accessors"][primitive["indices"]]
            require(indices["count"] % 3 == 0, f"{path}: non-triangle index count")
            require(set(primitive["attributes"]) >= {"POSITION", "NORMAL"}, f"{path}: position/normal missing")
    return doc, meta


def main():
    guids = set()
    docs = {}
    metas = {}
    for model in MODELS:
        docs[model.name], metas[model.name] = validate_model(model, guids)
    scene_path = ROOT / "Content/Scenes/LastLight_Valley.ascene"
    scene = json.loads(scene_path.read_text())
    scene_meta = json.loads(scene_path.with_name(scene_path.name + ".ameta").read_text())
    require(scene_meta["guid"] not in guids, "scene has duplicate asset GUID")
    guids.add(scene_meta["guid"])
    model = docs["LastLight_Valley.gltf"]
    env_meta = metas["LastLight_Valley.gltf"]
    entities = scene.get("entities", [])
    require(len(entities) == 1, "scene should contain one environment root")
    renderer = entities[0].get("components", {}).get("ModelRenderer", {})
    require(renderer.get("model") == env_meta["guid"], "scene model GUID does not match environment metadata")
    require(renderer.get("asset_path") == "Environments/LastLight/LastLight_Valley.gltf", "scene environment path mismatch")
    require(scene["$type"] == "Scene" and model["asset"]["version"] == "2.0", "scene/model format mismatch")
    print(f"Validated {len(MODELS)} glTF models and 1 scene; {len(guids)} unique asset GUIDs.")


if __name__ == "__main__":
    try:
        main()
    except (KeyError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Asset validation failed: {exc}", file=sys.stderr)
        sys.exit(1)
