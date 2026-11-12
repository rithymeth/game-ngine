"""The layout of Ashen Frontier's world as data: four compact regions around a central outpost, joined by passes.

This is the one place that says where things are. tools/make_scenes.py turns it into the OpenWorld scene; make_world_data() writes it to
Content/Data/world.json for the game (the map screen, fast travel, the mission markers) to read; nothing else repeats a coordinate.

The world is 460 x 460 m, x east and z south. Each region is 200 x 200 m with 4 to 6 authored points of interest (the design document asks for
compact regions of dense discoveries, not an empty planet). The centre is the outpost's crossroads; ridges separate the regions, with a 30 m
pass through each at the middle of its edge, so travel between regions is a walk, and a place the game can later gate or stream.
"""

HALF = 230.0          # the world's half-size
REGION_HALF = 100.0   # each region's half-size
PASS_WIDTH = 30.0

REGIONS = {
    "fallen_basin":  {"name": "Fallen Basin",  "centre": (-115.0, 115.0),  "ground": (0.55, 0.30, 0.18), "summary": "Crashed freighter, desert gullies, abandoned homesteads. Tutorial, salvage, the first robot."},
    "iron_wastes":   {"name": "Iron Wastes",   "centre": (115.0, 115.0),   "ground": (0.30, 0.27, 0.26), "summary": "Smelters, gantries, rail yards, buried assembly lines. Combat, traversal, production schematics."},
    "verdant_array": {"name": "Verdant Array", "centre": (-115.0, -115.0), "ground": (0.34, 0.40, 0.22), "summary": "Greenhouses, living domes, maintenance drones. Research, puzzles, ecological diplomacy."},
    "hollow_core":   {"name": "Hollow Core",   "centre": (115.0, -115.0),  "ground": (0.15, 0.14, 0.17), "summary": "The reactor plateau and the way down to the data network. Late-game exploration and the finale."},
}

# Points of interest: id, region, name, type, offset from the region's centre (x, z). Types are for the map's icons and the missions' markers.
POIS = [
    # Fallen Basin
    ("crash_site",       "fallen_basin", "Crash Site",          "landmark", (-20, -30)),
    ("mule3_wreck",      "fallen_basin", "MULE-3 Wreck",        "machine",  (10, -12)),
    ("homestead_ruins",  "fallen_basin", "Homestead Ruins",     "ruin",     (-45, -55)),
    ("salvage_field",    "fallen_basin", "Salvage Field",       "salvage",  (35, 10)),
    ("distress_relay",   "fallen_basin", "Distress Relay",      "relay",    (50, -60)),
    ("storm_field",      "fallen_basin", "Storm Field",         "hazard",   (30, 55)),
    # Iron Wastes
    ("smelter_row",      "iron_wastes",  "Smelter Row",         "landmark", (-40, -30)),
    ("rail_yard",        "iron_wastes",  "Rail Yard",           "salvage",  (20, 50)),
    ("gantry_works",     "iron_wastes",  "Gantry Works",        "landmark", (50, -40)),
    ("assembly_hall",    "iron_wastes",  "Buried Assembly Hall", "dungeon", (-50, 40)),
    ("warden_checkpoint", "iron_wastes", "Warden Checkpoint",   "faction",  (0, 0)),
    ("slag_pits",        "iron_wastes",  "Slag Pits",           "hazard",   (45, 35)),
    # Verdant Array
    ("greenhouse_rows",  "verdant_array", "Greenhouse Rows",    "landmark", (-35, -30)),
    ("living_dome",      "verdant_array", "Living Dome",        "dungeon",  (0, 40)),
    ("maintenance_depot", "verdant_array", "Maintenance Depot", "machine",  (45, -35)),
    ("repaired_garden",  "verdant_array", "Repaired Garden",    "faction",  (-50, 40)),
    ("research_annex",   "verdant_array", "Research Annex",     "relay",    (30, 5)),
    # Hollow Core
    ("core_approach",    "hollow_core",  "Core Approach",       "landmark", (0, 98)),
    ("reactor_tower",    "hollow_core",  "Reactor Tower",       "landmark", (0, -5)),
    ("data_spine",       "hollow_core",  "Data Spine",          "relay",    (-45, -10)),
    ("core_gate",        "hollow_core",  "Core Gate",           "dungeon",  (0, -60)),
    ("failing_grid",     "hollow_core",  "Failing Grid",        "hazard",   (45, 30)),
]

OUTPOST = {"id": "outpost", "name": "Outpost", "position": (0.0, 0.0)}
SPAWN = {"position": (-107.0, 0.05, 133.0), "yaw": 200}  # beside the wreck, in the Fallen Basin


def poi_position(poi_id):
    for pid, region, _name, _kind, (dx, dz) in POIS:
        if pid == poi_id:
            cx, cz = REGIONS[region]["centre"]
            return (cx + dx, cz + dz)
    raise KeyError(poi_id)


def region_at(x, z):
    """The region a point is in, or None in a pass or the hub."""
    for rid, r in REGIONS.items():
        cx, cz = r["centre"]
        if abs(x - cx) <= REGION_HALF and abs(z - cz) <= REGION_HALF:
            return rid
    return None


def world_data():
    """The layout as the game reads it."""
    return {
        "$type": "WorldLayout", "$version": 1,
        "bounds": {"min": [-HALF, -HALF], "max": [HALF, HALF]},
        "outpost": {"id": OUTPOST["id"], "name": OUTPOST["name"], "position": list(OUTPOST["position"])},
        "spawn": {"position": list(SPAWN["position"]), "yaw": SPAWN["yaw"]},
        "regions": [{"id": rid, "name": r["name"], "centre": list(r["centre"]), "half": REGION_HALF, "summary": r["summary"]} for rid, r in REGIONS.items()],
        "pois": [{"id": pid, "region": region, "name": name, "type": kind, "position": list(poi_position(pid))} for pid, region, name, kind, _off in POIS],
        "passes": [
            {"between": ["fallen_basin", "iron_wastes"], "position": [0, 115], "width": PASS_WIDTH},
            {"between": ["verdant_array", "hollow_core"], "position": [0, -115], "width": PASS_WIDTH},
            {"between": ["fallen_basin", "verdant_array"], "position": [-115, 0], "width": PASS_WIDTH},
            {"between": ["iron_wastes", "hollow_core"], "position": [115, 0], "width": PASS_WIDTH},
        ],
    }
