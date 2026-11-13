#!/usr/bin/env python3
"""Writes Ashen Frontier's placeholder sound effects into Content/Audio: short synthesized tones and noise bursts (stdlib only), each a .wav and a
.acue cue the scripts play with Audio.PlaySound2D("Audio/<name>.acue"). They are temporary, to be replaced by real recordings without touching a script.

    python tools/make_audio.py
"""

import array
import json
import math
import pathlib
import random
import sys
import uuid
import wave

CONTENT = pathlib.Path(__file__).resolve().parent.parent / "Content"
RATE = 48000


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def meta(path, importer):
    relative = path.relative_to(CONTENT).as_posix()
    write_json(pathlib.Path(str(path) + ".ameta"), {"$type": "AssetMeta", "$v": 1, "guid": str(uuid.uuid5(uuid.NAMESPACE_URL, "ashen-frontier/audio/" + relative)),
                                                     "importer": importer, "importer_version": 1, "settings": {}, "labels": ["Prototype"], "source_hash": ""})


def env(t, duration, attack=0.005, release=0.05):
    return min(1.0, t / attack if attack > 0 else 1.0, (duration - t) / release if release > 0 else 1.0)


# name -> (duration, volume_db, function(t, noise, sine) -> sample in about [-1, 1])
SOUNDS = {
    "pickup": (0.18, -8, lambda t, n, s: 0.5 * s(660 if t < 0.08 else 990) * math.exp(-9 * (t % 0.09))),
    "craft": (0.45, -6, lambda t, n, s: (0.45 * s(220) + 0.3 * n) * math.exp(-30 * t) + 0.35 * s(880) * math.exp(-8 * max(0, t - 0.12)) * (t > 0.12)),
    "power_up": (1.3, -5, lambda t, n, s: 0.4 * s(70 + 330 * (t / 1.3) ** 2 * 1.0) * min(1, t * 6) * (0.5 + 0.5 * (t / 1.3))),
    "repair": (0.7, -7, lambda t, n, s: 0.45 * s((440, 554, 659)[min(2, int(t / 0.2))]) * math.exp(-6 * (t % 0.2))),
    "relay": (0.9, -8, lambda t, n, s: 0.35 * s(1200 if int(t * 12) % 2 == 0 else 900) * (1 if (t * 12) % 1 < 0.6 else 0) * math.exp(-1.2 * t)),
    "door": (0.55, -10, lambda t, n, s: 0.4 * n * math.sin(math.pi * t / 0.55) ** 2 + 0.2 * s(60)),
    "swing": (0.16, -12, lambda t, n, s: 0.5 * n * math.sin(math.pi * t / 0.16)),
    "hit_enemy": (0.22, -5, lambda t, n, s: (0.5 * n + 0.5 * s(95)) * math.exp(-22 * t)),
    "scan": (0.55, -9, lambda t, n, s: 0.4 * s(300 + 1100 * (t / 0.55)) * math.sin(math.pi * t / 0.55) + 0.1 * s(2400) * math.exp(-8 * t)),
    "hurt": (0.28, -4, lambda t, n, s: (0.55 * s(130 - 150 * t) + 0.25 * n) * math.exp(-12 * t)),
    "sentinel_down": (0.8, -5, lambda t, n, s: (0.4 * s(300 * math.exp(-2.5 * t)) + 0.15 * n) * math.exp(-3.5 * t)),
}


def main():
    out = CONTENT / "Audio"
    out.mkdir(parents=True, exist_ok=True)
    for name, (duration, volume_db, fn) in SOUNDS.items():
        rng = random.Random(name)
        samples = array.array("h")
        for frame in range(round(RATE * duration)):
            t = frame / RATE
            value = fn(t, rng.uniform(-1, 1), lambda hz: math.sin(2 * math.pi * hz * t)) * env(t, duration)
            samples.append(round(max(-1.0, min(1.0, value)) * 32767))
        if sys.byteorder != "little":
            samples.byteswap()
        wav_path = out / f"{name}.wav"
        with wave.open(str(wav_path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(samples.tobytes())
        meta(wav_path, "Sound")
        cue = out / f"{name}.acue"
        write_json(cue, {"version": 1, "name": name, "root": 1, "output": {"bus": "SFX", "volume_db": volume_db, "spatial": False},
                         "nodes": [{"id": 1, "type": "Wave", "sound": f"Audio/{name}.wav", "looping": False}]})
        meta(cue, "SoundCue")
    print("wrote", len(SOUNDS), "sounds to", out)


if __name__ == "__main__":
    main()
