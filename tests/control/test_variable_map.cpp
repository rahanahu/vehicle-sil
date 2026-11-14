// SPDX-License-Identifier: MIT
//
// The variable map: the seen schema and the conversions (MappedSchema), and where the hub applies
// them: the schema of a connection, every sample before a reader sees it, a write before it goes
// to the agent, and the agent's reasons (no transport, no SIL Kit).
#include "test_harness.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/variable_map.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;

VariableInfo info(std::string name, ValueType type, Causality causality = Causality::Input)
{
    VariableInfo v;
    v.name = std::move(name);
    v.type = type;
    v.causality = causality;
    return v;
}

/// What an executable declares: a speed in tenths of rpm, a temperature in kelvin, a gear as a raw
/// number, a lamp, and two outputs.
std::vector<VariableInfo> executable()
{
    VariableInfo rpm = info("rpm_x10", ValueType::Integer);
    rpm.unit = "0.1 rpm";
    rpm.min = 0;
    rpm.max = 80000;
    rpm.step = 5;
    rpm.description = "engine speed";
    VariableInfo temp = info("temp_k", ValueType::Real);
    temp.unit = "K";
    temp.min = 233.15;
    temp.max = 423.15;
    VariableInfo gear = info("gear_raw", ValueType::Integer);
    gear.min = 0;
    gear.max = 3;
    VariableInfo mode = info("mode", ValueType::Enum);
    mode.items = {{"OFF", 0}, {"ECO", 1}, {"SPORT", 2}};
    return {rpm, temp, gear, mode, info("lamp", ValueType::Bool), info("boost", ValueType::Real, Causality::Output),
            info("count", ValueType::Integer, Causality::Output)};
}

VariableMapping renamed(std::string name, std::string from)
{
    VariableMapping m;
    m.name = std::move(name);
    m.from = std::move(from);
    return m;
}

VariableMapping linear(std::string name, std::string from, double scale, double offset = 0.0)
{
    VariableMapping m = renamed(std::move(name), std::move(from));
    m.scale = scale;
    m.offset = offset;
    return m;
}

VariableMapping named(std::string name, std::string from, std::vector<EnumItem> items)
{
    VariableMapping m = renamed(std::move(name), std::move(from));
    m.map = std::move(items);
    return m;
}

VariableMapping absent(std::string name)
{
    VariableMapping m;
    m.name = std::move(name);
    m.absent = true;
    return m;
}

VariableInfo seen(const MappedSchema& s, const std::string& name)
{
    for (const auto& v : s.schema())
    {
        if (v.name == name)
        {
            return v;
        }
    }
    throw vehicle_sil::unit::Failure("no variable " + name);
}

std::string problems(const MappedSchema& s)
{
    std::string out;
    for (const auto& p : s.problems())
    {
        out += std::to_string(p.entry) + " " + std::to_string(static_cast<int>(p.kind)) + " " + p.message + "\n";
    }
    return out;
}

/// The write as the executable gets it ("name=value"), or "refused: <reason>".
std::string written(const MappedSchema& s, const std::string& name, Value value)
{
    Write w{name, value};
    if (const auto refused = s.to_actual(w))
    {
        return "refused: " + *refused;
    }
    char text[64];
    if (const auto* b = std::get_if<bool>(&w.value))
    {
        return w.name + "=" + (*b ? "true" : "false");
    }
    if (const auto* i = std::get_if<std::int64_t>(&w.value))
    {
        return w.name + "=" + std::to_string(*i);
    }
    std::snprintf(text, sizeof(text), "%.10g", std::get<double>(w.value));
    return w.name + "=" + text + " (real)";
}

/// Throws a Failure that shows both texts if they differ.
void same(const std::string& actual, const std::string& expected)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure("texts differ:\n--- got ---\n" + actual + "\n--- expected ---\n" + expected + "\n---");
    }
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

} // namespace

TEST_CASE(a_variable_map_gives_the_seen_schema_in_the_executables_order)
{
    VariableMapping celsius = linear("coolant.temperature", "temp_k", 1.0, -273.15);
    celsius.unit = "degC";
    VariableMapping lamp = renamed("cabin.lamp", "lamp");
    lamp.unit = "on/off";
    const MappedSchema s = MappedSchema::make(
        executable(), {linear("engine.speed", "rpm_x10", 0.1), celsius, named("gear", "gear_raw", {{"P", 0}, {"D", 3}}), lamp,
                       absent("engine.turbo")});
    same(problems(s), std::string());
    std::string names;
    for (const auto& v : s.schema())
    {
        names += v.name + " ";
    }
    same(names, std::string("engine.speed coolant.temperature gear mode cabin.lamp boost count "));
    CHECK((s.sources() == std::vector<std::string>{"rpm_x10", "temp_k", "gear_raw", "", "lamp", "", ""}));

    // A linear conversion: a real, with the range, the step and the unit of the seen value.
    const VariableInfo speed = seen(s, "engine.speed");
    CHECK(speed.type == ValueType::Real);
    CHECK(speed.causality == Causality::Input);
    CHECK(near(*speed.min, 0.0) && near(*speed.max, 8000.0) && near(*speed.step, 0.5));
    same(speed.unit, std::string()); // "0.1 rpm" is not the unit of the seen value
    same(speed.description, std::string("engine speed"));
    const VariableInfo temperature = seen(s, "coolant.temperature");
    CHECK(near(*temperature.min, -40.0) && near(*temperature.max, 150.0));
    same(temperature.unit, std::string("degC"));
    // Names for values: an enum with the map's items; the range stays.
    const VariableInfo gear = seen(s, "gear");
    CHECK(gear.type == ValueType::Enum);
    CHECK((gear.items == std::vector<EnumItem>{{"P", 0}, {"D", 3}}));
    CHECK(near(*gear.max, 3.0));
    // A name only: everything else as declared, but a unit that is given.
    CHECK(seen(s, "cabin.lamp").type == ValueType::Bool);
    same(seen(s, "cabin.lamp").unit, std::string("on/off"));
    // Not listed: as declared.
    CHECK(seen(s, "mode") == executable()[3]);
}

TEST_CASE(a_negative_scale_swaps_the_ends_of_the_range)
{
    VariableInfo v = info("raw", ValueType::Real);
    v.min = 10;
    v.step = 2;
    const MappedSchema s = MappedSchema::make({v}, {linear("x", "raw", -2.0, 100.0)});
    CHECK(!s.schema()[0].min.has_value());
    CHECK(near(*s.schema()[0].max, 80.0)); // the executable's least value is the greatest seen
    CHECK(near(*s.schema()[0].step, 4.0));
    same(written(s, "x", 80.0), std::string("raw=10 (real)"));
    same(written(s, "x", 60.0), std::string("raw=20 (real)"));
    same(written(s, "x", 81.0), std::string("refused: x: 81 is out of range"));
}

TEST_CASE(a_sample_is_converted_place_by_place)
{
    const MappedSchema s = MappedSchema::make(
        executable(), {linear("engine.speed", "rpm_x10", 0.1), linear("coolant.temperature", "temp_k", 1.0, -273.15),
                       named("gear", "gear_raw", {{"P", 0}, {"D", 3}}), linear("boost.kpa", "boost", 100.0, 5.0),
                       linear("ticks", "count", 2.0, 1.0)});
    Snapshot sample;
    sample.time = SimTime{} + 30ms;
    sample.sequence = 7;
    sample.values = {std::int64_t{8005}, 373.15, std::int64_t{3}, std::int64_t{2}, true, 1.25, std::int64_t{42}};
    s.to_seen(sample);
    CHECK(sample.time == SimTime{} + 30ms);
    CHECK_EQ(sample.sequence, std::uint64_t{7});
    CHECK(near(std::get<double>(sample.values[0]), 800.5)); // integer * scale: a real
    CHECK(near(std::get<double>(sample.values[1]), 100.0)); // offset after scale
    CHECK_EQ(std::get<std::int64_t>(sample.values[2]), std::int64_t{3}); // a map keeps the number
    CHECK_EQ(std::get<std::int64_t>(sample.values[3]), std::int64_t{2});
    CHECK(std::get<bool>(sample.values[4]));
    CHECK(near(std::get<double>(sample.values[5]), 130.0)); // scale, then offset
    CHECK(near(std::get<double>(sample.values[6]), 85.0)); // an integer too: scale, then offset
    // A sample of another schema is left alone.
    Snapshot other;
    other.values = {std::int64_t{8005}};
    s.to_seen(other);
    CHECK_EQ(std::get<std::int64_t>(other.values[0]), std::int64_t{8005});
}

TEST_CASE(a_write_becomes_the_executables_name_and_value)
{
    const MappedSchema s = MappedSchema::make(
        executable(), {linear("engine.speed", "rpm_x10", 0.1), linear("coolant.temperature", "temp_k", 1.0, -273.15),
                       named("gear", "gear_raw", {{"P", 0}, {"D", 3}}), renamed("cabin.lamp", "lamp")});
    // The inverse of the conversion; an integer of the executable is the nearest one.
    same(written(s, "engine.speed", 800.5), std::string("rpm_x10=8005"));
    same(written(s, "engine.speed", std::int64_t{800}), std::string("rpm_x10=8000"));
    same(written(s, "engine.speed", 800.04), std::string("rpm_x10=8000"));
    same(written(s, "engine.speed", 800.06), std::string("rpm_x10=8001"));
    same(written(s, "coolant.temperature", 100.0), std::string("temp_k=373.15 (real)"));
    // What was read can be written back.
    Snapshot sample;
    sample.values = {std::int64_t{12345}, 300.0, std::int64_t{0}, std::int64_t{0}, false, 0.0, std::int64_t{0}};
    s.to_seen(sample);
    same(written(s, "engine.speed", sample.values[0]), std::string("rpm_x10=12345"));
    same(written(s, "coolant.temperature", sample.values[1]), std::string("temp_k=300 (real)"));
    // The ends of the range are in it, although the seen ends are products (rounded).
    same(written(s, "engine.speed", 8000.0), std::string("rpm_x10=80000"));
    same(written(s, "engine.speed", 0.0), std::string("rpm_x10=0"));
    same(written(s, "coolant.temperature", *seen(s, "coolant.temperature").max), std::string("temp_k=423.15 (real)"));
    same(written(s, "coolant.temperature", *seen(s, "coolant.temperature").min), std::string("temp_k=233.15 (real)"));
    same(written(s, "coolant.temperature", 150.0), std::string("temp_k=423.15 (real)"));
    // Beyond them: refused, in the seen name and value.
    same(written(s, "engine.speed", 8000.1), std::string("refused: engine.speed: 8000.1 is out of range"));
    same(written(s, "engine.speed", -0.1), std::string("refused: engine.speed: -0.1 is out of range"));
    same(written(s, "coolant.temperature", 150.001), std::string("refused: coolant.temperature: 150.001 is out of range"));
    same(written(s, "coolant.temperature", -40.001), std::string("refused: coolant.temperature: -40.001 is out of range"));
    same(written(s, "engine.speed", true), std::string("refused: engine.speed: needs a number"));
    same(written(s, "engine.speed", std::numeric_limits<double>::quiet_NaN()),
             std::string("refused: engine.speed: needs a finite number"));
    same(written(s, "engine.speed", std::numeric_limits<double>::infinity()),
             std::string("refused: engine.speed: needs a finite number"));
    // Names for values: the number as it is, and only a value that has a name.
    same(written(s, "gear", std::int64_t{3}), std::string("gear_raw=3"));
    same(written(s, "gear", 3.0), std::string("gear_raw=3 (real)")); // the agent takes a whole real
    same(written(s, "gear", std::int64_t{1}), std::string("refused: gear: 1 is not one of its values"));
    same(written(s, "gear", 2.0), std::string("refused: gear: 2 is not one of its values"));
    // A name only, and a variable without a mapping (left to the agent, also a wrong type).
    same(written(s, "cabin.lamp", true), std::string("lamp=true"));
    same(written(s, "cabin.lamp", 2.5), std::string("lamp=2.5 (real)"));
    same(written(s, "mode", std::int64_t{9}), std::string("mode=9"));
    // A name that was replaced is no longer a name (the hub refuses it before: not in the schema).
    same(written(s, "rpm_x10", std::int64_t{1}), std::string("rpm_x10=1"));
}

TEST_CASE(a_linear_conversion_without_a_range_keeps_integers_within_their_type)
{
    const MappedSchema s = MappedSchema::make({info("raw", ValueType::Integer)}, {linear("x", "raw", 0.5)});
    same(written(s, "x", 1.0), std::string("raw=2"));
    same(written(s, "x", 4e18), std::string("raw=8000000000000000000"));
    same(written(s, "x", 5e18), std::string("refused: x: 5e+18 is out of range")); // 1e19 is not an int64
    same(written(s, "x", 1e300), std::string("refused: x: 1e+300 is out of range"));
}

TEST_CASE(a_value_for_an_integer_of_the_executable_is_the_nearest_one_a_half_away_from_zero)
{
    VariableInfo raw = info("raw", ValueType::Integer);
    raw.min = -10;
    raw.max = 10;
    const MappedSchema s = MappedSchema::make({raw}, {linear("x", "raw", 0.5, 10.0)});
    same(written(s, "x", 12.0), "raw=4"); // (12 - 10) / 0.5: the offset first, then the scale
    same(written(s, "x", 12.2), "raw=4");
    same(written(s, "x", 12.3), "raw=5");
    // Exactly between two integers: away from zero, on both sides of it.
    same(written(s, "x", 10.25), "raw=1");
    same(written(s, "x", 10.75), "raw=2");
    same(written(s, "x", 11.25), "raw=3");
    same(written(s, "x", 9.75), "raw=-1");
    same(written(s, "x", 9.25), "raw=-2");
    same(written(s, "x", 10.24), "raw=0");
    same(written(s, "x", 9.76), "raw=0");
    // The range is the executable's, after the rounding: 15.2 is 10, 15.3 would be 11.
    same(written(s, "x", 15.2), "raw=10");
    same(written(s, "x", 15.3), "refused: x: 15.3 is out of range");
    CHECK(s.in_range("x", 15.2) == std::optional<bool>(true));
    CHECK(s.in_range("x", 15.3) == std::optional<bool>(false));
    CHECK(!s.in_range("raw", 1.0).has_value()); // not a seen name
}

TEST_CASE(the_seen_ends_of_a_range_are_the_numbers_one_would_write_and_the_executable_decides)
{
    // 4.35 * 100 is 434.99999999999994 and 3 * 0.1 is 0.30000000000000004 in doubles.
    VariableInfo bar = info("pressure_bar", ValueType::Real);
    bar.min = 0.0;
    bar.max = 4.35;
    VariableInfo raw = info("raw", ValueType::Integer);
    raw.min = 3;
    raw.max = 10;
    const MappedSchema s = MappedSchema::make({bar, raw, info("lamp", ValueType::Bool)},
                                              {linear("pressure", "pressure_bar", 100.0), linear("level", "raw", 0.1),
                                               renamed("cabin.lamp", "lamp")});
    CHECK(seen(s, "pressure").max == std::optional<double>(435.0));
    CHECK(seen(s, "level").min == std::optional<double>(0.3));
    CHECK(seen(s, "level").max == std::optional<double>(1.0));
    same(written(s, "pressure", 435.0), "pressure_bar=4.35 (real)");
    same(written(s, "level", 0.3), "raw=3");
    CHECK(s.in_range("pressure", 435.0) == std::optional<bool>(true));
    CHECK(s.in_range("pressure", 435.001) == std::optional<bool>(false));
    CHECK(s.in_range("pressure", 0.0) == std::optional<bool>(true));
    CHECK(s.in_range("pressure", -0.001) == std::optional<bool>(false));
    CHECK(s.in_range("level", 0.3) == std::optional<bool>(true));
    CHECK(s.in_range("level", 0.24) == std::optional<bool>(false));
    // Without a linear conversion the map does not decide.
    CHECK(!s.in_range("cabin.lamp", 1.0).has_value());
}

TEST_CASE(a_variable_that_is_seen_under_another_name_may_be_absent_under_its_own)
{
    const MappedSchema moved = MappedSchema::make(executable(), {absent("boost"), renamed("turbo.boost", "boost")});
    same(problems(moved), "");
    // But not a name that a mapping gives, or that the executable still has.
    const MappedSchema given = MappedSchema::make(executable(), {renamed("turbo", "boost"), absent("turbo"), absent("lamp")});
    same(problems(given), "1 5 turbo: it says absent: true, but the executable's 'boost' is mapped to this name: remove one "
                          "of the two entries\n"
                          "2 5 lamp: it says absent: true, but the executable has a variable 'lamp': remove this entry (a "
                          "variable that is not listed is seen under its own name)\n");
}

TEST_CASE(the_names_of_a_mapping_that_was_not_applied_are_not_writable)
{
    // "boost" could not become "lamp" (the executable has a lamp), and "torque" is not there.
    const MappedSchema s = MappedSchema::make(
        executable(), {linear("lamp", "boost", 2.0), renamed("engine.torque", "torque"), renamed("ticks", "count")});
    CHECK_EQ(s.problems().size(), std::size_t{2});
    const std::string why = "the mapping lamp of this participant's variables was not applied (lamp: another variable of the "
                            "executable is seen under the name 'lamp' already: choose another name here, or map that "
                            "variable to another name too), so this name is not what the configuration says it is: correct "
                            "variables in the configuration and run again";
    CHECK(s.not_writable("lamp") != nullptr && s.not_writable("boost") != nullptr);
    same(*s.not_writable("lamp"), why);  // the executable's own lamp, not the scaled boost
    same(*s.not_writable("boost"), why); // boost, but without the conversion
    CHECK(s.not_writable("engine.torque") != nullptr && s.not_writable("torque") != nullptr);
    CHECK(s.not_writable("ticks") == nullptr && s.not_writable("count") == nullptr && s.not_writable("mode") == nullptr);
    CHECK(MappedSchema::make(executable(), {renamed("ticks", "count")}).not_writable("ticks") == nullptr);
}

TEST_CASE(mappings_that_do_not_fit_are_left_out_and_said)
{
    VariableMapping both = linear("z", "rpm_x10", 2.0);
    both.map = std::vector<EnumItem>{{"a", 1}};
    VariableMapping zero = linear("z0", "rpm_x10", 0.0);
    VariableMapping nameless;
    nameless.name = "nowhere";
    VariableMapping absent_from = absent("engine.turbo");
    absent_from.from = "boost";
    const MappedSchema s = MappedSchema::make(
        executable(), {renamed("engine.speed", "rpm"),                         // 0: no such variable
                       linear("cabin.lamp", "lamp", 2.0),                     // 1: a bool is not a number
                       linear("drive.mode", "mode", 1.0, 1.0),                // 2: an enum is not a number
                       named("boost.level", "boost", {{"low", 0}}),           // 3: a real has no names
                       named("gear", "gear_raw", {{"P", 0}, {"D", 4}}),       // 4: 4 is out of range
                       named("drive", "mode", {{"off", 0}, {"race", 3}}),     // 5: 3 is not an item
                       renamed("temperature", "temp_k"),                       // 6: fits
                       renamed("temperature.2", "temp_k"),                     // 7: temp_k twice
                       renamed("lamp", "count"),                               // 8: lamp is another variable's name
                       absent("boost"),                                       // 9: it is there
                       both, zero, nameless, absent_from,                     // 10-13: not a form
                       absent("engine.turbo")});                              // 14: fits
    same(problems(s),
             std::string("0 1 engine.speed: the executable has no variable 'rpm'\n"
                         "1 2 cabin.lamp: scale and offset need a number, and the executable's 'lamp' is a bool: remove them\n"
                         "2 2 drive.mode: scale and offset need a number, and the executable's 'mode' is an enum: remove "
                         "them (map gives its values other names)\n"
                         "3 2 boost.level: map names the values of an integer or an enum, and the executable's 'boost' is a "
                         "real: remove map\n"
                         "4 3 gear: map gives D the value 4, which the executable's 'gear_raw' cannot take (its range is 0 "
                         "to 3): correct the value\n"
                         "5 3 drive: map gives race the value 3, which the executable's 'mode' cannot take (its values are "
                         "OFF (0), ECO (1), SPORT (2)): correct the value\n"
                         "7 4 temperature.2: the executable's 'temp_k' is mapped twice (also as temperature): keep one of "
                         "the two\n"
                         "8 4 lamp: another variable of the executable is seen under the name 'lamp' already: choose "
                         "another name here, or map that variable to another name too\n"
                         "9 5 boost: it says absent: true, but the executable has a variable 'boost': remove this entry (a "
                         "variable that is not listed is seen under its own name)\n"
                         "10 0 z: scale and offset cannot be combined with map: use one of the two\n"
                         "11 0 z0: scale must be a number other than 0 and offset a number (the value seen is the "
                         "executable's value * scale + offset)\n"
                         "12 0 nowhere: it does not name the executable's variable: add from: <the executable's name of it>\n"
                         "13 0 engine.turbo: absent: true takes no other key: remove the others, or absent\n"));
    // Only the one that fits applies; the schema has every name once.
    std::string names;
    for (const auto& v : s.schema())
    {
        names += v.name + " ";
    }
    same(names, std::string("rpm_x10 temperature gear_raw mode lamp boost count "));
    CHECK(seen(s, "gear_raw").type == ValueType::Integer);
}

TEST_CASE(names_may_be_swapped_and_a_name_that_is_taken_gives_way)
{
    // Swapped: each name is free once the other variable has moved.
    const MappedSchema swapped = MappedSchema::make(executable(), {renamed("boost", "count"), renamed("count", "boost")});
    same(problems(swapped), std::string());
    CHECK(seen(swapped, "boost").type == ValueType::Integer);
    CHECK(seen(swapped, "count").type == ValueType::Real);
    same(swapped.reason_to_seen("count: 3 does not fit its type"),
             std::string("boost (the executable's count): 3 does not fit its type"));
    // A chain: "lamp -> x" is fine, "boost -> lamp" too (lamp has moved); "count -> boost" then
    // "y -> boost's old place" does not exist, so nothing else breaks.
    const MappedSchema chain = MappedSchema::make(executable(), {renamed("x", "lamp"), renamed("lamp", "boost")});
    same(problems(chain), std::string());
    // Leaving one out gives its variable the old name back, which another mapping wanted.
    const MappedSchema undone =
        MappedSchema::make(executable(), {renamed("lamp", "boost"), linear("x", "lamp", 2.0) /* a bool: left out */});
    CHECK_EQ(undone.problems().size(), std::size_t{2});
    CHECK(undone.problems()[0].kind == MapProblemKind::Collision); // lamp is lamp again
    CHECK(undone.problems()[1].kind == MapProblemKind::Type);
    CHECK((undone.schema() == executable()));
}

TEST_CASE(an_agents_reason_gets_the_seen_name)
{
    const MappedSchema s = MappedSchema::make(executable(), {linear("engine.speed", "rpm_x10", 0.1), linear("temp_k", "temp_k", 2.0)});
    same(s.reason_to_seen("rpm_x10: 70000 does not fit its type"),
             std::string("engine.speed (the executable's rpm_x10): 70000 does not fit its type"));
    same(s.reason_to_seen("temp_k: 1 is out of range"), std::string("temp_k: 1 is out of range")); // the same name
    same(s.reason_to_seen("lamp: needs a bool"), std::string("lamp: needs a bool"));
    same(s.reason_to_seen("too many commands in flight"), std::string("too many commands in flight"));
    same(s.reason_to_seen(""), std::string());
}

// --- in the hub ---------------------------------------------------------------------------------

namespace {

/// An agent with the variables of an executable, linked to the hub in the process.
struct Executable
{
    struct Link final : AgentLink
    {
        std::mutex mutex;
        std::deque<Command> queue;
        bool send(const Command& command) override
        {
            const std::lock_guard lock(mutex);
            queue.push_back(command);
            return true;
        }
    };

    Hub& hub;
    std::string incarnation;
    std::int32_t rpm_x10 = 0;
    double temp_k = 300.0;
    std::int8_t gear_raw = 0;
    double boost = 0.0;
    Agent agent{AgentOptions{.sample_period = SimDuration::zero()}};
    std::shared_ptr<Link> link = std::make_shared<Link>();
    ConnectionId connection = 0;
    std::vector<Command> received; ///< as the agent got them

    Executable(Hub& h, std::string inc) : hub(h), incarnation(std::move(inc))
    {
        agent.input("rpm_x10", &rpm_x10, {.unit = "0.1 rpm", .min = 0.0, .max = 80000.0});
        agent.input("temp_k", &temp_k, {.unit = "K"});
        agent.input("gear_raw", &gear_raw);
        agent.output("boost", &boost);
        connection = hub.agent_connected(PeerHello{"Engine", incarnation, agent.schema(), 8}, link);
    }

    void step(SimTime now)
    {
        std::deque<Command> commands;
        {
            const std::lock_guard lock(link->mutex);
            commands.swap(link->queue);
        }
        for (auto& c : commands)
        {
            received.push_back(c);
            const auto seq = c.seq;
            hub.agent_submitted(connection, seq, agent.submit(std::move(c)));
        }
        agent.on_step_begin(now);
        boost = static_cast<double>(rpm_x10) / 40000.0;
        agent.on_step_end(now);
        std::vector<CommandResult> results;
        agent.drain_results(results);
        for (const auto& r : results)
        {
            hub.agent_result(connection, r);
        }
        hub.agent_values(connection, agent.latest());
    }
};

VariableMap engine_map()
{
    return {linear("engine.speed", "rpm_x10", 0.1), linear("coolant.temperature", "temp_k", 1.0, -273.15),
            named("gear", "gear_raw", {{"P", 0}, {"D", 3}}), linear("boost.kpa", "boost", 100.0)};
}

OperationRequest request(std::vector<PeerWrite> writes, std::optional<SimTime> when = std::nullopt)
{
    return OperationRequest{std::move(writes), when, LatePolicy::Reject, "test"};
}

double real_at(const Snapshot& sample, std::size_t i)
{
    return std::get<double>(sample.values.at(i));
}

} // namespace

TEST_CASE(control_hub_shows_a_mapped_peer_under_the_seen_names_to_every_reader)
{
    Hub hub(HubOptions{.first_seq = 1});
    hub.set_variable_map("Engine", engine_map());
    std::vector<std::shared_ptr<const Snapshot>> tapped;
    hub.set_values_tap([&](const std::string&, const std::string&, const std::shared_ptr<const Snapshot>& sample) {
        tapped.push_back(sample);
    });
    Executable engine(hub, "a");
    const auto subscription = hub.subscribe();

    const PeerInfo peer = hub.peers().at(0);
    std::string names;
    for (const auto& v : peer.schema)
    {
        names += v.name + " ";
    }
    same(names, std::string("engine.speed coolant.temperature gear boost.kpa "));
    CHECK((peer.sources == std::vector<std::string>{"rpm_x10", "temp_k", "gear_raw", "boost"}));
    CHECK(peer.map_problems.empty());
    CHECK(peer.schema[0].type == ValueType::Real);

    engine.rpm_x10 = 20000;
    engine.gear_raw = 3;
    engine.step(SimTime{} + 10ms);
    // The latest sample, the tap and the subscription share one converted sample.
    const auto latest = hub.latest("Engine");
    CHECK(latest != nullptr);
    CHECK(latest->time == SimTime{} + 10ms);
    CHECK(near(real_at(*latest, 0), 2000.0));
    CHECK(near(real_at(*latest, 1), 300.0 - 273.15));
    CHECK_EQ(std::get<std::int64_t>(latest->values[2]), std::int64_t{3});
    CHECK(near(real_at(*latest, 3), 50.0));
    CHECK_EQ(tapped.size(), std::size_t{1});
    CHECK(tapped[0] == latest);
    CHECK(hub.latest_of({"Engine"}).at(0).snapshot == latest);
    bool subscribed = false;
    for (const Update& update : subscription->poll(0, 0ms))
    {
        if (const auto* values = std::get_if<ValuesUpdate>(&update))
        {
            CHECK(values->snapshot == latest);
            subscribed = true;
        }
        if (const auto* announced = std::get_if<PeerUpdate>(&update))
        {
            same(announced->peer.schema[0].name, std::string("engine.speed"));
            same(announced->peer.sources[0], std::string("rpm_x10"));
        }
    }
    CHECK(subscribed);
    // Every sample, in the order taken, each converted once.
    engine.rpm_x10 = 30000;
    engine.step(SimTime{} + 20ms);
    CHECK_EQ(tapped.size(), std::size_t{2});
    CHECK(near(real_at(*tapped[0], 0), 2000.0));
    CHECK(near(real_at(*tapped[1], 0), 3000.0));
    CHECK(tapped[1]->sequence > tapped[0]->sequence);
}

TEST_CASE(control_hub_writes_a_mapped_peer_in_the_executables_names_and_values)
{
    Hub hub(HubOptions{.first_seq = 1});
    hub.set_variable_map("Engine", engine_map());
    Executable engine(hub, "a");
    engine.step(SimTime{} + 10ms);

    // At a time: converted when it is written, applied in the step it is for.
    const auto seq = hub.write(request({{"Engine", "engine.speed", 1234.5}, {"Engine", "coolant.temperature", 90.0},
                                        {"Engine", "gear", std::int64_t{3}}},
                                       SimTime{} + 30ms));
    engine.step(SimTime{} + 20ms);
    CHECK_EQ(engine.rpm_x10, 0);
    CHECK_EQ(engine.received.size(), std::size_t{1});
    same(engine.received[0].writes.at(0).name, std::string("rpm_x10"));
    CHECK_EQ(std::get<std::int64_t>(engine.received[0].writes.at(0).value), std::int64_t{12345});
    same(engine.received[0].writes.at(1).name, std::string("temp_k"));
    same(engine.received[0].writes.at(2).name, std::string("gear_raw"));
    engine.step(SimTime{} + 30ms);
    CHECK_EQ(engine.rpm_x10, 12345);
    CHECK(near(engine.temp_k, 363.15));
    CHECK_EQ(static_cast<int>(engine.gear_raw), 3);
    const OperationResult done = *hub.operation(seq);
    CHECK(done.targets.at(0).status == TargetStatus::Applied);
    CHECK(done.targets.at(0).applied_at == SimTime{} + 30ms);
    // The record keeps what the client asked for.
    same(done.request.writes.at(0).name, std::string("engine.speed"));
    CHECK(near(std::get<double>(done.request.writes.at(0).value), 1234.5));
    // And it reads back as written.
    CHECK(near(real_at(*hub.latest("Engine"), 0), 1234.5));
    CHECK(near(real_at(*hub.latest("Engine"), 1), 90.0));

    SimTime now = SimTime{} + 30ms;
    const auto refused = [&](PeerWrite write) {
        const auto s = hub.write(request({std::move(write)}));
        engine.step(now += 10ms);
        const TargetResult t = hub.operation(s)->targets.at(0);
        return std::string(t.status == TargetStatus::Invalid ? "invalid: " : "other: ") + t.reason;
    };
    const std::size_t before = engine.received.size();
    // The executable's own name is not a name any more.
    same(refused({"Engine", "rpm_x10", std::int64_t{1}}), std::string("invalid: unknown variable 'rpm_x10'"));
    same(refused({"Engine", "engine.speed", 9000.0}), std::string("invalid: engine.speed: 9000 is out of range"));
    same(refused({"Engine", "gear", std::int64_t{1}}), std::string("invalid: gear: 1 is not one of its values"));
    same(refused({"Engine", "boost.kpa", 1.0}), std::string("invalid: boost.kpa: an output is not writable"));
    CHECK_EQ(engine.received.size(), before); // none of them reached the agent
    // One bad write refuses the whole command, also after a good one.
    same(refused({"Engine", "engine.speed", 9000.0}), std::string("invalid: engine.speed: 9000 is out of range"));
    const auto mixed = hub.write(request({{"Engine", "coolant.temperature", 20.0}, {"Engine", "engine.speed", -1.0}}));
    engine.step(now += 10ms);
    CHECK(hub.operation(mixed)->targets.at(0).status == TargetStatus::Invalid);
    CHECK(near(engine.temp_k, 363.15));
    // What only the agent knows (the C++ type of gear_raw... here: a bool for a number) comes
    // back in the seen name.
    same(refused({"Engine", "gear", true}), std::string("invalid: gear (the executable's gear_raw): needs an integer"));
}

TEST_CASE(control_hub_refuses_a_write_to_a_variable_whose_mapping_was_not_applied)
{
    // The map wants "temp_k" to be the scaled boost, but the executable has a temp_k of its own:
    // the mapping is left out, and temp_k is the executable's temperature, boost the raw boost.
    Hub hub(HubOptions{.first_seq = 1});
    hub.set_variable_map("Engine", {linear("temp_k", "boost", 2.0), linear("engine.speed", "rpm_x10", 0.1)});
    Executable engine(hub, "a");
    CHECK_EQ(hub.peers().at(0).map_problems.size(), std::size_t{1});
    engine.step(SimTime{} + 10ms);
    const auto status = [&](PeerWrite write) {
        const auto seq = hub.write(request({std::move(write)}));
        const TargetResult t = hub.operation(seq)->targets.at(0);
        return std::string(t.status == TargetStatus::Invalid ? "invalid: " : "other: ") + t.reason.substr(0, t.reason.find('('));
    };
    // Neither name of the mapping is written: a client that means the mapped variable would put
    // its value into another variable, or without the conversion.
    same(status({"Engine", "temp_k", 350.0}),
         "invalid: temp_k: not written: the mapping temp_k of this participant's variables was not applied ");
    same(status({"Engine", "boost", 1.0}),
         "invalid: boost: not written: the mapping temp_k of this participant's variables was not applied ");
    // One such write refuses the whole command.
    const auto mixed = hub.write(request({{"Engine", "engine.speed", 100.0}, {"Engine", "temp_k", 350.0}}));
    CHECK(hub.operation(mixed)->targets.at(0).status == TargetStatus::Invalid);
    engine.step(SimTime{} + 20ms);
    CHECK(engine.received.empty());
    CHECK(near(engine.temp_k, 300.0));
    CHECK_EQ(engine.rpm_x10, 0);
    // They are read, as the executable announced them; the mapping that fits works.
    const PeerInfo peer = hub.peers().at(0);
    CHECK(peer.schema.at(1).name == "temp_k" && peer.schema.at(3).name == "boost" && peer.sources.at(1).empty());
    CHECK(near(real_at(*hub.latest("Engine"), 1), 300.0));
    const auto seq = hub.write(request({{"Engine", "engine.speed", 100.0}}));
    engine.step(SimTime{} + 30ms);
    CHECK(hub.operation(seq)->targets.at(0).status == TargetStatus::Applied);
    CHECK_EQ(engine.rpm_x10, 1000);
}

TEST_CASE(control_hub_applies_a_variable_map_per_connection_and_says_what_does_not_fit)
{
    Hub hub(HubOptions{.first_seq = 1});
    {
        // No map: as announced.
        Executable engine(hub, "a");
        same(hub.peers().at(0).schema.at(0).name, std::string("rpm_x10"));
        CHECK(hub.peers().at(0).sources.empty());
        // A map set now does not change the connection that is there.
        VariableMap map = engine_map();
        map.push_back(renamed("engine.torque", "torque"));
        hub.set_variable_map("Engine", map);
        hub.set_variable_map("Other", {renamed("x", "rpm_x10")});
        same(hub.peers().at(0).schema.at(0).name, std::string("rpm_x10"));
        engine.rpm_x10 = 500;
        engine.step(SimTime{} + 10ms);
        CHECK_EQ(std::get<std::int64_t>(hub.latest("Engine")->values.at(0)), std::int64_t{500});
        const auto seq = hub.write(request({{"Engine", "rpm_x10", std::int64_t{700}}}));
        engine.step(SimTime{} + 20ms);
        CHECK(hub.operation(seq)->targets.at(0).status == TargetStatus::Applied);
    }
    // The restarted executable: mapped, and the mapping that does not fit is said.
    Executable engine(hub, "b");
    const PeerInfo peer = hub.peers().at(0);
    same(peer.schema.at(0).name, std::string("engine.speed"));
    CHECK_EQ(peer.map_problems.size(), std::size_t{1});
    same(peer.map_problems.at(0), "engine.torque: the executable has no variable 'torque'");
    // A write to a name of that mapping is refused with the reason; the others are written.
    const auto unfit = hub.write(request({{"Engine", "engine.torque", 1.0}}));
    same(hub.operation(unfit)->targets.at(0).reason,
         "engine.torque: not written: the mapping engine.torque of this participant's variables was not applied "
         "(engine.torque: the executable has no variable 'torque'), so this name is not what the configuration says it is: "
         "correct variables in the configuration and run again");
    CHECK(hub.operation(unfit)->targets.at(0).status == TargetStatus::Invalid);
    engine.rpm_x10 = 500;
    engine.step(SimTime{} + 10ms);
    CHECK(near(real_at(*hub.latest("Engine"), 0), 50.0));
    // Taking the map away: from the next connection.
    hub.set_variable_map("Engine", {});
    Executable again(hub, "c");
    same(hub.peers().at(0).schema.at(0).name, std::string("rpm_x10"));
    CHECK(hub.peers().at(0).map_problems.empty());
    CHECK(hub.peers().at(0).sources.empty());
}
