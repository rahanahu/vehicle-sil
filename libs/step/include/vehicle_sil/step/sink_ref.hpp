// SPDX-License-Identifier: MIT
//
// A non-owning reference to a callable that takes values one by one (vehicle_sil::step).
#pragma once

#include <memory>
#include <type_traits>

namespace vehicle_sil::step {

/// Non-owning reference to a callable object `void(const T&)` (e.g. a lambda), valid for one
/// call.
///
/// Used instead of std::function so that handing values over never allocates. The referenced
/// callable must outlive the SinkRef; pass it directly as an argument.
template <typename T>
class SinkRef
{
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, SinkRef> && std::is_object_v<std::remove_reference_t<F>> &&
                 std::is_invocable_v<F&, const T&>)
    SinkRef(F&& f) noexcept
      : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
        call_([](void* object, const T& value) { (*static_cast<std::remove_reference_t<F>*>(object))(value); })
    {
    }

    void operator()(const T& value) const { call_(object_, value); }

private:
    void* object_;
    void (*call_)(void*, const T&);
};

} // namespace vehicle_sil::step
