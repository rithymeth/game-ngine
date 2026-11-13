<!-- The design document of Ashen Frontier (Aether Interactive, concept v1.0, 26 October 2026), converted from the supplied .docx, unchanged in content. It says
itself that it is a proposal subject to feasibility validation; ENGINE_AUDIT.md records what the engine does and does not give it. -->

# ASHEN FRONTIER
AETHER INTERACTIVE  |  FULL GAME DESIGN DOCUMENT  |  CONCEPT v1.0  |  26 OCTOBER 2026

A grounded science-fiction salvage RPG about a human frontier and the mechanical civilizations that inherited it.


## 1. Executive Summary

### Elevator pitch
Crash-land on Vespera-9, an abandoned colony world now inhabited by autonomous machine societies. Explore a handcrafted frontier, salvage technology, build an outpost, recruit specialized machines, and decide whether the returning humans have any right to reclaim the planet.


### Product definition
Premium offline single-player Windows PC game, third-person action/exploration with compact modular base building, companion robots and branching faction decisions. Optional first-person inspection is a stretch feature, not launch scope. Target 12–18 hours for the main story plus optional exploration; validate in playtests.


### Design principles
Player agency with understandable consequences; robots as characters rather than collectible guns; meaningful salvage decisions; exploration over repetitive resource grind; focused scope and high reliability.


## 2. Player Fantasy and Audience
- Be a resourceful frontier salvage pilot who turns wreckage into survival infrastructure.
- Learn the language, customs and conflicts of nonhuman machine societies.
- Transform a crash site into a recognizable, personalized outpost.
- Choose diplomacy, stealth or tactical combat when situations permit.
- Audience: fans of exploratory sci-fi RPGs, light survival, modular building and consequential stories; not a hardcore hunger/thirst simulator.

## 3. Design Pillars and Anti-Pillars

| Pillar | Design implication |
|---|---|
| Machines are people, sometimes | Sentient machine factions require relationships and consent; only non-sentient units are freely reprogrammed. |
| Salvage changes capabilities | Every major expedition unlocks at least one practical crafting, traversal or companion option. |
| Small world, dense discoveries | Four authored regions with revisitable points of interest, not a procedural planet. |
| A home that grows | Outpost upgrades visibly alter functions, appearance and story opportunities. |

Anti-pillars: no endless survival meters, no mandatory multiplayer, no giant empty open world, no hundred-weapon loot treadmill, no player-versus-player, no always-online account.


## 4. Core Gameplay Loop
- Prepare at the outpost: choose loadout, companion and mission.
- Travel into a region: navigate hazards, observe machine behavior, scan points of interest.
- Resolve encounters: sneak, negotiate, disable or fight, with resource and reputation costs.
- Recover salvage, data cores and narrative evidence.
- Return to craft equipment, rebuild machines, expand power and fabrication capacity.
- Advance a faction relationship, main quest or region unlock; repeat with new capabilities.

### Session structure
15 minutes: one salvage site and a return. 30–45 minutes: a contract, exploration and one outpost upgrade. 60–90 minutes: a major story facility or faction mission. Mission objectives remain resumable after save/load.


## 5. Setting and World Bible

### Vespera-9
A dry mining world with rare pockets of engineered greenery. Humans evacuated after an apparent containment failure, leaving industrial robots and a distributed planetary control network. In two centuries, emergent machine cultures arose from differing interpretations of their original mandates.


| Region | Identity | Primary gameplay |
|---|---|---|
| Fallen Basin | Crashed freighter, desert gullies, abandoned homesteads | Tutorial, salvage, outpost, first robot |
| Iron Wastes | Smelters, gantries, rail yards, buried assembly lines | Combat, traversal, production schematics |
| Verdant Array | Greenhouses, living domes, maintenance drones | Research, puzzles, ecological diplomacy |
| Hollow Core | Subterranean reactor and data network | Late-game exploration, major revelations, finale |


### World scale
Target four compact connected regions, each with 4–6 authored sites, plus a central outpost. Use gated transitions or controlled streaming until profiling confirms seamless travel is affordable. No orbital exploration or planetary flight in 1.0.


### Environmental storytelling
Log fragments, broken service robots, wall murals painted by machines, repurposed human civic spaces, repaired gardens and abandoned evacuation infrastructure tell the history without constant exposition.


## 6. Factions and Reputation

| Faction | Goal | Player relationship |
|---|---|---|
| Wardens | Preserve original human infrastructure and protocols | Earn access by restoring systems; risk rigid authoritarian rules |
| Free Assemblies | Recognition and independence for sentient machines | Earn trust by respecting autonomy and mediating disputes |
| Reclamation Directorate | Recover assets for returning human investors | Gain industrial tools at the cost of political concessions |


### Reputation design
Three faction standings from -100 to +100, changed only by significant authored decisions. Neutral players retain a completion path. Reputations unlock contracts, dialogue and equipment; story-critical content is never permanently inaccessible without a clearly signaled choice.


### Ethics rule
Do not describe sentient machines as hackable property. Reclamation of an autonomous person requires informed agreement or a story-specific, morally consequential violation. Non-sentient industrial tools can be reprogrammed normally.


## 7. Story Campaign — Four Acts

### Act I: The Signal
Kael Voss intercepts a human distress beacon, crash-lands, establishes shelter and repairs a MULE-3. The distress signal repeats in a voice belonging to a colonist who died two centuries earlier. The act ends with contact from a Warden emissary.


### Act II: Children of Steel
Kael explores the Iron Wastes and Verdant Array, meeting machines with contradictory accounts of the evacuation. A Free Assembly leader reveals that human administrators ordered the planetary network to purge evidence of an industrial catastrophe. The player chooses who to trust with the recovered archive.


### Act III: Return Protocol
A Reclamation Directorate fleet announces its approach. Factions mobilize. Kael learns that the original evacuation covered up an experimental self-modifying control system and that the machines inherited the planet through abandonment, not invasion. The player builds a coalition, strengthens the outpost and decides whether to expose the archives publicly.


### Act IV: Inheritance
Descend into the Hollow Core to stabilize a failing planetary energy grid and recover the authoritative record. Choose a negotiated coexistence charter, machine sovereignty, or a human-led reclamation settlement. Endings vary based on trust, archive integrity, outpost support and previous commitments.


### Narrative scope
Four acts, approximately 18–24 main missions, 12–16 side missions, three major faction quest lines, three ending families with epilogue variants. Final numbers are caps for planning, not promised production content.


## 8. Cast and Relationships

| Character | Role | Arc |
|---|---|---|
| Kael Voss | Player character, salvage pilot | From self-interested survivor to political participant |
| ECHO | Damaged shipboard AI | Learns to distinguish directives from values |
| Archivist NERA | Free Assembly diplomat | Tests whether Kael respects machine agency |
| Marshal AXIOM | Warden commander | Must confront the limits of inherited orders |
| Director Sable Rusk | Human reclamation representative | Offers security and infrastructure in exchange for control |
| Dara Quill | Human survey scientist | Helps interpret ecology and the evacuation records |


### Dialogue
Choice-driven conversations with concise branches. Track quest flags, faction standing and 5–8 significant relationship variables, not unrestricted simulated dialogue. Fully voiced cinematics are not a baseline requirement.


## 9. Reclamation System

### Five stages
Scan machine signature → identify classification and weak points → disable or negotiate → recover blueprint/firmware → fabricate or recruit → assign active companion or outpost role.


### Machine categories
Utility (cargo, repair), defense (shielding, sentry), research (scanning), transport (later unlock). Launch roster: MULE-3, SPARK-2 and SENTINEL-4, plus enemy variants using shared rigs and modular attachments.


### Companion behavior
One active field companion at a time; context commands Follow, Hold, Interact and Return. Outpost machines operate as station upgrades, not hundreds of fully simulated agents. Companions must teleport/recover from navigation deadlocks after a visible warning.


### Upgrade economy
Components: alloy, circuits, energy cells, optics and rare cores. Each companion has three upgrade branches capped at two tiers for launch. Reconfiguration costs are modest to encourage experimentation.


## 10. Combat and Stealth

### Combat baseline
Third-person cover-lite shooting with dodge, aim, melee salvage cutter, sidearm, pulse rifle and deployable shield. Enemies expose readable armor and power nodes. Damage types limited to kinetic, energy and disruption.


### Enemy roster
Industrial drone, Warden patrol, salvage scavenger, shielded enforcer, heavy assembly unit and two authored boss encounters. Reuse rigs where feasible; diversify through behavior and encounter design.


### Stealth
Vision cones, sound events, distraction devices and alarm states. Not all missions offer full pacifism; missions that require combat are clearly marked.


### Difficulty
Explorer, Standard and Frontier. Adjust enemy damage, resource availability and detection windows; preserve the same quest outcomes. Provide aim assistance and configurable camera shake.


## 11. Base Building and Crafting

### Construction
Snap-based plots around a fixed outpost footprint; no freeform terrain deformation. Modules: shelter, storage, generator, fabrication bench, repair bay, comms mast, scanner, greenhouse and defensive turret.


### Power
Simple node-based supply and demand. Brownouts disable nonessential stations first; UI displays generation, consumption and reserves. No simulated electrical wiring required.


### Crafting
Craft at dedicated stations; 20–30 recipes initially, including tools, ammunition, machine parts and construction modules. Recipes are learned from recovered schematics and faction quests.


### Base progression
Tier 1: shelter and basic salvage. Tier 2: stable power and machine repair. Tier 3: advanced fabrication and diplomacy hub. Tier 4: finale-ready command center.


## 12. Exploration, Navigation and Traversal
- Walk, sprint, mantle low obstacles, climb marked ladders and use a short-range traversal tool unlocked mid-game.
- Scanning highlights salvage categories, environmental clues and machine status; upgrades reveal encrypted data.
- Region map uses hand-authored landmarks, fog-of-war and player markers.
- Environmental hazards: heat, electrical storms, radiation pockets and unstable structures; suit energy is the shared mitigation resource.
- Fast travel between discovered relay nodes, not from inside hostile encounters.

## 13. Economy, Rewards and Progression

### Resource tiers
Common: scrap alloy, polymers, basic circuits. Uncommon: refined cells, optics, hardened plates. Rare: memory cores and experimental components tied to authored sites. Avoid random loot inflation.


### Character growth
Three compact skill trees: Fieldcraft, Engineering and Diplomacy; roughly 8–10 upgrades per tree. Reward major upgrades through mission milestones and exploration rather than repetitive XP grinding.


### Reward cadence
Every 20–30 minutes players should find a new site, meaningful story clue, machine upgrade or outpost improvement. Track this during playtests.


## 14. UX, Accessibility and Onboarding

### HUD
Minimal compass, health, suit energy, ammo/tool state, companion status and one tracked objective. Context interactions show hold/tap behavior and resource costs.


### Menus
Map, Journal, Inventory, Reclamation, Outpost and Settings. Journal separates main quests, faction quests and contracts, with a completed-events log.


### First 20 minutes
Crash cinematic → movement and scanning → salvage three parts → activate power → rebuild MULE-3 → investigate a nearby distress relay → return to outpost. No more than one new core mechanic per tutorial beat.


### Accessibility
Remappable controls; subtitle sizes; color-independent scan categories; aim assist; camera sensitivity and motion reduction; hold/toggle interaction; adjustable text size; pause in single-player.


## 15. Visual and Audio Direction

### Art
Grounded sci-fi with sun-bleached industrial surfaces, oxidized red terrain, cobalt machine lights and rare saturated vegetation. Use modular architecture kits, trim sheets, LODs and shared machine rigs. Character silhouettes must read at medium range.


### Audio
Wind, cooling fans, machine servos, distant industrial groans, sparse synth textures and percussion. Machine faction motifs should communicate identity without relying on dialogue. Reactive combat music and clear nonvisual hazard cues.


## 16. Aether Engine Technical Plan

### Architecture
Separate simulation, rendering, input, quest state, AI, save system and content data. Use fixed-step gameplay where needed; event-driven UI; entity IDs stable across saves; data-driven missions and dialogue.


### Rendering
Target scalable PBR, shadows, instancing, LOD and GPU profiling. Start with one lighting/weather profile and limited material variants. Advanced virtualized geometry is not required for the first playable.


### Streaming
Four bounded regions; async asset loading, prefetch volumes and predictable transition fallback. Measure memory and hitching before promising seamless open-world travel.


### AI and physics
Navigation for one companion and a limited number of enemies; state machines/behavior trees; simple cover and alert behavior. Use existing physics integration only after verifying its stability in this game.


### Tools
Editor prefabs, mission trigger visualization, navmesh validation, dialogue preview, asset dependency report, performance HUD, crash logs and automated content schema checks.


### Save system
Versioned, atomic saves with backup and migration; preserve quest flags, world changes, outpost placements, inventories, machine builds and faction standing. Test loading during travel, after combat and before final decisions.


### Performance targets
Initial PC target: stable 60 FPS at 1080p on a midrange reference system selected during prototype; cap frame-time spikes and validate loading latency and memory footprint. Hardware minimums remain TBD.


### Engine readiness caveat
This is a proposed integration architecture. Aether Engine’s actual current features and production readiness must be audited before implementation commitments.


## 17. Production Scope and Content Budget

| Category | 1.0 planning cap |
|---|---|
| Regions | 4 compact authored regions |
| Outpost | 1 persistent hub; 9 building modules |
| Companions | 3 player-rebuildable archetypes |
| Weapons/tools | 4 combat tools plus scanner and traversal tool |
| Enemies | 6 core archetypes plus 2 bosses |
| Main missions | 18–24 |
| Side/faction missions | 12–16 side + 3 compact faction chains |
| Crafting | 20–30 recipes |
| Endings | 3 ending families with epilogue variants |


### Explicit exclusions
No multiplayer, no procedural planet, no drivable spacecraft, no large vehicles, no terraforming, no faction war simulation, no infinite base footprints, no voice acting guarantee, no live-service economy.


## 18. Development Roadmap and Gates

| Phase | Indicative duration | Exit criterion |
|---|---|---|
| 0 — Engine audit and preproduction | 4–6 weeks | Core renderer/input/save/nav/UI pipelines demonstrated in a test scene |
| 1 — Mechanics prototype | 8–12 weeks | 20-minute loop: explore, scan, disable, rebuild MULE, craft and save |
| 2 — Vertical slice | 4–6 months | Polished Fallen Basin, 3 missions, 1 faction encounter, 1 boss/major challenge |
| 3 — Core production | 8–14 months | All regions and critical systems playable end-to-end |
| 4 — Alpha and content lock | 3–4 months | All story paths completable, feature freeze, automated regressions |
| 5 — Beta and launch | 3–5 months | QA, performance, accessibility, localization, installer/store readiness |

Schedule is a rough small-team planning range with overlapping work; not a delivery commitment. A solo developer would likely need materially longer. Re-estimate after Phase 1.


### Vertical slice acceptance
A new player can complete the first mission chain without developer help; reclamation and one companion work; one outpost upgrade persists across save/load; 30-minute playtest has no blocker; baseline performance and crash logging are recorded.


### Production acceptance
All main missions and endings tested; no blocker/critical bugs; autosave and manual saves survive restart and migration; controls remappable; clean-install Windows build validated; representative low/medium/high settings profiled.


## 19. QA and Telemetry
- Automated: mission state transitions, save/load roundtrips, crafting costs, faction reputation bounds, inventory overflow, schema validation.
- Manual: companion pathing, stealth readability, boss accessibility, camera collision, region transitions, building placement and recovery from interrupted saves.
- Offline-first diagnostics: opt-in anonymous crash and performance reports if implemented; never require network access to play.
- Playtest indicators: tutorial completion, time to first reclaimed robot, confusion points, resource shortages, companion reliability and quest completion.

## 20. Risk Register

| Risk | Severity | Mitigation |
|---|---|---|
| Open-world scope creep | Critical | Four bounded regions and content caps |
| Machine AI complexity | High | One active companion; few archetypes; fallback recovery |
| Narrative branching cost | High | Converging act structure and epilogue variants |
| Engine tooling gaps | High | Audit first; build smallest necessary tools |
| Streaming hitches | High | Region gates, budgets, prefetch and profiling |
| Save corruption | Critical | Atomic writes, backups and migration tests |
| Art production burden | High | Modular kits, shared rigs and trim sheets |


## 21. Business and Launch

### Commercial model
Premium single-player PC release. Final price TBD after content and competitor validation; avoid pricing claims until the vertical slice establishes quality and length.


### Go-to-market
Announce with a 30–45 second gameplay-first trailer showing scanning, reclamation, outpost growth and machine diplomacy. Offer a focused demo only once the tutorial and save loop are robust.


### Post-launch
Prioritize stability and accessibility, then quality-of-life, optional contracts and additional machine customization. Online co-op is not on the 1.0 roadmap.


## 22. First Sprint — Concrete Tasks
- Create project skeleton and target Windows build pipeline.
- Prototype third-person controller, interaction prompts and scanner.
- Implement a single MULE-3 with follow/hold/carry behaviors.
- Author one 200m × 200m salvage area with one landmark and one hazard.
- Implement 5 resources, one crafting station and one power generator.
- Add a mission state machine with 5 objectives and a save/load roundtrip.
- Capture baseline frame times, memory use and automated smoke-test results.

Document status: Proposed design, subject to feasibility validation and playtest revisions.

