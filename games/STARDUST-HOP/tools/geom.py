"""Tiny glTF writer: boxes, cylinders and spheres with flat colours, embedded as data URIs."""
import base64, json, math, struct


def _box(sx, sy, sz):
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    faces = [
        ((0, 0, 1),  [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1, 0, 0),  [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0, 1, 0),  [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
    ]
    pos, nor, uv, idx = [], [], [], []
    for n, quad in faces:
        base = len(pos)
        for (p, t) in zip(quad, [(0, 0), (1, 0), (1, 1), (0, 1)]):
            pos.append(p); nor.append(n); uv.append(t)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nor, uv, idx


def _cylinder(radius, height, sides=16, axis="y"):
    pos, nor, uv, idx = [], [], [], []
    hh = height / 2
    for i in range(sides):
        a0 = 2 * math.pi * i / sides
        a1 = 2 * math.pi * (i + 1) / sides
        c0, s0, c1, s1 = math.cos(a0), math.sin(a0), math.cos(a1), math.sin(a1)
        base = len(pos)
        pos += [(radius * c0, -hh, radius * s0), (radius * c1, -hh, radius * s1), (radius * c1, hh, radius * s1), (radius * c0, hh, radius * s0)]
        nor += [(c0, 0, s0), (c1, 0, s1), (c1, 0, s1), (c0, 0, s0)]
        uv += [(0, 0), (1, 0), (1, 1), (0, 1)]
        idx += [base, base + 2, base + 1, base, base + 3, base + 2]
        for y, ny, flip in ((hh, 1, False), (-hh, -1, True)):
            b = len(pos)
            pos += [(0, y, 0), (radius * c0, y, radius * s0), (radius * c1, y, radius * s1)]
            nor += [(0, ny, 0)] * 3
            uv += [(0.5, 0.5), (0, 0), (1, 0)]
            idx += [b, b + 2, b + 1] if ny > 0 else [b, b + 1, b + 2]
    if axis == "z":  # lay the cylinder along z (a coin standing on its edge)
        pos = [(x, z, y) for (x, y, z) in pos]
        nor = [(x, z, y) for (x, y, z) in nor]
        idx = [i for tri in zip(idx[0::3], idx[2::3], idx[1::3]) for i in tri]  # swapping two axes flips the winding
    return pos, nor, uv, idx


def _sphere(radius, rings=8, segs=14):
    pos, nor, uv, idx = [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = math.pi * v
        for s in range(segs + 1):
            u = s / segs
            th = 2 * math.pi * u
            n = (math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th))
            pos.append(tuple(radius * c for c in n)); nor.append(n); uv.append((u, v))
    for r in range(rings):
        for s in range(segs):
            a = r * (segs + 1) + s
            b = a + segs + 1
            idx += [a, a + 1, b, a + 1, b + 1, b]
    return pos, nor, uv, idx


def inside_sphere(radius, rings=10, segs=20):
    pos, nor, uv, idx = _sphere(radius, rings, segs)
    nor = [(-x, -y, -z) for (x, y, z) in nor]
    idx = [i for tri in zip(idx[0::3], idx[2::3], idx[1::3]) for i in tri]
    return pos, nor, uv, idx


def box(sx, sy, sz): return _box(sx, sy, sz)
def cylinder(r, h, sides=16, axis="y"): return _cylinder(r, h, sides, axis)
def sphere(r, rings=8, segs=14): return _sphere(r, rings, segs)


def _translate(mesh, offset):
    pos, nor, uv, idx = mesh
    return [(x + offset[0], y + offset[1], z + offset[2]) for (x, y, z) in pos], nor, uv, idx


def gltf(parts, name="model"):
    """parts: list of (mesh, (r, g, b[, a]), offset(x, y, z), emissive(r, g, b) or None)."""
    buffer = bytearray()
    views, accessors, materials, meshes, nodes = [], [], [], [], []

    def add_view(data):
        while len(buffer) % 4:
            buffer.append(0)
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)})
        buffer.extend(data)
        return len(views) - 1

    for i, (mesh, color, offset, emissive) in enumerate(parts):
        pos, nor, uv, idx = _translate(mesh, offset)
        pv = add_view(b"".join(struct.pack("<3f", *p) for p in pos))
        nv = add_view(b"".join(struct.pack("<3f", *n) for n in nor))
        tv = add_view(b"".join(struct.pack("<2f", *t) for t in uv))
        iv = add_view(b"".join(struct.pack("<I", i2) for i2 in idx))
        lo = [min(p[k] for p in pos) for k in range(3)]
        hi = [max(p[k] for p in pos) for k in range(3)]
        base = len(accessors)
        accessors += [
            {"bufferView": pv, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": nv, "componentType": 5126, "count": len(nor), "type": "VEC3"},
            {"bufferView": tv, "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": iv, "componentType": 5125, "count": len(idx), "type": "SCALAR"},
        ]
        rgba = list(color) + [1.0] * (4 - len(color))
        mat = {"name": f"{name}_{i}", "pbrMetallicRoughness": {"baseColorFactor": rgba, "metallicFactor": 0.0, "roughnessFactor": 0.85}}
        if emissive:
            mat["emissiveFactor"] = list(emissive)
        materials.append(mat)
        meshes.append({"name": f"{name}_{i}", "primitives": [{"attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2}, "indices": base + 3, "material": i, "mode": 4}]})
        nodes.append({"name": f"{name}_{i}", "mesh": i})
    doc = {
        "asset": {"version": "2.0", "generator": "stardust hop build"},
        "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode("ascii"), "byteLength": len(buffer)}],
        "bufferViews": views, "accessors": accessors, "materials": materials, "meshes": meshes, "nodes": nodes,
        "scenes": [{"nodes": list(range(len(nodes)))}], "scene": 0,
    }
    return json.dumps(doc, separators=(",", ":"))
