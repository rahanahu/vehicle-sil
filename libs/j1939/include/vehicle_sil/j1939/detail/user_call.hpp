// SPDX-License-Identifier: MIT
//
// Not part of the API: what the protocol parts use so that a user function they call (an
// encoder, a callback) may destroy the part. The part keeps a `bool* gone_ = nullptr` member,
// sets *gone_ in its destructor (mark_gone()), and calls user code like this:
//
//     detail::UserCall call(gone_);
//     detail::Lent<std::function<void()>> fn(callback_, call); // the running function outlives the part
//     fn.value();
//     if (call.gone()) { return; } // touch nothing of the part any more
#pragma once

#include <utility>

namespace vehicle_sil::j1939::detail {

/// Marks an object as being in user code. If the user destroys the object, gone() becomes true
/// (also for the calls further out), and the caller must not touch the object any more.
class UserCall
{
public:
    /// `slot`: the object's gone_ member.
    explicit UserCall(bool*& slot) noexcept : slot_(slot), outer_(slot) { slot = &gone_; }
    ~UserCall()
    {
        if (!gone_)
        {
            slot_ = outer_;
        }
        else if (outer_ != nullptr)
        {
            *outer_ = true;
        }
    }
    UserCall(const UserCall&) = delete;
    UserCall& operator=(const UserCall&) = delete;

    [[nodiscard]] bool gone() const noexcept { return gone_; }

private:
    bool*& slot_; ///< Not touched once the object is gone.
    bool* outer_;
    bool gone_ = false;
};

/// For the object's destructor: tells the call in progress, if any, that the object is gone.
inline void mark_gone(bool* slot) noexcept
{
    if (slot != nullptr)
    {
        *slot = true;
    }
}

/// Moves a member (a function object, a buffer) out of the object for a user call and back
/// afterwards, unless the object was destroyed meanwhile: what the call uses lives until it
/// returns. Moving a std::function or a std::vector allocates nothing. Create it after the
/// UserCall (it must be destroyed first).
template <typename T>
class Lent
{
public:
    Lent(T& member, const UserCall& call) : member_(member), call_(call), value(std::move(member)) {}
    ~Lent()
    {
        if (!call_.gone())
        {
            member_ = std::move(value);
        }
    }
    Lent(const Lent&) = delete;
    Lent& operator=(const Lent&) = delete;

private:
    T& member_;
    const UserCall& call_;

public:
    T value;
};

} // namespace vehicle_sil::j1939::detail
