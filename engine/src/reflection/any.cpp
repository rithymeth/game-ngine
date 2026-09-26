#include "aether/reflection/any.h"

#include <new>

namespace aether::reflect {

void* Any::Allocate(const TypeInfo& type) {
    type_ = &type;
    if (FitsInline(type)) {
        heap_ = nullptr;
        return inline_;
    }
    heap_ = ::operator new(type.size, std::align_val_t{type.alignment});
    return heap_;
}

void Any::Reset() {
    if (type_ == nullptr) {
        return;
    }
    type_->destruct(Data());
    if (heap_ != nullptr) {
        ::operator delete(heap_, std::align_val_t{type_->alignment});
        heap_ = nullptr;
    }
    type_ = nullptr;
}

Any Any::DefaultOf(const TypeInfo& type) {
    Any any;
    if (type.construct != nullptr) {
        type.construct(any.Allocate(type));
    }
    return any;
}

Any Any::CopyOf(const TypeInfo& type, const void* src) {
    AETHER_ASSERT(type.copy_construct != nullptr);
    Any any;
    type.copy_construct(any.Allocate(type), src);
    return any;
}

Any::Any(const Any& other) {
    if (other.type_ != nullptr) {
        AETHER_ASSERT(other.type_->copy_construct != nullptr);
        other.type_->copy_construct(Allocate(*other.type_), other.Data());
    }
}

Any::Any(Any&& other) noexcept {
    if (other.type_ == nullptr) {
        return;
    }
    if (other.heap_ != nullptr) {
        // Steal the heap block; no per-value work needed.
        type_ = other.type_;
        heap_ = other.heap_;
        other.heap_ = nullptr;
        other.type_ = nullptr;
        return;
    }
    other.type_->move_construct(Allocate(*other.type_), other.inline_);
    other.Reset();
}

Any& Any::operator=(const Any& other) {
    if (this != &other) {
        Any copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Any& Any::operator=(Any&& other) noexcept {
    if (this != &other) {
        Reset();
        new (this) Any(std::move(other));
    }
    return *this;
}

Any FieldInfo::Get(const void* object) const {
    return Any::CopyOf(*type, Ptr(object));
}

bool FieldInfo::Set(void* object, const Any& value) const {
    if (value.Type() != type || type->copy_assign == nullptr) {
        return false;
    }
    type->copy_assign(Ptr(object), value.Data());
    return true;
}

bool FunctionInfo::Invoke(void* self, std::span<Any> args, Any* ret) const {
    if (args.size() != params.size()) {
        return false;
    }
    if (!HasFlag(Fn_Static) && self == nullptr) {
        return false;
    }
    for (usize i = 0; i < params.size(); ++i) {
        if (args[i].Type() != params[i].type) {
            return false;
        }
    }
    thunk(self, args.data(), ret);
    return true;
}

} // namespace aether::reflect
