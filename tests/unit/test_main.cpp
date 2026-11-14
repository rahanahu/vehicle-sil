// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include <cstdio>
#include <exception>
#include <string>

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
    // A wrong VEHICLE_SIL_TEST_TIMEOUT_SCALE fails the run before any test, saying what to set.
    double timeoutScale = 1.0;
    try
    {
        timeoutScale = vehicle_sil::unit::timeout_scale();
    }
    catch (const std::exception& e)
    {
        std::printf("%s\n", e.what());
        return 1;
    }

    int failures = 0;
    int executed = 0;
    int skipped = 0;
    for (const auto& [name, test] : tests)
    {
        if (argc == 2 && name != argv[1])
        {
            continue;
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
            if (timeoutScale != 1.0)
            {
                std::printf("  (the upper bounds of waiting were %g times as long: VEHICLE_SIL_TEST_TIMEOUT_SCALE)\n",
                            timeoutScale);
            }
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
    // A test run alone that could not run here is reported as skipped.
    return executed == 1 && skipped == 1 ? vehicle_sil::unit::kExitSkipped : 0;
}
