# Core API inventory (M8.1 draft)

This is the first ownership and threading inventory for APIs used by a game or
plugin. The current engine version is 0.7.0. The module levels in
[api_stability.txt](../design/api_stability.txt) are still **experimental**;
this inventory does not promote any header to a 1.0 stability guarantee.

| Area | Public entry points | Ownership and lifetime | Threading and failure contract |
|---|---|---|---|
| Core types | [`core/base.h`](../../engine/include/aether/core/base.h), [`core/version.h`](../../engine/include/aether/core/version.h) | Value types and compile-time constants. | No shared state. Version is a source constant, currently 0.7.0. |
| ECS | [`ecs/world.h`](../../engine/include/aether/ecs/world.h), [`ecs/entity.h`](../../engine/include/aether/ecs/entity.h), [`ecs/component.h`](../../engine/include/aether/ecs/component.h) | `World` owns entities and components. An `Entity` is a generation-checked handle; check `World::IsAlive` after destruction. Reacquire component pointers after structural edits that can move rows. | `World` does not synchronize itself. Coordinate mutation with iteration and parallel jobs. Component registration assigns process-local IDs; serialized files use names instead. |
| Jobs | [`job/job_system.h`](../../engine/include/aether/job/job_system.h) | The caller keeps job data and `JobCounter` alive until `Wait` returns, and drains jobs before destroying `JobSystem`. | Schedule and wait on the constructing thread or that instance's worker threads. Calls from unrelated threads are unsupported. A job callback runs on the caller or a worker. |
| Logging | [`core/log.h`](../../engine/include/aether/core/log.h) | `Logger::Instance` owns the recent-line buffer and sink registrations. `Recent` returns a copy. Sink callbacks must keep their captured state alive until in-flight calls finish. | Logging, sink registration, level and console-output settings are synchronized. Sinks run on the logging thread outside the logger lock, so they may log. `RemoveSink` can return while an earlier callback is still running. |
| Reflection | [`reflection/registry.h`](../../engine/include/aether/reflection/registry.h), [`reflection/type_info.h`](../../engine/include/aether/reflection/type_info.h) | `TypeRegistry` stores pointers; registered `TypeInfo` and referenced metadata must live until process exit. A lookup returns a borrowed pointer. | Registration and lookup lock the registry. Duplicate names keep the first registration and log an error. |
| Serialization | [`reflection/serialize.h`](../../engine/include/aether/reflection/serialize.h), [`scene/serialization.h`](../../engine/include/aether/scene/serialization.h) | Save functions return owned text or bytes. Load edits a caller-owned object or `World`; scene loads add entities rather than clear the world. | Register migrations and custom converters during startup, before parallel save/load. Loads tolerate unknown fields/components with warnings; malformed top-level data returns `false`. Saved type versions require migration when a schema changes. |
| Plugins | [`plugin/plugin.h`](../../engine/include/aether/plugin/plugin.h) | `PluginManager` owns discovered descriptors and started modules. Pointers from `Find` and `Enabled` are borrowed and invalidated by `Discover`. The manager shuts modules down in reverse start order. | The host serializes discover, resolve, start and shutdown. Resolve fails for missing required dependencies, incompatible versions or cycles; optional dependency problems are warnings. |
| Errors and files | [`platform/filesystem.h`](../../engine/include/aether/platform/filesystem.h), [`project/project.h`](../../engine/include/aether/project/project.h) | Callers own output buffers and any `std::string* error` supplied to an operation. | APIs still mix `bool` plus optional error text, reports and log-only warnings. A common error type and a `[[nodiscard]]` audit remain M8.1 work. |

## Compatibility work before Core API 1.0

1. Choose which headers and declarations are stable; record the choices in
   `api_stability.txt` or header overrides. Everything currently defaults to
   experimental.
2. Freeze the selected surface and check removals against a previous release.
   The existing stability checker validates level labels, but does not compare
   C++ signatures or ABI.
3. Load a corpus of projects and serialized scenes from older engine versions
   on each supported platform. Record migrations and unresolved breaks.
4. Resolve the open error-return convention and audit ownership, mutation and
   allocation behavior in the hot ECS, jobs and serialization paths.

These are release-gate tasks, not claims that Core API 1.0 is complete.
