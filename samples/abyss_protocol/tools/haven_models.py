"""Fits and shrinks the Abyss Protocol models (128 Meshy GLBs) for an Aether scene.

Every model arrives normalised to about 1 unit across with three 2048 x 2048 textures, and there are 128 of them (384 images). The renderer has ONE table of 128 texture
slots for everything it samples (its own targets included), so that many textures cannot all be bound at once: most of the models would draw plain. So:

- a *hero* model keeps its own textures (cut to `max_texture` pixels across);
- every other model gives up its normal and metallic-roughness maps, and its base colour goes into a shared atlas (a 4096 x 4096 image of 512-pixel tiles, kept as an
  external image next to the models so the renderer loads it once); the model's UVs are moved into its tile.

Either way the model is written to Content/Models/<AP-id>.glb at a real-world size, standing on y = 0 and centred on x and z.
"""
import copy
import io
import json
import math
import pathlib
import struct

from PIL import Image


def read_glb(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:4] == b"glTF" and struct.unpack("<I", data[4:8])[0] == 2, f"{path}: not a glb 2.0"
    json_len = struct.unpack("<I", data[12:16])[0]
    doc = json.loads(data[20:20 + json_len])
    binary = data[20 + json_len + 8:]   # the BIN chunk's bytes (its 8-byte header is dropped)
    return doc, binary


def write_glb(path, doc, binary):
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    chunk = struct.pack("<II", len(binary), 0x004E4942) + binary
    total = 12 + 8 + len(text) + len(chunk)
    pathlib.Path(path).write_bytes(b"glTF" + struct.pack("<II", 2, total) + struct.pack("<I", len(text)) + b"JSON" + text + chunk)


def bounds(doc):
    lo, hi = [1e30] * 3, [-1e30] * 3
    for mesh in doc["meshes"]:
        for prim in mesh["primitives"]:
            acc = doc["accessors"][prim["attributes"]["POSITION"]]
            for i in range(3):
                lo[i], hi[i] = min(lo[i], acc["min"][i]), max(hi[i], acc["max"][i])
    return lo, hi


def shrink_textures(doc, binary, max_texture):
    """Re-encodes every embedded image wider than `max_texture` at that width and rebuilds the binary chunk (every buffer view moves). Returns the new binary."""
    replaced = {}
    for image in doc.get("images", []):
        view = doc["bufferViews"][image["bufferView"]]
        raw = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
        pic = Image.open(io.BytesIO(raw))
        if max(pic.size) <= max_texture:
            continue
        scale = max_texture / max(pic.size)
        pic = pic.resize((max(1, round(pic.width * scale)), max(1, round(pic.height * scale))), Image.LANCZOS)
        out = io.BytesIO()
        if image.get("mimeType") == "image/jpeg":
            pic.convert("RGB").save(out, "JPEG", quality=90)
        else:
            pic.save(out, "PNG", optimize=False)
        replaced[image["bufferView"]] = out.getvalue()
    if not replaced:
        return binary
    pieces, offset = [], 0
    for index, view in enumerate(doc["bufferViews"]):
        data = replaced.get(index, binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]])
        view["byteOffset"], view["byteLength"] = offset, len(data)
        pad = -len(data) % 4
        pieces.append(data + b"\0" * pad)
        offset += len(data) + pad
    doc["buffers"][0]["byteLength"] = offset
    return b"".join(pieces)


def drop_images(doc, binary):
    """Removes every image, texture and texture reference from the document and the bytes of the buffer views that held the images. Returns the new binary."""
    image_views = {image["bufferView"] for image in doc.get("images", []) if "bufferView" in image}
    keep = [i for i in range(len(doc["bufferViews"])) if i not in image_views]
    remap = {old: new for new, old in enumerate(keep)}
    pieces, offset, views = [], 0, []
    for old in keep:
        view = doc["bufferViews"][old]
        data = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
        view = dict(view, byteOffset=offset)
        views.append(view)
        pad = -len(data) % 4
        pieces.append(data + b"\0" * pad)
        offset += len(data) + pad
    for accessor in doc["accessors"]:
        if "bufferView" in accessor:
            accessor["bufferView"] = remap[accessor["bufferView"]]
    doc["bufferViews"] = views
    doc["buffers"][0]["byteLength"] = offset
    doc.pop("images", None)
    doc.pop("textures", None)
    doc.pop("samplers", None)
    return b"".join(pieces)


def read_base_color_image(doc, binary):
    """The model's base colour texture as a PIL image (RGB), or None."""
    for material in doc.get("materials", []):
        info = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if info is None:
            continue
        image = doc["images"][doc["textures"][info["index"]]["source"]]
        view = doc["bufferViews"][image["bufferView"]]
        return Image.open(io.BytesIO(binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]])).convert("RGB")
    return None


def remap_uvs(doc, binary, tile_x, tile_y, scale, inset):
    """Moves every TEXCOORD_0 of the document into the atlas tile at (tile_x, tile_y) (fractions of the atlas, `scale` across), each coordinate first held to
    [inset, 1 - inset] of the tile so a sample stays off the tile's edge. Edits `binary` in place (a bytearray); returns the number of accessors changed."""
    done = set()
    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            index = primitive["attributes"].get("TEXCOORD_0")
            if index is None or index in done:
                continue
            done.add(index)
            accessor = doc["accessors"][index]
            assert accessor["componentType"] == 5126 and accessor["type"] == "VEC2", "TEXCOORD_0 is not a float VEC2"
            view = doc["bufferViews"][accessor["bufferView"]]
            stride = view.get("byteStride", 8)
            base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            lo, hi = [1e9, 1e9], [-1e9, -1e9]
            for i in range(accessor["count"]):
                at = base + i * stride
                u, v = struct.unpack_from("<ff", binary, at)
                u = min(max(u, inset), 1.0 - inset) * scale + tile_x
                v = min(max(v, inset), 1.0 - inset) * scale + tile_y
                struct.pack_into("<ff", binary, at, u, v)
                lo = [min(lo[0], u), min(lo[1], v)]
                hi = [max(hi[0], u), max(hi[1], v)]
            if "min" in accessor:
                accessor["min"], accessor["max"] = lo, hi
    return len(done)


def use_atlas(doc, atlas_uri):
    """Points every material's base colour at the atlas image (an external file) and drops its other maps, keeping the factors, with a middling surface where a
    metallic-roughness map is gone."""
    doc["images"] = [{"uri": atlas_uri, "mimeType": "image/png"}]
    doc["textures"] = [{"source": 0}]
    doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
    doc["textures"][0]["sampler"] = 0
    for material in doc.get("materials", []):
        pbr = material.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"] = {"index": 0}
        if "metallicRoughnessTexture" in pbr:
            pbr.pop("metallicRoughnessTexture")
            pbr["metallicFactor"], pbr["roughnessFactor"] = 0.25, 0.6
        for key in ("normalTexture", "occlusionTexture", "emissiveTexture"):
            material.pop(key, None)


class Atlas:
    """A grid of `tile`-pixel tiles in a `size` x `size` image; `add` puts a picture in the next free tile and returns where. Several atlases when one fills."""

    def __init__(self, directory, tile=512, size=4096, prefix="atlas"):
        self.directory, self.tile, self.size, self.prefix = pathlib.Path(directory), tile, size, prefix
        self.per_row = size // tile
        self.images = []
        self.count = 0

    def add(self, picture):
        per_atlas = self.per_row * self.per_row
        index, slot = divmod(self.count, per_atlas)
        if index == len(self.images):
            self.images.append(Image.new("RGB", (self.size, self.size), (90, 90, 90)))
        row, col = divmod(slot, self.per_row)
        self.images[index].paste(picture.resize((self.tile, self.tile), Image.LANCZOS), (col * self.tile, row * self.tile))
        self.count += 1
        return f"{self.prefix}_{index}.png", col * self.tile / self.size, row * self.tile / self.size, self.tile / self.size

    def save(self):
        names = []
        for index, image in enumerate(self.images):
            name = f"{self.prefix}_{index}.png"
            image.save(self.directory / name)
            names.append(name)
        return names


def fit(src, dst, size, yaw_degrees=0.0, long_axis_z=True, max_texture=1024, by="fit", atlas=None):
    """Writes `src` to `dst` scaled uniformly so it fits in `size` = (x, y, z) metres (`by` = "fit": the best fit in all three; "width": the cross-section
    (x and y) fills size[0] and size[1] whichever is the tighter, the length follows), standing on y = 0 and centred on x and z. A model whose long side runs along X
    is turned a quarter first when `long_axis_z`, so a vehicle's length is its Z. With an `Atlas` the model's base colour goes into it and the model uses it (no other
    maps); without one the model keeps its own textures, cut to `max_texture`. Returns the metres it ended up across: (x, y, z)."""
    doc, binary = read_glb(src)
    lo, hi = bounds(doc)
    dims = [hi[i] - lo[i] for i in range(3)]
    theta = math.radians(yaw_degrees)
    if long_axis_z and dims[0] > dims[2] * 1.2 and dims[0] > dims[1]:
        theta += math.pi / 2
    c, s = math.cos(theta), math.sin(theta)
    ex = abs(c) * dims[0] + abs(s) * dims[2]
    ez = abs(s) * dims[0] + abs(c) * dims[2]
    if by == "width":
        k = min(size[0] / ex, size[1] / dims[1])
    else:
        k = min(size[0] / ex, size[1] / dims[1], size[2] / ez)
    centre = [(hi[i] + lo[i]) / 2 for i in range(3)]
    cols = [[k * c, 0.0, -k * s], [0.0, k, 0.0], [k * s, 0.0, k * c]]   # column-major R * S
    mapped = [sum(cols[j][i] * centre[j] for j in range(3)) for i in range(3)]
    translation = [-mapped[0], -k * lo[1], -mapped[2]]
    doc["nodes"][0]["matrix"] = [cols[0][0], cols[0][1], cols[0][2], 0.0, cols[1][0], cols[1][1], cols[1][2], 0.0, cols[2][0], cols[2][1], cols[2][2], 0.0,
                                 translation[0], translation[1], translation[2], 1.0]
    if atlas is not None:
        picture = read_base_color_image(doc, binary)
        binary = bytearray(drop_images(doc, binary))
        if picture is None:
            picture = Image.new("RGB", (8, 8), tuple(int(255 * c) for c in doc["materials"][0]["pbrMetallicRoughness"].get("baseColorFactor", [0.5, 0.5, 0.5])[:3]))
        uri, tile_x, tile_y, tile_scale = atlas.add(picture)
        remap_uvs(doc, binary, tile_x, tile_y, tile_scale, inset=0.5 / atlas.tile)
        use_atlas(doc, uri)
        binary = bytes(binary)
    else:
        binary = shrink_textures(doc, binary, max_texture)
    write_glb(dst, doc, binary)
    return (ex * k, dims[1] * k, ez * k)
