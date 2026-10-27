# Ghost Shift — Aether Engine M0 integration and upgrade plan

Source: project owner's Ghost Shift GDD, October 2026 (uploaded DOCX; document text identifies version 1.0). This is an engine-facing execution plan, not a claim that any feature has passed testing.

## Product contract
- Windows x64 first; stylized 3D first-person cooperative cleaning/comedy horror.
- 1–4 players: solo offline plus host-and-join over the internet.
- Handcrafted Willow House is the first playable; 15-minute representative contract.
- Host authority for ghost AI, interactions, objectives, physics outcomes and rewards; clients submit requests and predict local movement.
- Candidate network targets to **measure**, not guarantees: 30 Hz host simulation, ~20 Hz relevant replication, 60 FPS client, 100–150 ms RTT.
- No launch requirement for host migration, native voice, fully procedural buildings or separately hosted dedicated servers.

## Team manifest
Mode: standard; manager owns integration. Roles: network/gameplay engineer, physics/AI engineer, test strategist, security reviewer, release reviewer. Roles are assignments only until actual work and evidence exist.
Objective: deliver a reliable M0 two-remote-client test and then a complete Willow House contract without breaking AETHER-01 or other existing engine consumers.
Approval checkpoint: choose relay/NAT service, credentials/cost, and any deployment before implementation.
Stop conditions: existing production checks regress; state authority or persistence integrity cannot be demonstrated.

## Work ledger (ordered by dependency)
| ID | Owner | Engine change / integration | Acceptance evidence | Status |
|---|---|---|---|---|
| GS-00 | Architect | Inventory current multiplayer/RPC, Jolt, AI/Nav, editor, audio and packaging APIs; map reuse vs gaps | Code references and tests on current master | done — [GHOST_SHIFT_INVENTORY.md](GHOST_SHIFT_INVENTORY.md) |
| GS-01 | Network | Host-and-join session lifecycle, lobby, timeout and reconnect test harness | Two remote clients join one host; logs include session IDs | queued |
| GS-02 | Network/security | Authority checks for tool requests, ownership and rate limiting | Malformed/duplicate/late RPCs rejected without state corruption | queued |
| GS-03 | Gameplay | Replicated contamination state with idempotent vacuum action | All peers agree on removed residue and battery/tank usage | queued |
| GS-04 | Physics | Host-owned constrained furniture/door/tether interactions | Two players interact with same prop under packet loss without divergence | queued |
| GS-05 | AI | Prankster state machine and haunting-intensity director | Seeded headless scenario; warnings and counterplay verified | queued |
| GS-06 | Gameplay | Objective transitions, containment and extraction | Contract reaches exactly one authoritative result | queued |
| GS-07 | Persistence | Per-player reward validation and company save | Reconnect and restart do not duplicate payout | queued |
| GS-08 | Rendering/audio | Readable stylized lighting, UV feedback, spatial threat cues and subtitles | Windows packaged capture and accessibility checks | queued |
| GS-09 | Editor | Author Willow House rooms, ghost anchors, contamination and tool stations using reusable data | Create/save/reopen/cook scene without source assets | queued |
| GS-10 | QA/performance | Deterministic 1/2/4 player loss/latency/soak matrix | 100/150ms RTT runs, CPU/GPU/network telemetry, no critical errors | queued |

## Milestones
**M0 (2–3 weeks indicative):** host + two remote clients; synchronized room, one tool, one movable object; packet-loss test and connectivity decision.
**M1 (4–6 weeks indicative):** Willow House, 1–4 player 15-minute contract, spectral vacuum, Prankster, cooperative trap, objectives, extraction and save.
**M2 (6–8 weeks indicative):** HQ, company progression, second map, polished presentation.
**M3–M5:** content (10–14 weeks), alpha/beta (6–8 weeks), release (3–4 weeks). These are GDD estimates, contingent on engine capability.

## Initial vertical test: Ghost Shift Sync Lab
1. Host starts a local authoritative session and advertises a join code; two remote clients connect.
2. Each client moves; host resolves collision and broadcasts correction.
3. One client vacuums an ectoplasm patch; host validates reach, battery and patch state; all peers converge.
4. Both clients attempt to clean the last residue simultaneously; reward and objective count advance once.
5. Host spawns a possessed prop; one client tethers it while another purifies; host controls physics outcome.
6. Introduce 100–150 ms RTT, packet loss, client disconnect/reconnect and late/duplicate RPCs.
7. Capture state hashes, reconciliation counts, bandwidth, frame times and memory usage; fail on mismatch or duplicate payout.

## Shared-engine safety rules
- Add reusable engine interfaces only for cross-game needs; Ghost Shift contracts, scoring, ghosts and equipment remain game-owned.
- Keep AETHER-01 CI and serialized content backward compatible.
- Prefer existing replication, physics and AI/Nav infrastructure over parallel replacements.
- Require targeted unit/integration tests and independent review before merging.
- Keep work on this feature branch until CI and review prove it safe.

## Immediate next action
Inspect current networking implementation and test harness on master; implement the smallest GS-01/GS-03 deterministic host-client integration test, then run it under simulated latency. No engine implementation is claimed complete by this planning document.
