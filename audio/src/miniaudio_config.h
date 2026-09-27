#pragma once

// What of miniaudio the engine uses: only device output (Phase 17 step 5).
// Shared by the file that compiles it and the files that use it, so both
// see the same declarations.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
