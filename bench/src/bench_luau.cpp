// Luau benchmarks (Phase 38 step 5): the cost of calling a script function from C++, and plain VM speed.
#include "aether/bench/bench.h"
#include "aether/script/luau_host.h"

#include <memory>

using namespace aether;
using namespace aether::script;

namespace {

LuauHost& Host() {
    static LuauHost host;
    return host;
}

} // namespace

// 10,000 host-to-script calls of a tiny function (lookup, argument and result marshalling).
AETHER_BENCH_SETUP(
    "luau/call_10k", 10,
    [] { Host().Run("function Add(a, b) return a + b end"); },
    [] {
        const std::vector<ScriptValue> args{1.0, 2.0};
        for (int i = 0; i < 10'000; ++i) Host().Call("Add", args);
    });

// One call that loops 100,000 times inside the VM: interpreter speed without the call overhead.
AETHER_BENCH_SETUP(
    "luau/loop_100k", 10,
    [] { Host().Run("function Sum(n) local s = 0 for i = 1, n do s += i end return s end"); },
    [] { Host().Call("Sum", {100000.0}); });
