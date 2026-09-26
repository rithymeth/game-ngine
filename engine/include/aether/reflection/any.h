#pragma once

#include "aether/reflection/type_info.h"

#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <utility>

namespace aether::reflect {

// A value of any reflected type, together with its TypeInfo: the currency
// scripting, Blueprints and the generic Inspector pass values around in.
//
// Values up to kInlineSize bytes (and kInlineAlign alignment) live inside the
// Any itself; larger ones (Mat4, big structs) go to the heap. Copying an Any
// copies its value through TypeInfo::copy_construct; moving an Any leaves the
// source empty. An empty Any has Type() == nullptr. Arrays (char[N]) can't be
// passed by value; use Any::CopyOf, which FieldInfo::Get does for you.
class Any {
public:
    static constexpr usize kInlineSize = 32;
    static constexpr usize kInlineAlign = 16;

    Any() = default;

    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Any> && !std::is_array_v<std::remove_cvref_t<T>> &&
                 Reflected<std::remove_cvref_t<T>>)
    Any(T&& value) { // NOLINT(google-explicit-constructor): implicit so calls read naturally
        using V = std::remove_cvref_t<T>;
        void* storage = Allocate(Reflect<V>());
        new (storage) V(std::forward<T>(value));
    }

    // A default-constructed value of `type` (empty if it has no default constructor).
    static Any DefaultOf(const TypeInfo& type);
    // A copy of the live object at `src`, which must be of `type`.
    static Any CopyOf(const TypeInfo& type, const void* src);

    Any(const Any& other);
    Any(Any&& other) noexcept;
    Any& operator=(const Any& other);
    Any& operator=(Any&& other) noexcept;
    ~Any() { Reset(); }

    void Reset();

    bool HasValue() const { return type_ != nullptr; }
    const TypeInfo* Type() const { return type_; }
    bool IsHeapAllocated() const { return heap_ != nullptr; }

    void* Data() { return heap_ != nullptr ? heap_ : static_cast<void*>(inline_); }
    const void* Data() const { return heap_ != nullptr ? heap_ : static_cast<const void*>(inline_); }

    template <typename T>
    T* TryGet() {
        return type_ == &Reflect<T>() ? static_cast<T*>(Data()) : nullptr;
    }
    template <typename T>
    const T* TryGet() const {
        return type_ == &Reflect<T>() ? static_cast<const T*>(Data()) : nullptr;
    }

    // Asserts on a type mismatch; use TryGet when the type isn't known.
    template <typename T>
    T& Get() {
        T* value = TryGet<T>();
        AETHER_ASSERT(value != nullptr);
        return *value;
    }
    template <typename T>
    const T& Get() const {
        const T* value = TryGet<T>();
        AETHER_ASSERT(value != nullptr);
        return *value;
    }

private:
    static bool FitsInline(const TypeInfo& type) {
        return type.size <= kInlineSize && type.alignment <= kInlineAlign;
    }
    // Sets type_ and returns uninitialized storage for one value of `type`.
    void* Allocate(const TypeInfo& type);

    const TypeInfo* type_ = nullptr;
    void* heap_ = nullptr;
    alignas(kInlineAlign) unsigned char inline_[kInlineSize];
};

namespace detail {

template <typename T>
using Bare = std::remove_cvref_t<T>;

template <typename Signature>
struct FunctionTraits;

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> {
    using Class = C;
    using Return = R;
    static constexpr bool kIsConst = false;
    static constexpr bool kIsStatic = false;
    static constexpr usize kArity = sizeof...(Args);
    template <usize I>
    using Arg = std::tuple_element_t<I, std::tuple<Args...>>;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const> : FunctionTraits<R (C::*)(Args...)> {
    static constexpr bool kIsConst = true;
};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> {
    using Class = void;
    using Return = R;
    static constexpr bool kIsConst = false;
    static constexpr bool kIsStatic = true;
    static constexpr usize kArity = sizeof...(Args);
    template <usize I>
    using Arg = std::tuple_element_t<I, std::tuple<Args...>>;
};

template <auto Fn, usize... I>
void InvokeWithArgs(void* self, Any* args, Any* ret, std::index_sequence<I...>) {
    using Traits = FunctionTraits<decltype(Fn)>;
    using R = typename Traits::Return;
    auto call = [&]() -> decltype(auto) {
        if constexpr (Traits::kIsStatic) {
            return Fn(args[I].template Get<Bare<typename Traits::template Arg<I>>>()...);
        } else {
            using C = typename Traits::Class;
            return (static_cast<C*>(self)->*Fn)(args[I].template Get<Bare<typename Traits::template Arg<I>>>()...);
        }
    };
    if constexpr (std::is_void_v<R>) {
        call();
    } else {
        Any result{Bare<R>(call())};
        if (ret != nullptr) {
            *ret = std::move(result);
        }
    }
}

// Builds a FunctionInfo for a member function pointer or a static/free
// function pointer. Overloaded functions can't be named this way (the pointer
// would be ambiguous); give reflected functions unique names.
template <auto Fn>
FunctionInfo MakeMethod(const char* name, u32 flags = Fn_None, std::initializer_list<const char*> param_names = {}) {
    using Traits = FunctionTraits<decltype(Fn)>;
    using R = typename Traits::Return;

    FunctionInfo function;
    function.name = name;
    function.flags = flags | (Traits::kIsConst ? Fn_Const : Fn_None) | (Traits::kIsStatic ? Fn_Static : Fn_None);
    if constexpr (!std::is_void_v<R>) {
        function.return_type = &Reflect<Bare<R>>();
    }
    [&]<usize... I>(std::index_sequence<I...>) {
        (function.params.push_back(ParamInfo{
             I < param_names.size() ? param_names.begin()[I] : "",
             &Reflect<Bare<typename Traits::template Arg<I>>>(),
         }),
         ...);
    }(std::make_index_sequence<Traits::kArity>{});
    function.thunk = [](void* self, Any* args, Any* ret) {
        InvokeWithArgs<Fn>(self, args, ret, std::make_index_sequence<Traits::kArity>{});
    };
    return function;
}

} // namespace detail

} // namespace aether::reflect
