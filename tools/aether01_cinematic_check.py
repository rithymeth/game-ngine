"""M8.7: play the AETHER-01 archive reveal in the packaged player, to the end and with a skip.

Replays the input trace the Aether01_WardenPhasesEndingAndNewGame test records
(AETHER01_INPUT_TRACE), headless, against a cooked package, and checks the log
markers the player writes for sequence dialogue (cue start/stop, subtitle
show/hide, finished/skipped), Kael's archive montage and animation states (M8.6),
the archive signal particle effect (started, stopped, and died out after a skip)
and the final game state from --report. The skip
run is the same trace with the SkipCinematic key (Enter) pressed partway through
the reveal: it must stop the line that is playing, hide its subtitle, start no
later line and leave the game at the archive ending.

    python3 tools/aether01_cinematic_check.py --player build/player/aether_player \\
        --pak Game.apak --trace aether01-input-trace.json --work out/
"""
import argparse
import json
import pathlib
import subprocess
import sys

LINES = 3  # dialogue lines in Sequences/Archive.asequence
SKIP_AFTER_START = 95  # frames (60 Hz) into the reveal: during the first line (0.5 s .. 2.3 s)


def skip_trace(trace):
    """The trace with Enter tapped SKIP_AFTER_START frames after the last E press (which starts the reveal)."""
    events = trace["events"]
    starts = [e["frame"] for e in events if e["key"] == "E" and e["value"] > 0]
    if not starts:
        raise SystemExit("the trace has no E press to start the archive reveal")
    press = starts[-1] + SKIP_AFTER_START
    if press + 2 >= trace["frames"]:
        raise SystemExit("the trace ends before the skip point")
    events = events + [{"frame": press, "key": "Enter", "value": 1.0}, {"frame": press + 1, "key": "Enter", "value": 0.0}]
    events.sort(key=lambda e: e["frame"])  # the player reads them in frame order (stable for same-frame events)
    return {"frames": trace["frames"], "events": events}


def run(player, pak, trace_file, user_dir, log_file):
    result = subprocess.run([player, "--pak", pak, "--headless", "--report", "--replay-input", trace_file, "--user-dir", user_dir],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    pathlib.Path(log_file).write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout)
        raise SystemExit(f"the player exited with {result.returncode} ({log_file})")
    return result.stdout.splitlines()


def check(lines, skipped, log_file):
    problems = []
    def count(text):
        return sum(1 for line in lines if text in line)
    state = [line for line in lines if line.startswith("Player attributes: ")]
    if not state:
        problems.append("no final 'Player attributes' report")
        attributes = {}
    else:
        attributes = json.loads(state[-1][len("Player attributes: "):])
    errors = [line for line in lines if line.startswith("[ERROR]") or line.startswith("[FATAL]")]
    if errors: problems.append("errors in the log: " + "; ".join(errors[:5]))
    if count("Sequence audio cue missing"): problems.append("a dialogue cue is missing from the package")
    started, shown, hidden = count("Player: Sequence cue started: Audio/dialogue_archive_"), count("Player: Subtitle shown: dlg.archive."), count("Player: Subtitle hidden")
    if attributes.get("MissionStage") != 5 or attributes.get("ArchiveTime") != 8 or attributes.get("Health", 0) <= 0:
        problems.append(f"not at the archive ending: {attributes}")
    if skipped:
        if count("Player: Sequence skipped: Sequences/Archive.asequence") != 1: problems.append("no 'Sequence skipped' marker")
        if count("Player: Sequence finished: Sequences/Archive.asequence"): problems.append("the skipped reveal also finished normally")
        if started != 1 or shown != 1 or hidden != 1: problems.append(f"expected only the first line before the skip (started {started}, shown {shown}, hidden {hidden})")
        if count("Player: Sequence cue stopped: Audio/dialogue_archive_1.acue") != 1: problems.append("the skip didn't stop the playing line")
        if attributes.get("ArchiveComplete") != 2: problems.append(f"ArchiveComplete should be 2 (skipped): {attributes}")
    else:
        if count("Player: Sequence finished: Sequences/Archive.asequence") != 1: problems.append("no 'Sequence finished' marker")
        if count("Player: Sequence skipped"): problems.append("the full run was skipped")
        if started != LINES or shown != LINES or hidden != LINES: problems.append(f"expected {LINES} lines (started {started}, shown {shown}, hidden {hidden})")
        if attributes.get("ArchiveComplete") != 1: problems.append(f"ArchiveComplete should be 1 (played through): {attributes}")
    # M8.6/M8.7: the reveal's Animation track plays Kael's listening montage at 0.5 s and 5 s (the skip comes
    # before the second), and the playthrough drives his Animator's states from gameplay.
    montages = count("Player: Anim montage started: Player Kael_ArchiveListen")
    if montages != (1 if skipped else 2): problems.append(f"expected {1 if skipped else 2} Kael_ArchiveListen montage start(s), saw {montages}")
    if count("Player: Anim montage ended: Player Kael_ArchiveListen") != montages: problems.append("a Kael montage never ended")
    for state in ("Walk", "Run", "Fire", "Interact"):
        if not count(f"Player: Anim state: Player Locomotion: ") or not count(f" -> {state}"): problems.append(f"Kael never entered {state}")
    if count("Animation:"): problems.append("animation problems in the log")
    # M8.7: the reveal's Vfx track starts the archive signal particles on the terminal at 0.5 s and stops them
    # at 7 s (or at the skip); the player's particle world runs them until they die out.
    fx = "Fx/ArchiveSignal.avfx"
    if count(f"Player: Sequence vfx started: {fx}") != 1: problems.append(f"expected one '{fx}' start, saw {count(f'Player: Sequence vfx started: {fx}')}")
    if count(f"Player: Sequence vfx stopped: {fx}") != 1: problems.append(f"the {fx} effect was never stopped")
    # The recorded trace ends with the reveal (1 s after the stop, inside the motes' 1.2-1.8 s life), so only the
    # skip run plays on long enough to see them die out.
    if skipped and count(f"Player: Vfx finished: {fx}") != 1: problems.append(f"the {fx} particles never died out")
    if count("Particles:") or count("didn't start"): problems.append("particle problems in the log")
    # Each line's subtitle shows on the frame its voice starts: the two markers are adjacent.
    for index, line in enumerate(lines):
        if "Player: Subtitle shown: dlg.archive." in line:
            n = line.rsplit(".", 1)[-1]
            following = lines[index + 1] if index + 1 < len(lines) else ""
            if f"Sequence cue started: Audio/dialogue_archive_{n}.acue" not in following:
                problems.append(f"subtitle {n} and its voice didn't start together")
    mode = "skip" if skipped else "full"
    for p in problems: print(f"[{mode}] {p}")
    print(f"[{mode}] {'ok' if not problems else 'FAILED'}: {started} line(s) voiced, {shown} subtitle(s), state {attributes} ({log_file})")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--player", required=True)
    parser.add_argument("--pak", required=True)
    parser.add_argument("--trace", required=True)
    parser.add_argument("--work", required=True, help="a folder for the skip trace, user data and logs")
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    trace = json.loads(pathlib.Path(args.trace).read_text(encoding="utf-8"))
    skip_file = work / "skip-trace.json"
    skip_file.write_text(json.dumps(skip_trace(trace)), encoding="utf-8")
    ok = True
    for mode, trace_file in (("full", args.trace), ("skip", str(skip_file))):
        log = work / f"cinematic-{mode}.log"
        ok = check(run(args.player, args.pak, trace_file, str(work / f"user-{mode}"), log), mode == "skip", log) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
