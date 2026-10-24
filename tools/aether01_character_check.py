"""M8.6: play Kael's move set in the packaged player and check its animation state markers.

Replays the input trace the Aether01_KaelMoveSetPlaysInTheCookedGame test records
(AETHER01_MOVESET_TRACE: aim, pick up the rifle, fire, reload, jump, walk to a
run, then the Scout's hits, death and a restart) headless against a cooked
package, and checks the markers the player's animation host logs for Kael's
Animator ("Player: Anim state: Player Locomotion: <from> -> <to>"): every state
of the move set the game can reach appears, the run ends back in Idle after the
death, and nothing is reported missing (no animation warnings, errors or
script errors).

    python3 tools/aether01_character_check.py --player build/player/aether_player \\
        --pak Game.apak --trace aether01-moveset-trace.json --work out/
"""
import argparse
import pathlib
import re
import subprocess
import sys

# Sprint needs more than the character's 6 m/s run speed, so the game never reaches it.
EXPECTED = ["Idle", "Aim", "Interact", "Fire", "Reload", "Jump", "Fall", "Land", "Walk", "Run", "Hit", "Death"]
MARKER = re.compile(r"Player: Anim state: Player Locomotion: (\w+) -> (\w+)$")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--player", required=True)
    parser.add_argument("--pak", required=True)
    parser.add_argument("--trace", required=True)
    parser.add_argument("--work", required=True, help="a folder for user data and the log")
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([args.player, "--pak", args.pak, "--headless", "--report", "--replay-input", args.trace,
                             "--user-dir", str(work / "user")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    log = work / "character.log"
    log.write_text(result.stdout, encoding="utf-8")
    lines = result.stdout.splitlines()
    problems = []
    if result.returncode != 0:
        problems.append(f"the player exited with {result.returncode}")
    transitions = [m.groups() for m in (MARKER.search(line) for line in lines) if m]
    entered = [to for _, to in transitions]
    for state in EXPECTED:
        if state not in entered:
            problems.append(f"Kael never entered {state}")
    if not transitions or transitions[-1] != ("Death", "Idle"):
        problems.append(f"the run should end with Death -> Idle (the restart), not {transitions[-1] if transitions else 'nothing'}")
    # Each transition starts from the state the previous one entered.
    previous = "Idle"
    for source, target in transitions:
        if source != previous:
            problems.append(f"{source} -> {target} follows a transition into {previous}")
        previous = target
    errors = [line for line in lines if line.startswith("[ERROR]") or line.startswith("[FATAL]")]
    if errors:
        problems.append("errors in the log: " + "; ".join(errors[:5]))
    warnings = [line for line in lines if "Animation:" in line or "Script error" in line]
    if warnings:
        problems.append("animation or script problems: " + "; ".join(warnings[:5]))
    for p in problems:
        print(f"[character] {p}")
    print(f"[character] {'ok' if not problems else 'FAILED'}: {len(transitions)} transition(s), states {sorted(set(entered))} ({log})")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
