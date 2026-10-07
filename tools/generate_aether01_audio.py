"""Regenerate original, temporary AETHER-01 sound cues using only the stdlib."""
from pathlib import Path
import array
import json
import math
import random
import sys
import uuid
import wave

CONTENT = Path(__file__).resolve().parents[1] / "games/AETHER-01/Content"
RATE = 48000


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def meta(path, importer):
    relative = path.relative_to(CONTENT).as_posix()
    write_json(Path(str(path) + ".ameta"), {"$type": "AssetMeta", "$v": 1,
        "guid": str(uuid.uuid5(uuid.NAMESPACE_URL, "aether01/prototype/" + relative)),
        "importer": importer, "importer_version": 1, "settings": {}, "labels": ["Prototype"], "source_hash": ""})


def generate():
    output = CONTENT / "Audio"
    output.mkdir(parents=True, exist_ok=True)
    durations = {"rifle": .18, "pistol": .14, "shotgun": .45, "reload": .35,
                 "hurt": .25, "warning": .6, "ambient": 2, "archive": 2}
    for name, duration in durations.items():
        rng = random.Random(name)
        samples = array.array("h")
        for frame in range(round(RATE * duration)):
            t = frame / RATE
            noise = rng.uniform(-1, 1)
            sine = lambda hz: math.sin(2 * math.pi * hz * t)
            if name == "rifle": value = (.55 * noise + .3 * sine(130)) * math.exp(-35 * t)
            elif name == "pistol": value = (.25 * noise + .5 * sine(900 - 2200 * t)) * math.exp(-35 * t)
            elif name == "shotgun": value = (.55 * noise + .35 * sine(70)) * math.exp(-14 * t)
            elif name == "reload": value = (.35 * sine(1100) + .25 * noise) * (math.exp(-80 * t) + math.exp(-80 * abs(t - .2)))
            elif name == "hurt": value = (.5 * sine(120) + .2 * noise) * math.exp(-18 * t)
            elif name == "warning": value = .3 * sine(660 if int(t * 10) % 2 == 0 else 880) * min(1, t * 50, (duration - t) * 30)
            elif name == "ambient": value = .025 * (sine(45) + .4 * sine(90))
            else: value = .16 * (sine(220) + sine(330) + sine(440)) * min(1, t * 20) * math.exp(-1.5 * t)
            samples.append(round(max(-1, min(1, value)) * 32767))
        if sys.byteorder != "little": samples.byteswap()
        path = output / f"{name}.wav"
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(samples.tobytes())
        meta(path, "Sound")
        cue = output / f"{name}.acue"
        write_json(cue, {"version": 1, "name": name, "root": 1,
            "output": {"bus": "Music" if name == "ambient" else "SFX", "volume_db": -6, "spatial": False},
            "nodes": [{"id": 1, "type": "Wave", "sound": f"Audio/{name}.wav", "looping": name == "ambient"}]})
        meta(cue, "SoundCue")


if __name__ == "__main__":
    generate()
