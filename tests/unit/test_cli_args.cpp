// SPDX-License-Identifier: MIT
//
// Tests for the command-line helper shared by the executables (tools/common).
#include "test_harness.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/utf8.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib> // __argc, __argv (Windows)
#include <string>
#include <vector>

namespace {

using vehicle_sil::cli::Args;
using vehicle_sil::cli::UsageError;
using vehicle_sil::unit::throws;

/// Builds an Args from `--key value` words (argv[0] is added).
Args make(std::vector<std::string> words)
{
    words.insert(words.begin(), "program");
    std::vector<char*> argv;
    for (auto& word : words)
    {
        argv.push_back(word.data());
    }
    return Args(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE(cli_utf8_arguments_are_the_arguments_after_the_program_name)
{
    // Where argv is UTF-8 already, it is taken as it is. On Windows the arguments come from the
    // wide command line of the process (here: the test's own), not from what is passed in; the
    // launcher's tests start the executables with names in other scripts.
    std::vector<std::string> words{"program", "--name", "\xE8\xBB\x8A\xE4\xB8\xA1", "", "a b"};
    std::vector<char*> argv;
    for (auto& word : words)
    {
        argv.push_back(word.data());
    }
#ifndef _WIN32
    CHECK(vehicle_sil::cli::utf8_arguments(static_cast<int>(argv.size()), argv.data()) ==
          (std::vector<std::string>{"--name", "\xE8\xBB\x8A\xE4\xB8\xA1", "", "a b"}));
    CHECK(vehicle_sil::cli::utf8_arguments(1, argv.data()).empty());
    CHECK(vehicle_sil::cli::utf8_arguments(0, argv.data()).empty());
#else
    // The test's own command line: the arguments the test runner gave this process, which are
    // ASCII, so the C runtime's argv (in the code page) says the same.
    const auto own = vehicle_sil::cli::utf8_arguments(static_cast<int>(argv.size()), argv.data());
    CHECK_EQ(own.size(), static_cast<std::size_t>(__argc > 0 ? __argc - 1 : 0));
    for (std::size_t i = 0; i < own.size() && i + 1 < static_cast<std::size_t>(__argc); ++i)
    {
        CHECK(own[i] == std::string(__argv[i + 1]));
    }
    CHECK(vehicle_sil::cli::utf8_from_wide(L"\u8ECA\u4E21 a") == "\xE8\xBB\x8A\xE4\xB8\xA1 a");
    CHECK(vehicle_sil::cli::utf8_from_wide(L"").empty());
#endif
}

TEST_CASE(cli_utf8_path_names_the_file_whatever_the_code_page)
{
    const std::string name = "\xE8\xBB\x8A\xE4\xB8\xA1.yaml";
    const auto path = vehicle_sil::cli::utf8_path("dir/" + name);
    CHECK(path.filename().u8string() == std::u8string(name.begin(), name.end()));
    CHECK(path.parent_path() == std::filesystem::path("dir"));
    CHECK(vehicle_sil::cli::utf8_path("").empty());
}

TEST_CASE(cli_args_reject_malformed_command_lines)
{
    CHECK(throws<UsageError>([] { make({"positional"}); }));
    CHECK(throws<UsageError>([] { make({"-x", "1"}); }));
    CHECK(throws<UsageError>([] { make({"--"}); }));
    CHECK(throws<UsageError>([] { make({"--key"}); })); // missing value
    const Args args = make({"--a", "1", "--a", "2"});   // the last value wins
    CHECK(args.str("a", "") == "2");
}

TEST_CASE(cli_args_strings_and_required_options)
{
    const Args args = make({"--name", "Ecu"});
    CHECK(args.has("name"));
    CHECK(!args.has("other"));
    CHECK(args.str("name", "x") == "Ecu");
    CHECK(args.str("other", "fallback") == "fallback");
    CHECK(args.required("name") == "Ecu");
    CHECK(throws<UsageError>([&] { (void)args.required("other"); }));
}

TEST_CASE(cli_args_unsigned_values_are_strict)
{
    const Args args = make({"--dec", "42", "--hex", "0x80", "--neg", "-5", "--junk", "50x", "--empty", "", "--big", "101"});
    CHECK_EQ(args.u64("dec", 0, 100), 42u);
    CHECK_EQ(args.u64("hex", 0, 0xFF), 0x80u);
    CHECK_EQ(args.u64("absent", 7, 100), 7u);
    CHECK(throws<UsageError>([&] { (void)args.u64("neg", 0, 100); }));
    CHECK(throws<UsageError>([&] { (void)args.u64("junk", 0, 100); }));
    CHECK(throws<UsageError>([&] { (void)args.u64("empty", 0, 100); }));
    CHECK(throws<UsageError>([&] { (void)args.u64("big", 0, 100); }));
    CHECK_EQ(args.required_u64("dec", 100), 42u);
    CHECK(throws<UsageError>([&] { (void)args.required_u64("absent", 100); }));
    CHECK(args.millis("dec", std::chrono::milliseconds(1)) == std::chrono::milliseconds(42));
    CHECK(args.millis("absent", std::chrono::milliseconds(9)) == std::chrono::milliseconds(9));
}

TEST_CASE(cli_args_positive_double_and_list)
{
    const Args args = make({"--f", "0.5", "--zero", "0", "--neg", "-1", "--junk", "1.5x", "--list", "a,,b,", "--none", ","});
    CHECK(args.positive_double("f") == 0.5);
    CHECK(!args.positive_double("absent").has_value());
    CHECK(throws<UsageError>([&] { (void)args.positive_double("zero"); }));
    CHECK(throws<UsageError>([&] { (void)args.positive_double("neg"); }));
    CHECK(throws<UsageError>([&] { (void)args.positive_double("junk"); }));
    CHECK((args.list("list") == std::vector<std::string>{"a", "b"}));
    CHECK(args.list("none").empty());
    CHECK(throws<UsageError>([&] { (void)args.list("absent"); }));
}

TEST_CASE(cli_args_with_flags_and_positionals)
{
    const std::vector<std::string> words{"check", "--json", "a b.yaml", "--timeout-ms", "50"};
    const Args args(words, {"timeout-ms"}, {"json", "strict"});
    CHECK((args.positionals() == std::vector<std::string>{"check", "a b.yaml"}));
    CHECK(args.flag("json"));
    CHECK(!args.flag("strict"));
    CHECK(args.millis("timeout-ms", std::chrono::milliseconds(1)) == std::chrono::milliseconds(50));
    // Unknown options, repeated ones and missing values are usage errors.
    const auto parse = [](std::vector<std::string> w) { return Args(w, {"timeout-ms"}, {"json"}); };
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--bogus"}); }, "unknown option --bogus"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"-x"}); }, "unknown option -x"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--json", "--json"}); }, "--json is given twice"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--timeout-ms", "1", "--timeout-ms", "2"}); },
                                                     "--timeout-ms is given twice"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--timeout-ms"}); }, "missing value for --timeout-ms"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--timeout-ms", "--json"}); },
                                                     "missing value for --timeout-ms"));
    // A flag takes no value: what follows is a positional argument.
    CHECK((parse({"--json", "x"}).positionals() == std::vector<std::string>{"x"}));
}

TEST_CASE(cli_args_double_dash_ends_the_options)
{
    const std::vector<std::string> words{"check", "--json", "--", "-odd.yaml", "--json"};
    const Args args(words, {}, {"json"});
    CHECK(args.flag("json"));
    CHECK((args.positionals() == std::vector<std::string>{"check", "-odd.yaml", "--json"}));
}

TEST_CASE(cli_args_repeatable_options_keep_every_value)
{
    const std::vector<std::string> words{"run", "--disable", "A", "x.yaml", "--disable", "B", "--json"};
    const Args args(words, {}, {"json"}, {"disable", "enable"});
    CHECK((args.all("disable") == std::vector<std::string>{"A", "B"}));
    CHECK(args.all("enable").empty());
    CHECK(args.has("disable"));
    CHECK(!args.has("enable"));
    // The accessors for one value see the last one.
    CHECK(args.str("disable", "") == "B");
    CHECK(args.required("disable") == "B");
    CHECK(throws<UsageError>([&] { (void)args.required("enable"); }));
    CHECK((args.positionals() == std::vector<std::string>{"run", "x.yaml"}));
    const auto parse = [](std::vector<std::string> w) { return Args(w, {}, {"json"}, {"disable"}); };
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--disable"}); }, "missing value for --disable"));
    CHECK(vehicle_sil::unit::throws_with<UsageError>([&] { parse({"--disable", "--json"}); },
                                                     "missing value for --disable"));
}
