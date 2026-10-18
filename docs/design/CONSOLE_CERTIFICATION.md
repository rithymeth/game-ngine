# Console certification checklist

This checklist records engine-side release evidence that can be gathered
without publishing first-party certification requirements. Each licensed
console integration must add its current SDK, program and title-specific gates
from the vendor's private documentation before submission. A passing checklist
here does not mean the title is certified or that the platform is supported.

## Build and plugin integration

- [ ] Record the console SDK version, compiler/toolchain version, target
  architecture and build configuration used for the candidate.
- [ ] Confirm the platform plugin is linked into the player and registers once.
- [ ] Confirm plugin startup initializes required services before the game
  loads and shutdown releases callbacks, devices, storage handles and SDK
  resources after the game stops.
- [ ] Check that startup failure and partial initialization unwind cleanly.
- [ ] Record warnings, validation-layer output and known platform-specific
  exceptions.

## Save data and user storage

- [ ] Confirm `user_data_dir` resolves to the title's approved persistent
  storage container. Check that command-line and `AETHER_USER_DIR` overrides
  take precedence only in development or test builds where allowed.
- [ ] Verify save slots, settings and crash reports stay within the permitted
  title and user boundaries; test two users and two titles for isolation.
- [ ] Exercise first launch, normal save/load, full storage, quota exhaustion,
  read-only storage, missing storage and corrupted save recovery.
- [ ] Interrupt the process during a save and confirm the previous valid save
  remains recoverable. Record the storage API's atomicity and flush guarantees.
- [ ] Verify account switching, sign-out and any cloud-save conflict flow when
  the title uses those services.
- [ ] Confirm user deletion and platform storage cleanup remove or retain data
  according to the applicable policy.

## Suspend, resume and shutdown

- [ ] Deliver native suspend and resume callbacks to
  `PlatformPlugins::Lifecycle()` on the main thread.
- [ ] Suspend during gameplay, while paused, during a scene transition and
  while a save is pending. Confirm the game stops advancing, audio stops and
  queued saves flush within the platform's allowed background time.
- [ ] Resume after short and long suspends. Confirm audio is restored, input
  focus is refreshed, and the first simulation tick does not include time
  spent suspended.
- [ ] Test repeated and overlapping suspend/resume notifications, controller
  disconnect/reconnect, and system UI or account overlays.
- [ ] Force termination during suspend and relaunch. Confirm startup handles
  interrupted state without hanging or losing the last valid save.
- [ ] Deliver moderate and critical memory-pressure events; record memory
  released, frame-time recovery and any content that must be reloaded.

## Runtime and player experience

- [ ] Cold boot to the first playable screen and exit through the platform's
  required user flow.
- [ ] Check controller focus, reconnect, player assignment, text entry and
  navigation through startup, pause, settings and error dialogs.
- [ ] Check graphics, audio, display changes and performance at the platform's
  required modes and supported hardware configurations.
- [ ] Exercise offline startup, network loss and entitlement/service failures
  for every platform service the title uses.
- [ ] Review accessibility, privacy, parental-control and user-generated
  content requirements using the current first-party checklist.

## Evidence record

For every run, attach the build identifier, test date, console and OS version,
test account class, steps exercised, pass/fail result, logs or capture, and
linked issue for each failure. Keep first-party checklist IDs and submission
results in the licensed team's approved private records; do not copy
confidential certification text into this repository.

| Field | Value |
|---|---|
| Platform / title | |
| SDK and toolchain | |
| Candidate build | |
| Test date and owner | |
| First-party checklist revision | |
| Open blockers | |
| Submission result | Not submitted |
