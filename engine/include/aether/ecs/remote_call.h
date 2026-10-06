#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/any.h"

#include <span>
#include <unordered_map>

namespace aether {

// Where a remote function call (Fn_Server, Fn_Client or Fn_Multicast; see
// reflection/type_info.h) goes. The networking layer installs a router on
// a world (net::RpcRouter, Phase 22); C++, Blueprints and Luau all call
// reflected functions through CallFunction, so a remote call made from any
// of them is routed the same way. A world without a router runs every
// call where it is made, as a single-player game would.
enum class RemoteCallResult : u8 {
    RunLocally, // run it here (for a multicast on the server: it has also been sent)
    Sent,       // it went over the network; nothing runs here
    Refused,    // not allowed from here (a server call on an entity this client doesn't own)
};

class RemoteCallRouter {
public:
    virtual ~RemoteCallRouter() = default;
    virtual RemoteCallResult Route(Entity entity, const reflect::TypeInfo& component, const reflect::FunctionInfo& function,
                                   std::span<reflect::Any> args) = 0;
};

namespace detail {
inline std::unordered_map<const World*, RemoteCallRouter*>& RemoteCallRouters() {
    static std::unordered_map<const World*, RemoteCallRouter*> routers;
    return routers;
}
} // namespace detail

// Installs (or, with nullptr, removes) the router for `world`.
inline void SetRemoteCallRouter(const World& world, RemoteCallRouter* router) {
    if (router) detail::RemoteCallRouters()[&world] = router;
    else detail::RemoteCallRouters().erase(&world);
}

inline RemoteCallRouter* GetRemoteCallRouter(const World& world) {
    auto& routers = detail::RemoteCallRouters();
    auto it = routers.find(&world);
    return it == routers.end() ? nullptr : it->second;
}

// Calls `function` of `component` (the type of the component at `self`, on
// `entity`), routing a remote call through the world's router. False when
// it was refused, or the call itself was (see FunctionInfo::Invoke). A
// remote call that was sent leaves `ret` empty.
inline bool CallFunction(World& world, Entity entity, const reflect::TypeInfo& component,
                         const reflect::FunctionInfo& function, void* self, std::span<reflect::Any> args,
                         reflect::Any* ret = nullptr, RemoteCallResult* result = nullptr) {
    RemoteCallResult routed = RemoteCallResult::RunLocally;
    if (function.IsRemote() && !function.HasFlag(reflect::Fn_Static))
        if (RemoteCallRouter* router = GetRemoteCallRouter(world)) routed = router->Route(entity, component, function, args);
    if (result) *result = routed;
    if (routed == RemoteCallResult::Refused) return false;
    if (routed == RemoteCallResult::Sent) return true;
    return function.Invoke(function.HasFlag(reflect::Fn_Static) ? nullptr : self, args, ret);
}

} // namespace aether
