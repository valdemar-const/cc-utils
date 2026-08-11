#pragma once

// Universal value carrier for the node-graph platform.
//
// `any_value` is a value-semantic, type-erased container built on AnyAny. It
// holds any copyable value, exposes its runtime type via `type_descriptor()`,
// and is introspected via `aa::any_cast<T>`. It crosses the plugin (dlopen)
// boundary as a plain value — no inheritance, no virtual functions on the
// concrete payload type.
//
// CROSS-DSO RULE: a concrete type T wrapped into any_value must have its
// typeinfo anchored in a shared library that every plugin links against
// (e.g. cc::ir::module in libcc-ir, std::string in libstdc++). Under that
// condition `aa::any_cast<T>` succeeds across DSOs and `descriptor_t`
// compares equal between producer and consumer plugins.
//
// The `any_value` typedef itself lives in this shared header so every plugin
// instantiates the same vtable layout; per-TU vtable instances carry absolute
// function pointers, so invocation across DSOs just dereferences pointers.

#include <anyany/anyany.hpp>
#include <anyany/type_descriptor.hpp>

namespace cc {

// The universal pin/wire value type. Copyable + movable + RTTI-enabled.
using any_value = aa::any_with<aa::move, aa::copy, aa::type_info>;

// Stable, comparable, hashable identifier of a value's runtime type.
// Two descriptors compare equal iff their types are equal.
using type_descriptor_t = aa::descriptor_t;

// Compile-time descriptor for a known type T.
template <typename T>
constexpr type_descriptor_t descriptor_of = aa::descriptor_v<T>;

}  // namespace cc
