"""A small library for writing Ashen Frontier's scenes (.ascene JSON): entities with GUIDs, transforms, models, colliders,
lights and the camera. The scenes are generated (tools/make_scenes.py), so a layout is code and a change is a diff."""

import json
import pathlib
import math
import uuid

_counter = [0]


def guid(name):
    """A stable GUID per name, so regenerating a scene does not change every entity's identity."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "ashen-frontier/" + name))


def quat_from_euler(rx=0.0, ry=0.0, rz=0.0):
    """Quaternion (x, y, z, w) for degrees about X, then Y, then Z."""
    ax, ay, az = (math.radians(a) / 2 for a in (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay), math.cos(az), math.sin(az)
    # q = qz * qy * qx
    qx = sx * cy * cz - cx * sy * sz
    qy = cx * sy * cz + sx * cy * sz
    qz = cx * cy * sz - sx * sy * cz
    qw = cx * cy * cz + sx * sy * sz
    return [qx, qy, qz, qw]


def look_quat(yaw_deg, pitch_deg):
    """The orientation of something that looks along its own -Z after turning `yaw_deg` about Y and then tilting `pitch_deg` (down is negative)."""
    return quat_from_euler(pitch_deg, yaw_deg, 0)


class Scene:
    def __init__(self):
        self.entities = []
        self.ground = None   # a function (x, z) -> the ground's height: when set, snapped entities are placed on it

    def add(self, name, components, position=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), parent=None, tags=(), snap=False):
        comps = dict(components)
        if snap and self.ground is not None:
            position = (position[0], position[1] + float(self.ground(position[0], position[2])), position[2])
        comps["Transform"] = {"$v": 2, "position": list(position), "rotation": quat_from_euler(*rotation), "scale": list(scale)}
        comps["EntityName"] = {"$v": 1, "value": name}
        if tags:
            comps["Tags"] = {"$v": 1, "names": list(tags)}
        if parent:
            comps["Parent"] = {"$v": 1, "parent": parent}
        g = guid(name)
        self.entities.append({"components": comps, "guid": g})
        return g

    def model(self, name, asset, position=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), color=None, emissive=None, collider=None, tags=(), texture=None, snap=True):
        """A model, optionally recoloured (a MaterialParameters over every material of it) and given a box collider
        (`collider` = (centre, half extents) in the model's own space)."""
        comps = {"ModelRenderer": {"$v": 1, "asset_path": "Models/" + asset + ".glb"}}
        values = []
        if color is not None:
            values.append({"$v": 1, "is_texture": False, "name": "BaseColor", "texture": "", "value": list(color) + [1.0]})
        if emissive is not None:
            values.append({"$v": 1, "is_texture": False, "name": "Emissive", "texture": "", "value": list(emissive) + [1.0]})
        if texture is not None:
            values.append({"$v": 1, "is_texture": True, "name": "BaseColorTexture", "texture": texture, "value": [0, 0, 0, 0]})
        if values:
            comps["MaterialParameters"] = {"$v": 1, "material": "", "revision": 0, "values": values}
        if collider is not None:
            centre, half = collider
            comps["BoxCollider"] = {"$v": 1, "center": list(centre), "friction": 0.6, "half_extents": list(half), "is_trigger": False, "report_stay": False, "restitution": 0.0}
        return self.add(name, comps, position, rotation, scale, tags=tags, snap=snap)

    def sun(self, yaw=-35, pitch=-42, intensity=3.2, color=(1.0, 0.92, 0.80)):
        return self.add("Sun", {"DirectionalLight": {"$v": 1, "cast_shadows": True, "color": list(color), "intensity": intensity}}, rotation=(pitch, yaw, 0))

    def sky(self, intensity=1.0, color=(0.75, 0.82, 0.95)):
        return self.add("Sky", {"SkyLight": {"$v": 1, "color": list(color), "intensity": intensity}})

    def camera(self, position, yaw, pitch, fov=60.0, far=1500.0, name="MainCamera", ortho_height=None):
        """A camera; with ortho_height it looks straight along its direction without perspective (a map), that many metres tall."""
        comps = {"Camera": {"$v": 1, "far_plane": far, "fov_degrees": fov, "near_plane": 0.1, "ortho_height": ortho_height or 10.0, "priority": 0,
                            "projection": "Orthographic" if ortho_height else "Perspective"}}
        g = guid(name)
        comps["Transform"] = {"$v": 2, "position": list(position), "rotation": look_quat(yaw, pitch), "scale": [1, 1, 1]}
        comps["EntityName"] = {"$v": 1, "value": name}
        comps["Tags"] = {"$v": 1, "names": ["MainCamera"]}
        self.entities.append({"components": comps, "guid": g})
        return g

    def save(self, path):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"$type": "Scene", "$version": 1, "entities": self.entities}, f, indent=2)
            f.write("\n")


def script_guid(name):
    """The asset GUID of a script in Content/Scripts, from its .ameta."""
    meta = json.loads((pathlib.Path(__file__).resolve().parent.parent / "Content" / "Scripts" / (name + ".luau.ameta")).read_text(encoding="utf-8"))
    return meta["guid"]


def give_hands(scene, components=0, scrap=0):
    """Puts the Interactor script on the entity added last (Kael's body): what lets him take salvage. Adds the HUD (pack counts, the prompt) too."""
    scene.entities[-1]["components"]["ScriptComponent"] = {"$v": 1, "properties": ([{"name": "start_components", "value": str(components)}] if components else []) + ([{"name": "start_scrap", "value": str(scrap)}] if scrap else []), "script": script_guid("Interactor")}
    scene.add("Hud", {"WidgetComponent": {"$v": 1, "layout": "UI/Hud.aui", "z": 0, "visible": True}})


def mule(scene, name, position, rotation=(0, 0, 0), **kwargs):
    """MULE-3, broken, with its script: repaired in play, then it follows Kael. (No collider: it moves.)"""
    guid_ = scene.model(name, "MULE3", position=position, rotation=rotation, tags=("MULE3",), **kwargs)
    scene.entities[-1]["components"]["ScriptComponent"] = {"$v": 1, "properties": [], "script": script_guid("Mule3")}
    return guid_


def workbench(scene, name, position, rotation=(0, 0, 0), **kwargs):
    """The fab bench with its script: Interact beside it crafts the Generator Core."""
    guid_ = scene.model(name, "FabBench", position=position, rotation=rotation, tags=("Workbench",), **kwargs)
    scene.entities[-1]["components"]["ScriptComponent"] = {"$v": 1, "properties": [], "script": script_guid("Workbench")}
    return guid_


def generator(scene, name, position, rotation=(0, 0, 0), **kwargs):
    """The outpost generator with its script and a light that is dark until it is powered."""
    guid_ = scene.model(name, "Generator", position=position, rotation=rotation, tags=("Generator",), **kwargs)
    comps = scene.entities[-1]["components"]
    comps["ScriptComponent"] = {"$v": 1, "properties": [], "script": script_guid("PowerGenerator")}
    comps["PointLight"] = {"$v": 1, "cast_shadows": False, "color": [0.3, 0.7, 1.0], "intensity": 0.0, "range": 16.0}
    return guid_


def beacon(scene, name, asset, position, **kwargs):
    """A model with the Beacon script: reached when the player walks up to it (the distress relay)."""
    guid_ = scene.model(name, asset, position=position, tags=("Relay",), **kwargs)
    scene.entities[-1]["components"]["ScriptComponent"] = {"$v": 1, "properties": [], "script": script_guid("Beacon")}
    return guid_
