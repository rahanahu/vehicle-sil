// SPDX-License-Identifier: MIT
//
// The table of the rules of a run's controls (control_rules.hpp) and decide(), which reads it.
#include "vehicle_sil/launch/control_rules.hpp"

#include "vehicle_sil/launch/config.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>

namespace vehicle_sil::launch {

const char* operation_name(Operation operation)
{
    switch (operation)
    {
    case Operation::Pause: return "pause";
    case Operation::Resume: return "resume";
    case Operation::Step: return "step";
    case Operation::RunUntil: return "run_until";
    case Operation::Speed: return "speed";
    case Operation::Stop: return "stop";
    case Operation::Write: return "write";
    case Operation::Unknown: return "";
    }
    return "";
}

Operation command_operation(std::string_view name)
{
    for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil, Operation::Speed,
                              Operation::Stop})
    {
        if (name == operation_name(o))
        {
            return o;
        }
    }
    return Operation::Unknown;
}

const char* entry_name(Entry entry) { return entry == Entry::ControlServer ? "control_server" : "stdin"; }

std::optional<Entry> entry_of(std::string_view source)
{
    for (const Entry e : {Entry::Stdin, Entry::ControlServer})
    {
        if (source == entry_name(e))
        {
            return e;
        }
    }
    return std::nullopt;
}

namespace {

/// What a row looks at.
struct Query
{
    const ControlSituation& situation;
    Entry entry;
    Operation operation;
    std::string_view name;
    const Command* arguments; ///< nullptr: no argument is looked at

    [[nodiscard]] bool is(Operation o) const { return operation == o; }
    /// One of the operations that move the time.
    [[nodiscard]] bool moves_time() const
    {
        return is(Operation::Pause) || is(Operation::Resume) || is(Operation::Step) || is(Operation::RunUntil) ||
               is(Operation::Speed);
    }
    /// The time of a pause at a time or of a run_until, if the arguments are looked at and have one.
    [[nodiscard]] const std::int64_t* at_ms() const
    {
        return arguments != nullptr && arguments->at_ms && (is(Operation::Pause) || is(Operation::RunUntil))
                   ? &*arguments->at_ms
                   : nullptr;
    }
    /// A pause at a time, or a run_until: they need the controller's step to round to. Without
    /// arguments a pause has no time and a run_until has one.
    [[nodiscard]] bool at_time() const
    {
        return at_ms() != nullptr || (arguments == nullptr && is(Operation::RunUntil));
    }
    [[nodiscard]] std::string name_text() const { return std::string(name); }
};

/// One row: the operations it refuses and when, the code, and the message.
struct Rule
{
    const char* reason;
    bool (*refuses)(const Query& q);
    std::string (*message)(const Query& q);
};

// The rows, in the order they are asked: the first that refuses gives the answer. What no row
// refuses is taken. By entry, operation and situation (the last four also by the arguments):
//
//   row                      entry           operation                          when
//   scenario-running         control server  pause resume step run_until speed  a scenario plays
//                                            write
//   not-running              standard input  all but stop                       detached
//                            control server  all but write                      detached
//   not-a-time-command       both            unknown                            always
//                            standard input  write                              always
//   outside-real-time        both            pause step run_until,              the time is locked (the run is
//                                            speed other than 1 (also max)      connected outside, --time-policy
//                                                                               realtime)
//   out-of-range (speed)     both            speed                              value not kMinSpeed..kMaxSpeed
//   out-of-range (step)      both            step                               ms not 1..kMaxMillis
//   missing-argument         both            run_until                          no at_ms
//   out-of-range (time)      both            pause at a time, run_until         at_ms not 0..kMaxMillis
//   not-set-up               both            step, pause at a time, run_until   setting up
//   not-paused               both            step                               not paused
//
// The two entries differ in two places, and both are how the launcher has always answered:
//   - While a scenario plays, a control server's clients neither write nor move the time; the
//     commands of standard input are taken. (The control server refuses those calls itself, before
//     they come here: always_refused(). The row is first because that refusal is.)
//   - A stop from standard input is taken whenever it comes (it is a stop request, and the run
//     takes one until the process ends); a control server's is refused once the simulation is over.
// A write goes to the hub, not through the time commands: whether the run is over does not
// matter to it here.
//
// The time lock and a scenario: the lock's row refuses whatever the scenario's row leaves (the
// commands of standard input while a scenario plays). A control server's clients are refused by
// the scenario's row, which stays first: the server refuses those calls itself, with that one
// reason, before they come here (always_refused()), and the table must answer as it does.
constexpr std::array<Rule, 10> kRules{{
    {reason::kScenarioRunning,
     [](const Query& q) {
         return q.entry == Entry::ControlServer && q.situation.scenario && (q.moves_time() || q.is(Operation::Write));
     },
     [](const Query&) {
         return std::string(
             "this run plays a scenario (run --scenario), and until it ends only the scenario writes variables and "
             "moves the time, so that the same scenario always gives the same result; a client can read (ListPeers, "
             "GetValues, GetSnapshot, Watch, GetTime, WaitPaused) and Stop; to write or to move the time from a "
             "client, run without --scenario");
     }},
    {reason::kNotRunning,
     [](const Query& q) {
         return q.situation.stage == ControlStage::Detached &&
                !(q.entry == Entry::Stdin && q.is(Operation::Stop)) &&
                !(q.entry == Entry::ControlServer && q.is(Operation::Write));
     },
     [](const Query& q) {
         return "the simulation is not running (it is over, or it did not start): " + q.name_text() +
                " works from the start of the run until the simulation ends";
     }},
    {reason::kNotATimeCommand,
     [](const Query& q) { return q.is(Operation::Unknown) || (q.entry == Entry::Stdin && q.is(Operation::Write)); },
     [](const Query& q) {
         return "'" + q.name_text() +
                "' is no time command: the time commands are pause, resume, step, run_until and speed";
     }},
    {reason::kOutsideRealTime,
     [](const Query& q) {
         return q.situation.time_locked &&
                (q.is(Operation::Pause) || q.is(Operation::Step) || q.is(Operation::RunUntil) ||
                 (q.is(Operation::Speed) && q.arguments != nullptr &&
                  q.arguments->speed != std::optional<double>(1.0)));
     },
     [](const Query& q) {
         return "this run is connected outside the simulation and keeps to real time (--time-policy realtime, the "
                "default): " +
                q.name_text() +
                (q.is(Operation::Speed) ? std::string(" other than 1") : std::string()) +
                " is refused, because what is connected does not wait while the virtual time stands still, jumps or "
                "goes at another speed (a resume, a speed of 1 and a stop are taken). To move the time all the same, "
                "start the run again with --time-policy free, or without the connection (no --connect, --disable for a "
                "participant with role: bridge)";
     }},
    {reason::kOutOfRange,
     [](const Query& q) {
         return q.is(Operation::Speed) && q.arguments != nullptr && q.arguments->speed &&
                !(*q.arguments->speed >= kMinSpeed && *q.arguments->speed <= kMaxSpeed);
     },
     [](const Query&) {
         return std::string(
             "the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max");
     }},
    {reason::kOutOfRange,
     [](const Query& q) {
         return q.is(Operation::Step) && q.arguments != nullptr &&
                !(q.arguments->ms.value_or(1) >= 1 && q.arguments->ms.value_or(1) <= kMaxMillis);
     },
     [](const Query&) { return "step needs a time from 1 to " + std::to_string(kMaxMillis) + " ms"; }},
    {reason::kMissingArgument,
     [](const Query& q) { return q.is(Operation::RunUntil) && q.arguments != nullptr && !q.arguments->at_ms; },
     [](const Query&) { return std::string("run_until needs the time to stand still at"); }},
    {reason::kOutOfRange,
     [](const Query& q) { return q.at_ms() != nullptr && !(*q.at_ms() >= 0 && *q.at_ms() <= kMaxMillis); },
     [](const Query& q) {
         return q.name_text() + " at a time needs a time from 0 to " + std::to_string(kMaxMillis) + " ms";
     }},
    {reason::kNotSetUp,
     [](const Query& q) {
         return (q.is(Operation::Step) || q.at_time()) && q.situation.stage == ControlStage::SettingUp;
     },
     [](const Query& q) {
         return "the simulation has not been set up yet: " + q.name_text() +
                (q.is(Operation::Step)
                     ? std::string(" works once it stands still (a state event with paused true), with "
                                   "--start-paused from the start")
                     : std::string(" at a time works once the participants have been started"));
     }},
    {reason::kNotPaused,
     [](const Query& q) { return q.is(Operation::Step) && !q.situation.paused; },
     [](const Query&) {
         return std::string("step works while the simulation is paused: send {\"command\":\"pause\"} first (or run "
                            "with --start-paused)");
     }},
}};

} // namespace

Decision decide(const ControlSituation& situation, Entry entry, Operation operation, std::string_view name,
                const Command* arguments)
{
    const Query q{situation, entry, operation, name, arguments};
    for (const Rule& rule : kRules)
    {
        if (rule.refuses(q))
        {
            return Decision{false, rule.reason, rule.message(q)};
        }
    }
    return Decision{};
}

Allowed allowed(const ControlSituation& situation, Entry entry, Operation operation)
{
    Decision d = decide(situation, entry, operation, operation_name(operation), nullptr);
    Allowed out{d.ok, std::move(d.reason), std::move(d.message), std::nullopt, std::nullopt};
    // What the lock would refuse is taken in a run whose time is the user's although it is
    // connected outside: it is told with the answer.
    if (out.ok && situation.time_free &&
        (operation == Operation::Pause || operation == Operation::Step || operation == Operation::RunUntil ||
         operation == Operation::Speed))
    {
        out.caution = Caution{
            caution::kOutsideNotRealTime,
            std::string("this run is connected outside the simulation with --time-policy free: ") +
                operation_name(operation) + (operation == Operation::Speed ? " other than 1" : "") +
                " is taken, but what is connected does not wait for the virtual time and its bridge still carries "
                "frames both ways (frames from outside pile up while the simulation stands still); the run then ends "
                "with the note outside-time-changed"};
    }
    if (operation == Operation::Step)
    {
        out.range = ArgumentRange{1, static_cast<double>(kMaxMillis)};
    }
    else if (operation == Operation::Speed)
    {
        // Locked: only the speed of 1 is taken (the row outside-real-time).
        out.range = situation.time_locked ? ArgumentRange{1, 1} : ArgumentRange{kMinSpeed, kMaxSpeed};
    }
    return out;
}

std::optional<Decision> always_refused(bool scenario, Entry entry)
{
    std::optional<Decision> first;
    for (const ControlStage stage : {ControlStage::SettingUp, ControlStage::Attached, ControlStage::Detached})
    {
        for (const bool paused : {false, true})
        {
            // No lock, the lock, and the time left to the user (never both of the last two).
            for (const int outside : {0, 1, 2})
            {
                for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil,
                                          Operation::Speed, Operation::Write})
                {
                    Decision d = decide(ControlSituation{stage, paused, scenario, outside == 1, outside == 2}, entry, o,
                                        operation_name(o), nullptr);
                    if (d.ok || (first && first->reason != d.reason))
                    {
                        return std::nullopt;
                    }
                    if (!first)
                    {
                        first = std::move(d);
                    }
                }
            }
        }
    }
    return first;
}

} // namespace vehicle_sil::launch
