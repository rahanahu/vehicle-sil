// SPDX-License-Identifier: MIT
//
// A variable map: the names and values under which the clients of a simulation see the variables
// of one executable (vehicle_sil::control). Executables that differ in the name, the scale or the
// values of a variable are seen alike. The Hub applies it where an agent's schema, samples and
// results come in and where a client's writes go out; the launcher's check applies the same code
// to what an executable describes, so both follow one set of rules.
#pragma once

#include "vehicle_sil/control/agent.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::control {

/// How one variable of an executable is seen: under `name`, and in one of four forms.
/// - a new name only (`from`);
/// - a linear conversion (`scale`, `offset`): seen value = executable's value * scale + offset;
/// - names for its values (`map`): the variable is seen as an enum with these items, and the
///   numbers stay the executable's;
/// - `absent`: the executable has no such variable (said on purpose; nothing is converted).
/// To add a form: a member here, its rules in MappedSchema::make(), and the reader of the
/// configuration (libs/launch/src/config.cpp).
struct VariableMapping
{
    std::string name; ///< the name the clients see (the common name)
    std::string from; ///< the executable's variable; "" with absent
    bool absent = false;
    std::optional<double> scale{};  ///< not 0; missing: 1
    std::optional<double> offset{}; ///< missing: 0
    std::optional<std::vector<EnumItem>> map{};
    /// The unit of the seen value. Missing: the executable's, or none after a linear conversion
    /// (the executable's unit is not the unit of the converted value).
    std::optional<std::string> unit{};

    [[nodiscard]] bool linear() const noexcept { return scale.has_value() || offset.has_value(); }
    friend bool operator==(const VariableMapping&, const VariableMapping&) = default;
};

using VariableMap = std::vector<VariableMapping>;

enum class MapProblemKind
{
    Form,      ///< the mapping itself is not one of the forms (scale 0, map with scale, ...)
    Unknown,   ///< the executable has no variable `from`
    Type,      ///< the form does not fit the variable's type
    BadValue,  ///< a value of `map` is not a value of the variable
    Collision, ///< two variables would be seen under one name, or one variable under two
    Present,   ///< absent, but the executable has a variable of that name
};

/// A mapping that does not fit the executable's variables. It is not applied: the variable (if
/// any) is seen under the executable's name, unchanged.
struct MapProblem
{
    std::size_t entry = 0; ///< index in the VariableMap
    MapProblemKind kind = MapProblemKind::Form;
    std::string message; ///< what is wrong and how to correct it; starts with the mapping's name

    friend bool operator==(const MapProblem&, const MapProblem&) = default;
};

/// The variables of one executable as its clients see them, and the conversions between the
/// two views. Immutable once made; every function is safe from any thread.
///
/// The seen schema has the executable's variables in the executable's order (a sample is
/// converted place by place), each under a unique name.
class MappedSchema
{
public:
    /// Applies `map` to the executable's variables `actual`. A mapping with a problem is left out
    /// (problems()); the others apply.
    [[nodiscard]] static MappedSchema make(const std::vector<VariableInfo>& actual, const VariableMap& map);

    /// What the clients see.
    [[nodiscard]] const std::vector<VariableInfo>& schema() const noexcept { return schema_; }
    /// For each variable of schema(): the executable's name of it if a mapping applies to it, else "".
    [[nodiscard]] const std::vector<std::string>& sources() const noexcept { return sources_; }
    [[nodiscard]] const std::vector<MapProblem>& problems() const noexcept { return problems_; }

    /// A sample of the executable (in schema order) becomes what the clients see, in place.
    /// A sample of another size is left alone.
    void to_seen(Snapshot& sample) const;

    /// A client's write (seen name and value) becomes the executable's, in place.
    /// @returns the reason if the value cannot be converted (not a number, not one of the map's
    /// values, out of the variable's range), in the words of Agent's own refusals. A name that is
    /// not in schema() is left alone.
    [[nodiscard]] std::optional<std::string> to_actual(Write& write) const;

    /// A reason the executable's agent gave ("<its name>: ..."), with the seen name in front.
    [[nodiscard]] std::string reason_to_seen(std::string reason) const;

private:
    enum class Convert
    {
        None,
        Linear,
        Items,
    };
    struct Rule
    {
        Convert convert = Convert::None;
        double scale = 1.0;
        double offset = 0.0;
        bool integer = false; ///< the executable's variable is an integer
        std::optional<double> min{}; ///< the executable's range
        std::optional<double> max{};
    };

    std::vector<VariableInfo> schema_;
    std::vector<std::string> sources_;
    std::vector<std::string> actual_names_;
    std::vector<Rule> rules_; ///< per variable
    std::vector<std::size_t> linear_; ///< the variables whose samples are converted
    std::map<std::string, std::size_t, std::less<>> seen_index_;   ///< of the variables with a mapping
    std::map<std::string, std::size_t, std::less<>> actual_index_; ///< of the variables seen under another name
    std::vector<MapProblem> problems_;
};

} // namespace vehicle_sil::control
