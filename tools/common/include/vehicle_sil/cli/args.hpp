// SPDX-License-Identifier: MIT
//
// Command-line parsing and exit codes shared by the vehicle-sil executables.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::cli {

/// Process exit codes. Test launchers check them; executables may give code 1 a more specific
/// name (e.g. a verification failure).
enum ExitCode : int
{
    kExitOk = 0,
    kExitFailed = 1,      ///< Ran, but the result is wrong (error state, failed check, ...).
    kExitTimeout = 2,     ///< Did not finish in time.
    kExitSetupFailed = 3, ///< Could not connect or start.
    kExitUsage = 4,       ///< Bad command line.
};

/// A bad command line. Report it with the usage text and kExitUsage.
class UsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Parses `--key value` pairs. Every option takes exactly one value. All accessors throw
/// UsageError for a missing required option or a malformed value.
class Args
{
public:
    Args(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view key = argv[i];
            if (key.size() < 3 || key.substr(0, 2) != "--")
            {
                throw UsageError("unexpected argument: " + std::string(key));
            }
            if (i + 1 >= argc)
            {
                throw UsageError("missing value for " + std::string(key));
            }
            values_[std::string(key.substr(2))] = argv[++i];
        }
    }

    [[nodiscard]] bool has(const std::string& key) const { return values_.count(key) != 0; }

    [[nodiscard]] std::string str(const std::string& key, const std::string& fallback) const
    {
        const auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }

    [[nodiscard]] std::string required(const std::string& key) const
    {
        const auto it = values_.find(key);
        if (it == values_.end())
        {
            throw UsageError("missing required option --" + key);
        }
        return it->second;
    }

    /// Accepts decimal or 0x-prefixed hexadecimal values up to `max`.
    [[nodiscard]] std::uint64_t u64(const std::string& key, std::uint64_t fallback, std::uint64_t max) const
    {
        const auto it = values_.find(key);
        return it == values_.end() ? fallback : parse_u64(key, it->second, max);
    }

    /// Like u64(), for a required option.
    [[nodiscard]] std::uint64_t required_u64(const std::string& key, std::uint64_t max) const
    {
        return parse_u64(key, required(key), max);
    }

    /// Like u64(), or nullopt if the option is absent.
    [[nodiscard]] std::optional<std::uint64_t> optional_u64(const std::string& key, std::uint64_t max) const
    {
        const auto it = values_.find(key);
        return it == values_.end() ? std::nullopt : std::optional(parse_u64(key, it->second, max));
    }

    /// A duration in milliseconds (at most kMaxMillis).
    [[nodiscard]] std::chrono::milliseconds millis(const std::string& key, std::chrono::milliseconds fallback) const
    {
        return std::chrono::milliseconds(u64(key, static_cast<std::uint64_t>(fallback.count()), kMaxMillis));
    }

    /// Like millis(), or nullopt if the option is absent.
    [[nodiscard]] std::optional<std::chrono::milliseconds> optional_millis(const std::string& key) const
    {
        const auto value = optional_u64(key, kMaxMillis);
        return value ? std::optional(std::chrono::milliseconds(*value)) : std::nullopt;
    }

    /// Upper limit of millis() values: about 31 years.
    static constexpr std::uint64_t kMaxMillis = 1'000'000'000'000;

    /// A positive, finite floating-point value, or nullopt if the option is absent.
    [[nodiscard]] std::optional<double> positive_double(const std::string& key) const
    {
        const auto it = values_.find(key);
        if (it == values_.end())
        {
            return std::nullopt;
        }
        std::size_t used = 0;
        double value = 0.0;
        try
        {
            value = std::stod(it->second, &used);
        }
        catch (const std::exception&)
        {
            used = 0;
        }
        if (used != it->second.size() || !std::isfinite(value) || !(value > 0.0))
        {
            throw UsageError("invalid value for --" + key + ": " + it->second);
        }
        return value;
    }

    /// A comma-separated list; empty items are skipped.
    [[nodiscard]] std::vector<std::string> list(const std::string& key) const
    {
        std::vector<std::string> out;
        std::stringstream in(required(key));
        std::string item;
        while (std::getline(in, item, ','))
        {
            if (!item.empty())
            {
                out.push_back(item);
            }
        }
        return out;
    }

private:
    /// Decimal digits, or "0x" followed by hexadecimal digits. No sign, no whitespace, and a
    /// leading 0 does not mean octal.
    static std::uint64_t parse_u64(const std::string& key, const std::string& text, std::uint64_t max)
    {
        const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        const std::string digits = hex ? text.substr(2) : text;
        const bool wellFormed =
            !digits.empty() && std::all_of(digits.begin(), digits.end(), [hex](char c) {
                return hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0 : std::isdigit(static_cast<unsigned char>(c)) != 0;
            });
        std::size_t used = 0;
        std::uint64_t value = 0;
        if (wellFormed)
        {
            try
            {
                value = std::stoull(digits, &used, hex ? 16 : 10);
            }
            catch (const std::exception&)
            {
                used = 0; // out of range
            }
        }
        if (!wellFormed || used != digits.size() || value > max)
        {
            throw UsageError("invalid value for --" + key + ": " + text);
        }
        return value;
    }

    std::map<std::string, std::string> values_;
};

} // namespace vehicle_sil::cli
