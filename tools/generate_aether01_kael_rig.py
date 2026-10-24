"""Regenerate AETHER-01's temporary skinned Kael animation rig, move set and animation graph (M8.6).

Kael's production model (Characters/Kael/Kael_Recon.gltf) is rigid parts with no
skin or clips, so the character gate runs on a small, procedurally generated
stand-in: a 13-bone humanoid with one skinned box per bone and the slice move set
(idle, walk, run, sprint, jump, fall, land, aim, fire, reload, hit, death,
interact) as glTF animations. Walk, run and sprint carry root translation so root
motion can be checked across frame rates. Also written: the animation graph the
player runs on Kael (Kael.aanim), the montage the archive sequence plays on him
(Kael_ArchiveListen.amontage), and a second rig with other bone names,
proportions and an A-pose rest (Characters/Warden/Warden_Rig.gltf) that the
retargeting tests map Kael's clips onto.

Stdlib only and deterministic: running it twice writes identical bytes.

    python3 tools/generate_aether01_kael_rig.py
"""
from pathlib import Path
import base64
import json
import math
import struct
import uuid

CONTENT = Path(__file__).resolve().parents[1] / "games/AETHER-01/Content"
FPS = 30


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def meta(path, importer):
    relative = path.relative_to(CONTENT).as_posix()
    write_json(Path(str(path) + ".ameta"), {"$type": "AssetMeta", "$v": 1,
        "guid": str(uuid.uuid5(uuid.NAMESPACE_URL, "aether01/prototype/" + relative)),
        "importer": importer, "importer_version": 1, "settings": {}, "labels": ["Prototype", "generated"], "source_hash": ""})


# --- Quaternions (x, y, z, w) -------------------------------------------------------------
def axis_angle(axis, degrees):
    h = math.radians(degrees) / 2
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def rotate(q, v):
    p = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return p[:3]


X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)
IDENTITY = (0.0, 0.0, 0.0, 1.0)

# --- Kael's skeleton ------------------------------------------------------------------------
# (name, parent, rest translation from the parent)
KAEL_BONES = [
    ("Root", -1, (0, 0, 0)),
    ("Hips", 0, (0, 1.0, 0)),
    ("Spine", 1, (0, 0.2, 0)),
    ("Chest", 2, (0, 0.2, 0)),
    ("Head", 3, (0, 0.28, 0)),
    ("UpperArm_L", 3, (0.2, 0.15, 0)),
    ("Forearm_L", 5, (0.3, 0, 0)),
    ("UpperArm_R", 3, (-0.2, 0.15, 0)),
    ("Forearm_R", 7, (-0.3, 0, 0)),
    ("Thigh_L", 1, (0.1, -0.05, 0)),
    ("Shin_L", 9, (0, -0.45, 0)),
    ("Thigh_R", 1, (-0.1, -0.05, 0)),
    ("Shin_R", 11, (0, -0.45, 0)),
]


def clip_pose(name, t, duration):
    """Bone rotations (and the Root translation) of a clip at time t."""
    p = t / duration  # 0..1
    cycle = math.sin(2 * math.pi * p)
    r = {}
    root = (0.0, 0.0, 0.0)
    if name == "Idle":
        r["Chest"] = axis_angle(X, 2.5 * cycle)
        r["Head"] = axis_angle(Y, 4 * math.sin(math.pi * p))
    elif name in ("Walk", "Run", "Sprint"):
        swing = {"Walk": 25, "Run": 45, "Sprint": 55}[name]
        speed = {"Walk": 2.0, "Run": 6.0, "Sprint": 8.0}[name]
        r["Thigh_L"] = axis_angle(X, swing * cycle)
        r["Thigh_R"] = axis_angle(X, -swing * cycle)
        r["Shin_L"] = axis_angle(X, -0.6 * swing * max(0.0, -cycle))
        r["Shin_R"] = axis_angle(X, -0.6 * swing * max(0.0, cycle))
        r["UpperArm_L"] = axis_angle(X, -0.7 * swing * cycle)
        r["UpperArm_R"] = axis_angle(X, 0.7 * swing * cycle)
        r["Spine"] = axis_angle(X, {"Walk": 3, "Run": 10, "Sprint": 16}[name])
        root = (0.0, 0.03 * abs(math.sin(2 * math.pi * p)), speed * t)
    elif name == "Jump":
        crouch = math.sin(math.pi * p)
        r["Thigh_L"] = r["Thigh_R"] = axis_angle(X, 40 * crouch)
        r["Shin_L"] = r["Shin_R"] = axis_angle(X, -70 * crouch)
        r["UpperArm_L"] = axis_angle(Z, 30 * p)
        r["UpperArm_R"] = axis_angle(Z, -30 * p)
    elif name == "Fall":
        r["UpperArm_L"] = axis_angle(Z, 50 + 5 * cycle)
        r["UpperArm_R"] = axis_angle(Z, -50 - 5 * cycle)
        r["Thigh_L"] = axis_angle(X, 15 + 5 * cycle)
        r["Thigh_R"] = axis_angle(X, 5 - 5 * cycle)
    elif name == "Land":
        crouch = math.sin(math.pi * p)
        r["Thigh_L"] = r["Thigh_R"] = axis_angle(X, 55 * crouch)
        r["Shin_L"] = r["Shin_R"] = axis_angle(X, -90 * crouch)
        r["Spine"] = axis_angle(X, 20 * crouch)
    elif name in ("Aim", "Fire"):
        kick = math.sin(math.pi * p) if name == "Fire" else 0.0
        r["UpperArm_R"] = qmul(axis_angle(Y, -90), axis_angle(X, -10 * kick))
        r["Forearm_R"] = axis_angle(Y, -15 * kick)
        r["UpperArm_L"] = axis_angle(Y, 60)
        r["Forearm_L"] = axis_angle(Y, 40)
        r["Head"] = axis_angle(Y, -10)
    elif name == "Reload":
        lift = math.sin(math.pi * p)
        r["UpperArm_R"] = axis_angle(Y, -60)
        r["UpperArm_L"] = axis_angle(Y, 30 + 50 * lift)
        r["Forearm_L"] = axis_angle(Y, 30 + 60 * lift)
        r["Head"] = axis_angle(X, 15 * lift)
    elif name == "Hit":
        jolt = math.sin(math.pi * p)
        r["Spine"] = axis_angle(X, -18 * jolt)
        r["Head"] = axis_angle(X, -25 * jolt)
    elif name == "Death":
        fall = min(1.0, p * 1.4)
        r["Hips"] = axis_angle(X, -85 * fall)
        r["Thigh_L"] = r["Thigh_R"] = axis_angle(X, 30 * fall)
        r["UpperArm_L"] = axis_angle(Z, 70 * fall)
        r["UpperArm_R"] = axis_angle(Z, -70 * fall)
        root = (0.0, 0.0, -0.4 * fall)
    elif name == "Interact":
        reach = math.sin(math.pi * p)
        r["UpperArm_R"] = axis_angle(Y, -80 * reach)
        r["Forearm_R"] = axis_angle(Y, -20 * reach)
        r["Head"] = axis_angle(X, 10 * reach)
    return r, root


# (name, seconds, loops in the graph)
CLIPS = [("Idle", 2.0), ("Walk", 1.0), ("Run", 0.6), ("Sprint", 0.5), ("Jump", 0.3), ("Fall", 0.5), ("Land", 0.25),
         ("Aim", 0.5), ("Fire", 0.2), ("Reload", 1.0), ("Hit", 0.35), ("Death", 1.2), ("Interact", 0.8)]


class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views, self.accessors = [], []

    def add(self, fmt, values, count, type_, component=5126, target=None, minmax=None):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        self.data += struct.pack("<" + fmt * (len(values) // len(fmt)), *values) if len(fmt) == 1 else struct.pack("<" + fmt, *values)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset}
        if target:
            view["target"] = target
        self.views.append(view)
        accessor = {"bufferView": len(self.views) - 1, "componentType": component, "count": count, "type": type_}
        if minmax:
            accessor["min"], accessor["max"] = minmax
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def model_positions(bones):
    """Rest model-space positions and rotations (parents come first)."""
    pos, rot = [], []
    for name, parent, translation, rest in bones:
        if parent < 0:
            pos.append(tuple(float(c) for c in translation))
            rot.append(rest)
        else:
            moved = rotate(rot[parent], translation)
            pos.append(tuple(pos[parent][i] + moved[i] for i in range(3)))
            rot.append(qmul(rot[parent], rest))
    return pos, rot


def inverse_bind(position, rotation):
    # inverse of (T * R): R^-1 * T^-1, column-major
    ix, iy, iz, iw = -rotation[0], -rotation[1], -rotation[2], rotation[3]
    inv = (ix, iy, iz, iw)
    cols = [rotate(inv, axis) for axis in (X, Y, Z)]
    t = rotate(inv, tuple(-c for c in position))
    return [cols[0][0], cols[0][1], cols[0][2], 0, cols[1][0], cols[1][1], cols[1][2], 0,
            cols[2][0], cols[2][1], cols[2][2], 0, t[0], t[1], t[2], 1]


def build_rig(bones, clips, mesh_name, generator):
    """A glTF document: the bones, one skinned box per bone and the clips (embedded buffer)."""
    buffer = Buffer()
    pos, rot = model_positions(bones)
    positions, normals, joints, weights, indices = [], [], [], [], []
    for b in range(len(bones)):
        cx, cy, cz = pos[b]
        h = 0.06
        base = len(positions) // 3
        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    positions += [cx + sx * h, cy + sy * h, cz + sz * h]
                    n = math.sqrt(3)
                    normals += [sx / n, sy / n, sz / n]
                    joints += [b, 0, 0, 0]
                    weights += [1.0, 0.0, 0.0, 0.0]
        for a, b2, c in ((0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1),
                         (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)):
            indices += [base + a, base + b2, base + c]
    count = len(positions) // 3
    lo = [min(positions[i::3]) for i in range(3)]
    hi = [max(positions[i::3]) for i in range(3)]
    a_pos = buffer.add("f", positions, count, "VEC3", target=34962, minmax=(lo, hi))
    a_nrm = buffer.add("f", normals, count, "VEC3", target=34962)
    a_jnt = buffer.add("H", joints, count, "VEC4", component=5123, target=34962)
    a_wgt = buffer.add("f", weights, count, "VEC4", target=34962)
    a_idx = buffer.add("H", indices, len(indices), "SCALAR", component=5123, target=34963)
    ibms = []
    for b in range(len(bones)):
        ibms += inverse_bind(pos[b], rot[b])
    a_ibm = buffer.add("f", ibms, len(bones), "MAT4")

    nodes = []
    for b, (name, parent, translation, rest) in enumerate(bones):
        node = {"name": name, "translation": [float(c) for c in translation]}
        if rest != IDENTITY:
            node["rotation"] = [round(c, 7) for c in rest]
        children = [i for i, other in enumerate(bones) if other[1] == b]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({"name": mesh_name, "mesh": 0, "skin": 0})

    animations = []
    for clip, duration in clips:
        frames = max(1, round(duration * FPS))
        times = [duration * f / frames for f in range(frames + 1)]
        a_time = buffer.add("f", times, len(times), "SCALAR", minmax=([0.0], [duration]))
        samplers, channels = [], []
        samples = [clip_pose(clip, t, duration) for t in times]
        for b, (name, parent, translation, rest) in enumerate(bones):
            source = generator(name)
            if source is None:
                continue
            values = []
            animated = False
            for rotations, root in samples:
                q = rotations.get(source, IDENTITY)
                animated = animated or q != IDENTITY
                values += list(qmul(rest, q))
            if animated:
                samplers.append({"input": a_time, "output": buffer.add("f", values, len(times), "VEC4"), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": b, "path": "rotation"}})
            if source == "Root" and any(root != (0.0, 0.0, 0.0) for _, root in samples):
                values = []
                for _, root in samples:
                    values += [translation[i] + root[i] for i in range(3)]
                samplers.append({"input": a_time, "output": buffer.add("f", values, len(times), "VEC3"), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": b, "path": "translation"}})
        animations.append({"name": clip, "samplers": samplers, "channels": channels})

    return {
        "asset": {"version": "2.0", "generator": "tools/generate_aether01_kael_rig.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": mesh_name, "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
                                                       "indices": a_idx, "material": 0}]}],
        "materials": [{"name": "RigGrey", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.6, 0.65, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.8}}],
        "skins": [{"joints": list(range(len(bones))), "inverseBindMatrices": a_ibm, "skeleton": 0}],
        "animations": animations,
        "buffers": [{"byteLength": len(buffer.data), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer.data)).decode("ascii")}],
        "bufferViews": buffer.views,
        "accessors": buffer.accessors,
    }


# --- The animation graph --------------------------------------------------------------------
LOOPING = {"Idle", "Walk", "Run", "Sprint", "Fall", "Aim"}
GROUND = ["Idle", "Walk", "Run", "Sprint", "Aim"]


def cond(variable, op, value=None):
    c = {"variable": variable, "op": op}
    if value is not None:
        c["value"] = value
    return c


def graph():
    names = [name for name, _ in CLIPS]
    nodes = [{"id": i + 1, "kind": "Clip", "asset": name, **({} if name in LOOPING else {"loop": False})} for i, name in enumerate(names)]
    machine_node, slot_node = 20, 30
    nodes.append({"id": machine_node, "kind": "StateMachine", "machine": "Locomotion"})
    nodes.append({"id": slot_node, "kind": "Slot", "inputs": [machine_node], "slot": "Default"})
    alive = cond("Dead", "false")
    t = []

    def add(src, dst, conditions, blend=0.15, priority=0, finished=False):
        item = {"from": src, "to": dst, "blend_time": blend, "conditions": conditions}
        if finished:
            item["when_finished"] = True
        if priority:
            item["priority"] = priority
        t.append(item)

    add("*", "Death", [cond("Dead", "true")], 0.15, 100)
    add("Death", "Idle", [alive], 0.2)
    add("*", "Hit", [cond("Hit", "triggered"), alive], 0.05, 50)
    add("*", "Jump", [cond("Jump", "triggered"), alive], 0.05, 40)
    for state in GROUND:
        add(state, "Fall", [cond("Grounded", "false"), alive], 0.15, 30)
    add("*", "Reload", [cond("Reload", "triggered"), cond("Grounded", "true"), alive], 0.1, 20)
    add("*", "Interact", [cond("Interact", "triggered"), cond("Grounded", "true"), alive], 0.1, 15)
    add("*", "Fire", [cond("Fire", "triggered"), alive], 0.05, 10)
    add("Jump", "Fall", [], 0.1, finished=True)
    add("Fall", "Land", [cond("Grounded", "true")], 0.05)
    for state in ("Land", "Fire", "Reload", "Hit", "Interact"):
        add(state, "Idle", [], 0.1, finished=True)
    add("Idle", "Walk", [cond("Speed", ">", 0.5)])
    add("Idle", "Aim", [cond("Aiming", "true"), cond("Speed", "<=", 0.5)], 0.1)
    add("Aim", "Idle", [cond("Aiming", "false")], 0.1)
    add("Aim", "Walk", [cond("Speed", ">", 0.5)])
    add("Walk", "Idle", [cond("Speed", "<=", 0.5)], 0.2)
    add("Walk", "Run", [cond("Speed", ">", 3.5)], 0.2)
    add("Run", "Walk", [cond("Speed", "<=", 3.5)], 0.2)
    add("Run", "Sprint", [cond("Speed", ">", 7.0)], 0.2)
    add("Sprint", "Run", [cond("Speed", "<=", 7.0)], 0.2)
    states = [{"name": name, "pose": i + 1} for i, name in enumerate(names)]
    variables = [{"name": "Speed", "type": "float", "default": 0.0}, {"name": "Grounded", "type": "bool", "default": 1.0},
                 {"name": "Aiming", "type": "bool", "default": 0.0}, {"name": "Dead", "type": "bool", "default": 0.0}]
    variables += [{"name": n, "type": "trigger", "default": 0.0} for n in ("Jump", "Fire", "Reload", "Hit", "Interact")]
    return {"$type": "AnimGraph", "$version": 1, "variables": variables, "output": slot_node, "nodes": nodes,
            "machines": [{"name": "Locomotion", "entry": "Idle", "states": states, "transitions": t}]}


# --- The retarget target: other names, proportions (x1.35) and an A-pose rest -----------------
WARDEN_NAMES = {"Root": "warden:base", "Hips": "warden:pelvis", "Spine": "warden:spine_01", "Chest": "warden:spine_02",
                "Head": "warden:skull", "UpperArm_L": "warden:arm_upper_l", "Forearm_L": "warden:arm_lower_l",
                "UpperArm_R": "warden:arm_upper_r", "Forearm_R": "warden:arm_lower_r", "Thigh_L": "warden:leg_upper_l",
                "Shin_L": "warden:leg_lower_l", "Thigh_R": "warden:leg_upper_r", "Shin_R": "warden:leg_lower_r"}


def generate():
    kael_bones = [(name, parent, translation, IDENTITY) for name, parent, translation in KAEL_BONES]
    rig = build_rig(kael_bones, CLIPS, "KaelRig", lambda name: name)
    path = CONTENT / "Characters/Kael/Kael_Rig.gltf"
    write_json(path, rig)
    meta(path, "Model")

    path = CONTENT / "Characters/Kael/Kael.aanim"
    write_json(path, graph())
    meta(path, "AnimGraph")

    path = CONTENT / "Characters/Kael/Kael_ArchiveListen.amontage"
    write_json(path, {"$type": "Montage", "$version": 1, "name": "Kael_ArchiveListen", "clip": "Interact", "slot": "Default",
                      "blend_in": 0.2, "blend_out": 0.3, "sections": []})
    meta(path, "AnimMontage")

    scale = 1.35
    warden = []
    for name, parent, translation in KAEL_BONES:
        rest = IDENTITY
        if name == "UpperArm_L":
            rest = axis_angle(Z, -40)  # A-pose: the arms hang 40 degrees down
        elif name == "UpperArm_R":
            rest = axis_angle(Z, 40)
        moved = tuple(c * scale for c in translation)
        warden.append((WARDEN_NAMES[name], parent, moved, rest))
    warden.append(("warden:jaw", 4, (0, 0.05 * scale, 0.08 * scale), IDENTITY))  # a bone Kael has no counterpart for
    rig = build_rig(warden, [], "WardenRig", lambda name: None)
    rig.pop("animations")
    path = CONTENT / "Characters/Warden/Warden_Rig.gltf"
    write_json(path, rig)
    meta(path, "Model")


if __name__ == "__main__":
    generate()
