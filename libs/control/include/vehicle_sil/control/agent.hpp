// SPDX-License-Identifier: MIT
//
// The control agent of one ECU: typed variables, timed writes applied inside the simulation
// steps, sampled values and events, all through bounded queues (vehicle_sil::control).
#pragma once

#include "vehicle_sil/control/spsc_ring.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace vehicle_sil::control {

namespace detail {
/// An input of a 64-bit unsigned type cannot take all its values from an int64.
template <typename T>
inline constexpr bool writable_type_v = !(std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t));
} // namespace detail

enum class ValueType
{
    Bool,
    Integer,
    Real,
    Enum, ///< an integer with named values (VariableInfo::items)
};

/// What a variable is for. The names follow FMI (Functional Mock-up Interface).
enum class Causality
{
    Input,     ///< written from outside while the simulation runs (a switch, a sensor value)
    Output,    ///< read only (a computed value)
    Parameter, ///< written from outside only before the first step
};

using Value = std::variant<bool, std::int64_t, double>;

struct EnumItem
{
    std::string name;
    std::int64_t value = 0;

    friend bool operator==(const EnumItem&, const EnumItem&) = default;
};

/// Self-description of a variable: enough for a client to build an editor for it.
struct VariableInfo
{
    std::string name; ///< unique; '.' separates groups ("cabin.parking_brake")
    ValueType type = ValueType::Real;
    Causality causality = Causality::Input;
    std::string unit;
    std::optional<double> min;  ///< inclusive; Integer and Enum: also limited by the C++ type
    std::optional<double> max;  ///< inclusive
    std::optional<double> step; ///< a hint for editors (slider resolution); not enforced
    std::string description;
    std::vector<EnumItem> items; ///< Enum only: the allowed values

    friend bool operator==(const VariableInfo&, const VariableInfo&) = default;
};

struct VariableOptions
{
    std::string unit{};
    std::optional<double> min{};
    std::optional<double> max{};
    std::optional<double> step{};
    std::string description{};
    /// Writable only before the first step (Causality::Parameter) instead of at any time.
    bool parameter = false;
};

struct Write
{
    std::string name;
    Value value;
};

/// What to do with a write whose time has passed when the participant receives it.
enum class LatePolicy
{
    Reject,    ///< do not apply it (CommandStatus::Late)
    ApplyNext, ///< apply it in the next step (CommandResult::late is true)
};

/// One or more writes applied together in the same step. A check failure at submit() rejects
/// all of them; if a setter throws while they are applied, the writes before it stay.
struct Command
{
    std::uint64_t seq = 0;  ///< assigned by the sender; orders commands that take effect in the same step
    std::vector<Write> writes;
    std::optional<SimTime> at; ///< empty: the next step
    LatePolicy late = LatePolicy::Reject;
};

enum class SubmitStatus
{
    Queued,  ///< a CommandResult follows
    Busy,    ///< too many commands in flight (AgentOptions::max_pending); try again later
    Invalid, ///< unknown name, wrong type, out of range or not writable; nothing was queued
};

struct SubmitResult
{
    SubmitStatus status = SubmitStatus::Queued;
    std::string reason; ///< for Invalid and Busy
};

enum class CommandStatus
{
    Applied,
    Late,      ///< its time had passed (LatePolicy::Reject)
    Rejected,  ///< not writable any more (a Parameter after the first step), or a setter threw
    Cancelled, ///< the simulation ended before its time (Agent::finish())
};

struct CommandResult
{
    std::uint64_t seq = 0;
    CommandStatus status = CommandStatus::Applied;
    /// Start time of the step that applied it (Applied only).
    std::optional<SimTime> applied_at;
    bool late = false; ///< its time had passed (Late, or applied late with LatePolicy::ApplyNext)
    std::string reason;
};

/// The values of all variables (in schema order) at the end of a step.
struct Snapshot
{
    SimTime time{};
    std::uint64_t sequence = 0; ///< 0: no sample yet; increases with every published sample
    std::vector<Value> values;
};

enum class EventLevel
{
    Info,
    Warning,
    Error,
};

struct Event
{
    SimTime time{};
    EventLevel level = EventLevel::Info;
    std::string text;
};

struct AgentStats
{
    std::uint64_t submitted = 0;
    std::uint64_t busy = 0;
    std::uint64_t invalid = 0;
    std::uint64_t applied = 0;
    std::uint64_t late = 0;
    std::uint64_t rejected = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t events_dropped = 0;
};

struct AgentOptions
{
    /// Commands in flight: submitted and not yet consumed by drain_results(). Also the capacity
    /// of the command and result queues, so a result is never dropped. Must be > 0.
    std::size_t max_pending = 256;
    /// Capacity of the event queue. Events beyond it are dropped and counted. Must be > 0.
    std::size_t max_events = 1024;
    /// The values are sampled in the first step at or after 0, sample_period, 2 * sample_period,
    /// ... (like Peer::every()). Zero: every step.
    SimDuration sample_period = std::chrono::milliseconds(10);
};

/// The control agent of one ECU (vehicle_sil::Ecu::agent(): the ECU makes it, declares its
/// variables on it, and drives it from its steps).
///
/// Two sides use it:
/// - the step thread (the ECU's simulation steps): on_step_begin(), on_step_end(), log(), and
///   finish() after the last step (the ECU calls on_step_begin(), on_step_end() and finish()).
///   Call them from one thread at a time. The setters and getters of the variables run only
///   here, so the model needs no locks. The step thread never waits for the transport side.
/// - the transport side (the connection to a control server, or a test): submit(),
///   drain_results(), drain_events(), latest(), stats(). A transport that outlives the calls it
///   makes (one that lives beside the ECU, such as control_silkit::AgentTransport) holds a TransportLease on
///   the agent while it lives. These are thread safe among themselves
///   (a lock that the step thread never takes); they may wait briefly for finish() and for the
///   wake-up of a real-time driver (Host::post()).
/// One thread may play both sides. schema() may be read from any thread after registration.
///
/// A command takes effect in the first step whose start time is at or after Command::at, in
/// on_step_begin(), so the application sees the new values in that very step. Commands that take
/// effect in the same step are applied in (at, seq) order; a command without a time sorts as
/// the current step. A command whose time has passed (the participant ran a step at or after
/// `at` before it received the command) is Late. Every queued command gets exactly one result.
///
/// Bounds: at most max_pending commands are in flight (submit() returns Busy beyond that, at
/// once), at most max_events events are queued, and the values are kept in three buffers of one
/// value per variable. The queues are allocated in the constructor and the value buffers in the
/// first step. The step thread does free the commands it consumed and builds the results'
/// reason texts (bounded by max_pending per step); it takes no lock that the transport thread
/// holds.
///
/// Register the variables before the schema is complete (std::logic_error afterwards; this is a
/// check against misuse, not a synchronisation: do not register from one thread while another
/// submits). The schema is complete from the first of: complete_schema() (a transport such as
/// control_silkit::AgentTransport calls it before it reads schema(); the driver before the first
/// step), the first submit() and the first step. A pointer or function passed to input() and
/// output() must stay valid as long as the agent is used.
class Agent
{
public:
    /// std::invalid_argument for options that check_options() refuses.
    explicit Agent(AgentOptions options = {});
    /// std::invalid_argument (saying which option and why) for options the constructor refuses:
    /// max_pending or max_events 0, a negative sample_period.
    static void check_options(const AgentOptions& options);
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // ---- registration -------------------------------------------------------------------

    /// A writable variable bound to `*value` (bool, an integer type up to 64 bits signed or 32
    /// bits unsigned, or a floating-point type).
    template <typename T>
    void input(std::string name, T* value, VariableOptions options = {})
    {
        input(std::move(name), [value] { return *value; }, [value](T v) { *value = v; }, std::move(options));
    }

    /// A writable variable read and written through functions (the setter may do more than
    /// store). Its type is the getter's return type. If the setter throws, the command is
    /// Rejected; the writes of the command before it stay applied.
    template <typename Get, typename Set>
        requires std::is_invocable_v<Get>
    void input(std::string name, Get get, Set set, VariableOptions options = {})
    {
        using T = std::decay_t<std::invoke_result_t<Get>>;
        static_assert(detail::writable_type_v<T>,
                      "a 64-bit unsigned input does not fit the int64 values; use std::int64_t (outputs may be unsigned)");
        add_typed<T>(std::move(name), std::function<T()>(std::move(get)), std::function<void(T)>(std::move(set)),
                     options.parameter ? Causality::Parameter : Causality::Input, std::move(options), {});
    }

    /// A writable integer or enum variable that only takes the values of `items` (each within
    /// the type, names and values unique).
    template <typename T>
    void input_enum(std::string name, T* value, std::vector<EnumItem> items, VariableOptions options = {})
    {
        using Raw = std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>;
        using U = typename Raw::type;
        static_assert(std::is_integral_v<U> && !std::is_same_v<U, bool>, "input_enum needs an enum or integer type");
        static_assert(detail::writable_type_v<U>, "a 64-bit unsigned input does not fit the int64 values");
        add_typed<U>(std::move(name), [value] { return static_cast<U>(*value); }, [value](U v) { *value = static_cast<T>(v); },
                     options.parameter ? Causality::Parameter : Causality::Input, std::move(options), std::move(items), true);
    }

    /// A read-only variable; its type is the getter's return type.
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output(std::string name, Get get, VariableOptions options = {})
    {
        using T = std::decay_t<std::invoke_result_t<Get>>;
        add_typed<T>(std::move(name), std::function<T()>(std::move(get)), {}, Causality::Output, std::move(options), {});
    }

    /// A read-only integer variable that takes the values of `items` (a state).
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output_enum(std::string name, Get get, std::vector<EnumItem> items, VariableOptions options = {})
    {
        using T = std::decay_t<std::invoke_result_t<Get>>;
        using Raw = std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>;
        using U = typename Raw::type;
        static_assert(std::is_integral_v<U> && !std::is_same_v<U, bool>, "output_enum needs an enum or integer type");
        add_typed<U>(std::move(name), std::function<U()>([g = std::move(get)] { return static_cast<U>(g()); }), {},
                     Causality::Output, std::move(options), std::move(items), true);
    }

    template <typename T>
    void output(std::string name, const T* value, VariableOptions options = {})
    {
        output(std::move(name), [value] { return *value; }, std::move(options));
    }

    // ---- transport thread ---------------------------------------------------------------

    /// Checks the command against the schema and queues it. Never waits for the step thread.
    /// After finish(): Invalid. The first call completes the schema if nothing did before
    /// (complete_schema(), on the calling thread): the exceptions of the driver's completion
    /// function (e.g. std::invalid_argument for a Peer's monitoring output whose name a variable
    /// has) then come out of submit(), with nothing queued or counted.
    SubmitResult submit(Command command);
    /// Moves the results produced so far into `out` (appends) and frees their slots.
    std::size_t drain_results(std::vector<CommandResult>& out);
    std::size_t drain_events(std::vector<Event>& out);
    /// The latest published sample (sequence 0 before the first one). The step thread hands it
    /// over without a lock.
    [[nodiscard]] Snapshot latest() const;
    /// The variables in registration order. Complete (and fixed) after complete_schema().
    [[nodiscard]] const std::vector<VariableInfo>& schema() const noexcept { return infos_; }
    /// Completes the schema: runs the function given to bind_driver() (a Peer adds its
    /// monitoring outputs there; it reads the driver's set-up), then closes the registration
    /// (input() and output() throw std::logic_error afterwards). Once: later calls return at
    /// once; concurrent calls wait for the first. The function runs on the thread of the call
    /// that completes: a transport calls it on the set-up thread before it starts a thread of
    /// its own or reads schema() (control_silkit::AgentTransport's constructor does); otherwise its
    /// first submit() completes on the transport's thread, which races with a set-up still in
    /// progress. The driver calls it before its first step. If the function throws, the
    /// exception propagates and the schema stays open; what makes it throw must be fixed first
    /// (for a Peer's monitoring clash that means a new Peer: a declared variable cannot be
    /// removed, nor PeerOptions::monitor changed). Not from a step.
    void complete_schema();
    /// The number of TransportLease objects on this agent now.
    [[nodiscard]] std::size_t transports() const noexcept { return transports_.load(std::memory_order_acquire); }
    [[nodiscard]] AgentStats stats() const;
    [[nodiscard]] const AgentOptions& options() const noexcept { return options_; }

    // ---- the driver of the steps ----------------------------------------------------------

    /// Binds the agent to the one driver of its steps (the ECU that owns the agent does it,
    /// see Ecu::agent()). `wakeup`, if set, is called by submit() after a command was queued
    /// (from the transport side), e.g. to wake a real-time driver; it can be cleared later with
    /// clear_wakeup(). It must not call submit() or clear_wakeup(). `complete`, if set, is run
    /// once by complete_schema() before the registration closes, to register what the driver
    /// adds (it may call input() and output()). With `complete`, the driver must call
    /// complete_schema() before its first step (on_step_begin() throws std::logic_error
    /// otherwise). Once only and before the schema is complete (std::logic_error).
    void bind_driver(std::function<void()> wakeup = {}, std::function<void()> complete = {});
    /// Stops calling the wakeup (for example when the driver goes away). Thread safe.
    void clear_wakeup();

    // ---- a transport that sends from the steps ----------------------------------------------

    /// Called at the end of every step, on the step thread, after on_step_end() has sampled:
    /// `now` is the step's start time, `sample` the sample this step took (valid during the call;
    /// its sequence is the one latest() reports), or null if this step took none.
    using StepSink = std::function<void(SimTime now, const Snapshot* sample)>;
    /// Sets the one StepSink of this agent: for a transport that must send what a step produced
    /// (its results, its events, its sample) from within that step, such as
    /// control_silkit::AgentTransport, whose messages then carry the step's time in SIL Kit's time
    /// synchronisation. The sink may call the transport side (drain_results(), drain_events());
    /// it must not block, and what it throws fails the step. Without a sink a step pays one
    /// test for it. Once, on the set-up thread, before the first step and before finish()
    /// (std::logic_error; std::invalid_argument for an empty function). The sink is never removed:
    /// what it refers to must stay valid as long as steps run, or it must stop using it by itself.
    void set_step_sink(StepSink sink);

    // ---- step thread --------------------------------------------------------------------

    /// At the start of the step at `now`, before the application: applies the commands whose
    /// time has come. `now` increases from step to step (std::logic_error otherwise). The first
    /// step closes the registration; std::logic_error if bind_driver() was given a completion
    /// function and complete_schema() has not run.
    void on_step_begin(SimTime now);
    /// At the end of the step at `now`: samples the values (AgentOptions::sample_period).
    void on_step_end(SimTime now);
    /// Queues an event (dropped and counted when the queue is full).
    void log(EventLevel level, std::string text);
    /// After the last step (no step runs any more, and none will): every command still waiting
    /// is Cancelled, and later submit() calls are Invalid.
    void finish();

private:
    struct Variable
    {
        std::function<Value()> get;
        std::function<void(const Value&)> set; ///< empty for outputs
        /// Integer and Enum: the range of the C++ type.
        std::int64_t int_min = std::numeric_limits<std::int64_t>::lowest();
        std::int64_t int_max = std::numeric_limits<std::int64_t>::max();
        /// Real: the largest finite magnitude of the C++ type.
        double real_max = std::numeric_limits<double>::max();
    };
    /// A checked command: the writes resolved to variable indices and converted to their type.
    struct Pending
    {
        std::uint64_t seq = 0;
        std::optional<SimTime> at;
        LatePolicy late = LatePolicy::Reject;
        std::vector<std::pair<std::size_t, Value>> writes;
        bool writes_parameter = false;
    };

    template <typename T>
    void add_typed(std::string name, std::function<T()> get, std::function<void(T)> set, Causality causality,
                   VariableOptions options, std::vector<EnumItem> items, bool is_enum = false);
    void add(VariableInfo info, Variable variable);
    /// std::logic_error (naming `what`) once the schema is complete.
    void require_registering(const std::string& what) const;
    [[nodiscard]] std::optional<std::string> check(const Write& write, std::pair<std::size_t, Value>& out, bool& parameter) const;
    void apply(Pending& command, SimTime now, bool late);
    void push_result(CommandResult result);
    const Snapshot& sample(SimTime now);

    /// A command that takes effect in the current step.
    struct Due
    {
        SimTime at; ///< its time (the current step for a command without one)
        bool late = false;
        Pending command;
    };

    AgentOptions options_;
    std::map<std::string, std::size_t, std::less<>> index_;
    std::vector<VariableInfo> infos_;
    std::vector<Variable> variables_;
    std::atomic<bool> sealed_{false};
    std::atomic<bool> stepped_{false}; ///< the first step has begun
    bool bound_ = false;
    std::mutex schema_mutex_; ///< serialises complete_schema()
    std::atomic<bool> completed_{false}; ///< the schema is complete: no more registration
    std::function<void()> complete_; ///< bind_driver(); run by complete_schema()
    std::atomic<std::size_t> transports_{0}; ///< TransportLease objects on this agent
    friend class TransportLease;

    mutable std::mutex transport_mutex_; ///< serialises the transport side; never taken by the step thread
    std::mutex wakeup_mutex_; ///< guards wakeup_ (submit() and clear_wakeup())
    std::function<void()> wakeup_;

    // transport -> step
    std::mutex finish_mutex_; ///< serialises submit()'s queueing with finish(); the steps never take it
    bool finished_ = false;   ///< guarded by finish_mutex_
    SpscRing<Pending> commands_;
    std::atomic<std::size_t> in_flight_{0};
    // step -> transport
    SpscRing<CommandResult> results_;
    SpscRing<Event> events_;

    // step thread only
    std::vector<Pending> waiting_; ///< not yet due; capacity max_pending
    std::vector<Due> due_;         ///< capacity max_pending
    std::optional<SimTime> last_step_;
    std::optional<SimTime> next_sample_;
    StepSink step_sink_; ///< set before the first step, then read by the step thread only

    // Values: a triple buffer. The step thread fills buffers_[back_] and swaps it with the
    // middle one; latest() swaps the middle one with buffers_[front_] when it is newer.
    static constexpr unsigned kFresh = 4; ///< flag in middle_: the middle buffer is newer than the front one
    mutable std::array<Snapshot, 3> buffers_;
    mutable std::atomic<unsigned> middle_{1};
    unsigned back_ = 0;         ///< step thread only
    std::uint64_t sample_sequence_ = 0; ///< step thread only
    mutable unsigned front_ = 2; ///< guarded by transport_mutex_

    struct AtomicStats
    {
        std::atomic<std::uint64_t> submitted{0}, busy{0}, invalid{0}, applied{0}, late{0}, rejected{0}, cancelled{0},
            events_dropped{0};
    };
    AtomicStats stats_;
};

/// Marks a transport as attached to an agent for as long as it lives (Agent::transports()). The
/// owner of the agent checks that none is left when it destroys the agent: an ECU stops the
/// process with a message if one is (the transport would use a destroyed agent). A transport
/// with a thread of its own makes one before the thread starts and destroys it after the thread
/// ended; control_silkit::AgentTransport holds one from its constructor to its destructor.
class TransportLease
{
public:
    explicit TransportLease(Agent& agent) noexcept : agent_(agent) { agent_.transports_.fetch_add(1, std::memory_order_acq_rel); }
    ~TransportLease() { agent_.transports_.fetch_sub(1, std::memory_order_acq_rel); }
    TransportLease(const TransportLease&) = delete;
    TransportLease& operator=(const TransportLease&) = delete;

private:
    Agent& agent_;
};

namespace detail {

template <typename T>
constexpr ValueType value_type_of()
{
    if constexpr (std::is_same_v<T, bool>)
    {
        return ValueType::Bool;
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return ValueType::Integer;
    }
    else
    {
        static_assert(std::is_floating_point_v<T>, "a control variable is bool, an integer or a floating-point type");
        return ValueType::Real;
    }
}

template <typename T>
Value to_value(T v)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        return Value{v};
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t))
    {
        // Read only (inputs of this type are refused): saturated at the largest int64.
        constexpr auto kMax = static_cast<T>(std::numeric_limits<std::int64_t>::max());
        return Value{static_cast<std::int64_t>(v > kMax ? kMax : v)};
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return Value{static_cast<std::int64_t>(v)};
    }
    else
    {
        return Value{static_cast<double>(v)};
    }
}

/// The value converted by Agent::check() to the variable's own Value alternative.
template <typename T>
T from_value(const Value& v)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        return std::get<bool>(v);
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return static_cast<T>(std::get<std::int64_t>(v));
    }
    else
    {
        return static_cast<T>(std::get<double>(v));
    }
}

} // namespace detail

template <typename T>
void Agent::add_typed(std::string name, std::function<T()> get, std::function<void(T)> set, Causality causality,
                      VariableOptions options, std::vector<EnumItem> items, bool is_enum)
{
    VariableInfo info;
    info.name = std::move(name);
    info.type = is_enum ? ValueType::Enum : detail::value_type_of<T>();
    info.causality = causality;
    info.unit = std::move(options.unit);
    info.min = options.min;
    info.max = options.max;
    info.step = options.step;
    info.description = std::move(options.description);
    info.items = std::move(items);
    Variable variable;
    variable.get = [g = std::move(get)] { return detail::to_value<T>(g()); };
    if (set)
    {
        variable.set = [s = std::move(set)](const Value& v) { s(detail::from_value<T>(v)); };
    }
    if constexpr (std::is_floating_point_v<T>)
    {
        // long double: limited to what a double (the value type) can hold.
        variable.real_max = std::numeric_limits<T>::max() > static_cast<T>(std::numeric_limits<double>::max())
                                ? std::numeric_limits<double>::max()
                                : static_cast<double>(std::numeric_limits<T>::max());
    }
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    {
        if constexpr (std::is_signed_v<T>)
        {
            variable.int_min = std::numeric_limits<T>::lowest();
            variable.int_max = std::numeric_limits<T>::max();
        }
        else
        {
            variable.int_min = 0;
            variable.int_max = static_cast<std::int64_t>(std::numeric_limits<T>::max());
        }
    }
    add(std::move(info), std::move(variable));
}

} // namespace vehicle_sil::control
