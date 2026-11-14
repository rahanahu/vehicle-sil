// SPDX-License-Identifier: MIT
#include "vehicle_sil/step/host.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::step {

void Host::require_not_started(const char* what) const
{
    if (last_step_.has_value() || failed_)
    {
        throw std::logic_error(std::string("vehicle_sil::step::Host: ") + what + " is only allowed before the first step");
    }
}

namespace {

const char* domain_name(TimeDomain domain) noexcept
{
    return domain == TimeDomain::Virtual ? "Virtual" : "RealTime";
}

} // namespace

void Host::check_can_add(TimeDomain domain) const
{
    require_not_started("add()");
    if (required_domain_ && *required_domain_ != domain)
    {
        throw std::logic_error(std::string("vehicle_sil::step::Host: the driver requires ports of the ") +
                               domain_name(*required_domain_) + " time domain, the port is " + domain_name(domain));
    }
    if (domain_ && *domain_ != domain)
    {
        throw std::logic_error(std::string("vehicle_sil::step::Host: the ports of a host must share a time domain (") +
                               domain_name(*domain_) + " and " + domain_name(domain) + ")");
    }
}

void Host::add(Member& member)
{
    const TimeDomain domain = member.time_domain();
    check_can_add(domain);
    if (std::find(members_.begin(), members_.end(), &member) != members_.end())
    {
        throw std::logic_error("vehicle_sil::step::Host: a member can be added only once");
    }
    members_.push_back(&member);
    domain_ = domain;
    {
        const std::lock_guard lock(post_mutex_);
        if (wakeup_)
        {
            member.set_wakeup(wakeup_);
        }
    }
}

void Host::require_time_domain(TimeDomain domain)
{
    require_not_started("require_time_domain()");
    if (required_domain_ && *required_domain_ != domain)
    {
        throw std::logic_error(std::string("vehicle_sil::step::Host: two drivers require different time domains (") +
                               domain_name(*required_domain_) + " and " + domain_name(domain) + ")");
    }
    if (domain_ && *domain_ != domain)
    {
        throw std::logic_error(std::string("vehicle_sil::step::Host: the driver requires ports of the ") + domain_name(domain) +
                               " time domain, the host has ports of the " + domain_name(*domain_) + " time domain");
    }
    required_domain_ = domain;
}

std::optional<TimeDomain> Host::time_domain() const noexcept
{
    return domain_ ? domain_ : required_domain_;
}

void Host::post(std::function<void()> fn)
{
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::step::Host: post() needs a function");
    }
    const std::lock_guard lock(post_mutex_);
    if (failed_.load())
    {
        return; // never runs: do not keep it
    }
    posted_.push_back(std::move(fn));
    if (wakeup_)
    {
        wakeup_(SimTime{});
    }
}

void Host::set_wakeup(Wakeup wakeup)
{
    const std::lock_guard lock(post_mutex_);
    wakeup_ = std::move(wakeup);
    for (Member* member : members_)
    {
        member->set_wakeup(wakeup_);
    }
}

std::optional<SimTime> Host::next_deadline() const noexcept
{
    std::optional<SimTime> earliest;
    for (const Member* member : members_)
    {
        const auto deadline = member->next_deadline();
        if (deadline && (!earliest || *deadline < *earliest))
        {
            earliest = deadline;
        }
    }
    return earliest;
}

void Host::discard_posted() noexcept
{
    const std::lock_guard lock(post_mutex_);
    posted_.clear();
}

void Host::run_posted()
{
    {
        const std::lock_guard lock(post_mutex_);
        if (posted_.empty())
        {
            return;
        }
        running_.swap(posted_);
    }
    // Clear what ran even if a function throws (the host fails; nothing runs again).
    struct Clear
    {
        std::vector<std::function<void()>>& v;
        ~Clear() { v.clear(); }
    } clear{running_};
    for (auto& fn : running_)
    {
        fn();
        if (failed_)
        {
            throw std::logic_error("vehicle_sil::step::Host: the host failed during the step: " + error_);
        }
    }
}

void Host::set_application(Application application)
{
    require_not_started("set_application()");
    application_ = std::move(application);
}

void Host::set_step_size(SimDuration step_size)
{
    require_not_started("set_step_size()");
    if (step_size <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::step::Host: the step size must be > 0");
    }
    step_size_ = step_size;
}

void Host::step(SimTime now)
{
    if (failed_)
    {
        throw std::logic_error("vehicle_sil::step::Host: the host has failed: " + error_);
    }
    try
    {
        if (in_step_)
        {
            throw std::logic_error("vehicle_sil::step::Host: step() called from inside a step");
        }
        if (last_step_.has_value() && now <= *last_step_)
        {
            throw std::logic_error("vehicle_sil::step::Host: step time must increase (got t=" + std::to_string(to_ns(now)) +
                                   " ns after t=" + std::to_string(to_ns(*last_step_)) + " ns)");
        }
        in_step_ = true;
        const bool first = !last_step_.has_value();
        last_step_ = now;
        if (first)
        {
            for (Member* member : members_)
            {
                member->start(step_size_);
            }
        }
        run_step(now);
        in_step_ = false;
    }
    catch (const std::exception& e)
    {
        in_step_ = false;
        // The error first, then the flag (release): a thread that sees failed() reads a
        // complete error(), which is not written again afterwards.
        if (!failed_.load(std::memory_order_acquire))
        {
            error_ = e.what();
            failed_.store(true, std::memory_order_release);
        }
        discard_posted();
        throw;
    }
    catch (...)
    {
        in_step_ = false;
        // The error first, then the flag (release): a thread that sees failed() reads a
        // complete error(), which is not written again afterwards.
        if (!failed_.load(std::memory_order_acquire))
        {
            error_ = "unknown exception";
            failed_.store(true, std::memory_order_release);
        }
        discard_posted();
        throw;
    }
}

void Host::run_step(SimTime now)
{
    for (Member* member : members_)
    {
        member->begin(now);
    }
    for (Member* member : members_)
    {
        member->receive(now);
    }
    for (Member* member : members_)
    {
        member->process();
    }
    run_posted();
    if (application_)
    {
        application_(now);
        if (failed_)
        {
            // A nested step() failed the host and the application swallowed the exception.
            // (Checked again after the flushes, for a member that does the same.)
            throw std::logic_error("vehicle_sil::step::Host: the host failed during the step: " + error_);
        }
    }
    for (Member* member : members_)
    {
        member->flush();
    }
    if (failed_)
    {
        throw std::logic_error("vehicle_sil::step::Host: the host failed during the step: " + error_);
    }
}

} // namespace vehicle_sil::step
