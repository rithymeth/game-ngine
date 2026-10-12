"""Level layout for STARDUST HOP (units are metres; -Z is forward; y is up)."""

# (name, x, z, top_y, size_x, size_z)
PLATFORMS = [
    ("Start",   0.0,   0.0, 0.0, 8, 8),
    ("P1",      0.0,  -8.0, 0.6, 4, 4),
    ("P2",      3.5, -12.5, 1.2, 4, 4),
    ("P3",      3.5, -17.5, 1.8, 4, 4),
    ("P4",     -0.5, -21.0, 2.4, 4, 4),
    ("P5",     -5.0, -23.5, 3.0, 4, 4),
    ("P6",     -9.0, -27.0, 3.6, 4, 4),
    ("P7",     -9.0, -32.0, 4.2, 6, 6),   # checkpoint
    ("P8",     -4.0, -35.0, 4.8, 4, 4),
    ("P9",      1.0, -37.0, 5.4, 4, 4),
    ("P10",     6.0, -39.0, 6.0, 4, 4),
    ("P11",    11.0, -41.0, 6.6, 4, 4),
    ("P12",    14.0, -46.0, 7.2, 4, 4),
    ("Goal",   14.0, -53.0, 7.8, 8, 8),
]

CHECKPOINT = "P7"
GOAL = "Goal"
KILL_Y_BELOW_LOWEST = 14.0   # fall this far under the start platform and you respawn
PLATFORM_THICKNESS = 1.0


def platform_model(sx, sz):
    return f"models/plat_{sx}x{sz}.gltf"


def coin_positions():
    """One coin hovering over each of P1..P12, plus a few extra on the long jumps."""
    coins = []
    by_name = {p[0]: p for p in PLATFORMS}
    for i in range(1, 13):
        _, x, z, top, sx, sz = by_name[f"P{i}"]
        coins.append((x, top + 1.1, z))
    # bonus coins above the gaps
    coins.append((0.0, 2.4, -3.8))
    coins.append((-7.0, 5.6, -29.5))
    coins.append((8.5, 8.6, -43.5))
    return coins
