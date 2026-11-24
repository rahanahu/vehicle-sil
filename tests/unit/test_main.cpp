// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

bool parse_case_list(const std::string& arg, std::vector<std::string>& out)
{
    if (arg.empty())
    {
        return false;
    }
    std::size_t start = 0;
    while (start <= arg.size())
    {
        const std::size_t comma = arg.find(',', start);
        const std::size_t end = comma == std::string::npos ? arg.size() : comma;
        if (end == start)
        {
            return false;
        }
        out.emplace_back(arg.substr(start, end - start));
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return !out.empty();
}

} // namespace

int main(int argc, char** argv)
{
    auto& tests = vehicle_sil::unit::registry();
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        for (const auto& [name, test] : tests)
        {
            std::printf("%s\n", name.c_str());
        }
        return 0;
    }

    const bool batchMode = argc == 3 && std::string(argv[1]) == "--cases";
    std::vector<std::string> requested;
    if (batchMode)
    {
        if (!parse_case_list(argv[2], requested))
        {
            std::printf("invalid --cases list: '%s'\n", argv[2]);
            return 2;
        }
        for (const auto& name : requested)
        {
            if (tests.find(name) == tests.end())
            {
                std::printf("no test matched '%s'\n", name.c_str());
                return 2;
            }
        }
    }
    else if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--cases"))
    {
        std::printf("usage: %s [--list | <test-name> | --cases <name1>,<name2>,...]\n", argv[0]);
        return 2;
    }

    // A wrong VEHICLE_SIL_TEST_TIMEOUT_SCALE fails the run before any test, saying what to set; a
    // factor larger than the one CTest's TIMEOUT was made for is said before the tests.
    const vehicle_sil::test::TimeoutScale* timeoutScale = nullptr;
    try
    {
        timeoutScale = &vehicle_sil::test::timeout_scale_of_process();
        if (const auto warning = vehicle_sil::test::timeout_scale_warning())
        {
            std::printf("%s\n", warning->c_str());
        }
    }
    catch (const std::exception& e)
    {
        std::printf("%s\n", e.what());
        return 1;
    }

    int failures = 0;
    int executed = 0;
    int skipped = 0;
    auto run_one = [&](const std::string& name, const vehicle_sil::unit::TestFunction& test, bool logStart) {
        if (logStart)
        {
            // Flush before running so a crash or sanitizer abort identifies the active case.
            std::printf("[ RUN  ] %s\n", name.c_str());
            std::fflush(stdout);
        }
        ++executed;
        try
        {
            test();
            std::printf("[ PASS ] %s\n", name.c_str());
        }
        catch (const vehicle_sil::unit::Skipped& e)
        {
            ++skipped;
            std::printf("[ SKIP ] %s\n  %s\n", name.c_str(), e.what());
        }
        catch (const std::exception& e)
        {
            ++failures;
            std::printf("[ FAIL ] %s\n  %s\n", name.c_str(), e.what());
            if (timeoutScale->factor != 1.0)
            {
                std::printf("  (the upper bounds of waiting were %s times as long: %s)\n",
                            vehicle_sil::test::factor_text(timeoutScale->factor).c_str(), timeoutScale->source.c_str());
            }
        }
    };

    if (batchMode)
    {
        for (const auto& name : requested)
        {
            run_one(name, tests.at(name), true);
        }
    }
    else
    {
        for (const auto& [name, test] : tests)
        {
            if (argc == 2 && name != argv[1])
            {
                continue;
            }
            run_one(name, test, argc == 1);
        }
    }
    if (executed == 0)
    {
        std::printf("no test matched '%s'\n", argc == 2 ? argv[1] : "");
        return 2;
    }
    if (failures > 0)
    {
        return 1;
    }
    // A run whose executed cases were all skipped is reported as skipped.
    return executed > 0 && skipped == executed ? vehicle_sil::unit::kExitSkipped : 0;
}
