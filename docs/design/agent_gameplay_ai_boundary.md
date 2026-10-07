# Agent and gameplay AI boundary

Gameplay AI in `ai/` runs inside the game simulation. Behavior trees, perception,
navigation and future utility or planning systems consume game state through engine
components and services, and their decisions must remain deterministic when they affect
replicated gameplay. The AI module does not own editor UI, project-file access, external
model calls or transport protocols.

Editor and MCP agents are authoring assistants. They inspect and propose changes through
editor-owned tools and command stacks; they do not call gameplay AI internals or execute
gameplay decisions. The editor may expose AI assets and debugging views through stable
editor-facing interfaces, while the runtime remains usable without an agent or editor.

The dependency direction is therefore `editor/MCP -> editor command and asset interfaces`
and `gameplay -> engine/AI services`. MCP must not link to `Aether::AI`, and runtime AI
must not depend on MCP, an LLM SDK, or editor code. The module graph check enforces the
current link boundary; additions should preserve it.
