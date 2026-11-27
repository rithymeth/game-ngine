# Jump Envelope: timing R&D spike

Jump Proof answers, "Did one recorded route reach its target in this package?"
Jump Envelope asks, "How much earlier or later can I press this button, and
did the new package lose any of that margin?"

This experiment shifts one digital key's press and release together, leaving
its hold duration and all other inputs intact. It takes a bound Jump Proof
recording, runs integer offsets against an existing packaged player, and can
repeat the same sweep on a candidate package. Each trial gets a fresh user
directory. The original offset runs twice and must produce the same path
and outcome. Samples whose shifted events fall outside the route or hit a
neighboring event for the same key are skipped, not counted as misses.

## Try it

Capture a bound route as described in the 2D manual, then choose a digital
button and its zero-based press frame. Use only packages you trust; this
command executes each player.

~~~sh
python3 tools/jump_envelope.py \
    --player Before/MyGame --candidate After/MyGame \
    --route recorded.json --key Space --press-frame 4 \
    --radius 3 --output jump-envelope.json
~~~

Leave out the candidate for a single-package experiment. Offsets are fixed
simulation frames; at 60 Hz, one frame is about 16.7 ms. A contiguous window
of [-2, 2] means offsets -2, -1, 0, +1 and +2 passed, not that other jumps
or routes were verified. A null window means offset zero missed. The receipt
retains individual verdicts and final positions, plus lost/gained passing
offsets when comparing two packages. Exit 0 means the experiment completed,
not that the candidate preserved its timing margin. An error exits 2 and
does not publish a receipt.

## Prior art and what we would ship differently

- [Unity InputEventTrace](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.4/api/UnityEngine.InputSystem.LowLevel.InputEventTrace.html)
  records and replays input.
- [Unreal Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/running-gauntlet-tests-in-unreal-engine)
  runs automation on packaged builds.
- [Godot input injection](https://docs.godotengine.org/en/latest/tutorials/inputs/inputevent.html)
  is a building block for scripted input tests.
- [Celeste Frame Checker](https://gamebanana.com/mods/703842) already
  automates input-window experiments in a specific game.

The distinctive product direction is one engine-provided workflow for
capturing a route and comparing its timing margin across real packages
without writing a bespoke test for each project. This is not a claim that
timing sweeps themselves are new or that no third-party implementations exist.

## Evidence and next gate

Run python3 tools/ci/test_jump_envelope.py on POSIX. The test player has a
deliberately narrower candidate window. It exercises the CLI, process
invocation, independent profiles, status/landing checks and report handling;
it does not emulate Aether physics. Next, run the same experiment on two
actual packaged 2D starter builds and verify the output against their
traces. Before a production merge, also validate the desired platform(s).
Editor UI, CI thresholds, multi-input perturbations, player skill and general
level playability are outside the spike.
