# The API reference

Every component, struct and enum the engine reflects is documented, with its
fields (type, whether it's editable, range, unit, tooltip) and its
functions. It is **generated from the code**, so it can't be out of date:

    aether_docgen --out api

writes `api/API.md` (Markdown, with a contents list and links between types)
and `api/api.json` (the same data for tools). The Windows release zip includes
both, generated from the engine you downloaded.

The reference covers whatever is registered in the program that generates it.
The `aether_docgen` tool registers all the engine's modules; a game or plugin
that wants its own types in the reference calls
`aether::docs::WriteApiDocs(directory)` after they are registered, or builds
its own tool the same way. Types you document in code with tooltips, ranges
and units (`AETHER_FIELD(speed, Field_EditAnywhere, {.tooltip = "...", .units = "m/s"})`)
show up in the Inspector and in the reference from the same place.
