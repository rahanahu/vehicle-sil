// SPDX-License-Identifier: MIT
//
// The executables of a configuration (resolve_executable(), executable_status(), find_executable()
// and executable_finding()): the part of the configuration's tests that looks at the files of the
// machine through the launcher's probe.
#include "launch_test_util.hpp"

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
using vehicle_sil::launch::test::lines;

namespace {

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// What check_file() does with a participant's executable: looks at the file, then judges it.
std::optional<Finding> check_executable(const ParticipantConfig& participant, const fs::path& path)
{
    return executable_finding(participant, path, executable_status(path));
}

} // namespace

TEST_CASE(resolves_the_executable_from_the_directory_of_the_file)
{
    const fs::path base = scratch("resolve");
    CHECK(resolve_executable("engine/engine_ecu", base) == base / "engine" / "engine_ecu");
    // Not normalized: ".." keeps its meaning across a symbolic link.
    CHECK(resolve_executable("./a/../b", base) == base / "." / "a" / ".." / "b");
    const fs::path absolute = base / "abs";
    CHECK(resolve_executable(process::utf8_from_path(absolute), "/elsewhere") == absolute);
#ifdef _WIN32
    std::ofstream(base / "tool.exe") << "x";
    std::ofstream(base / "engine.v2.exe") << "x";
    CHECK(resolve_executable("tool", base) == base / "tool.exe");
    CHECK(resolve_executable("engine.v2", base) == base / "engine.v2.exe");
#endif
}

TEST_CASE(checks_that_the_executable_is_an_executable_file)
{
    const fs::path base = scratch("executable");
    ParticipantConfig p;
    p.name = "Engine";
    p.command = {"engine/engine_ecu"};
    p.command_line = 12;
    const fs::path missing = base / "engine" / "engine_ecu";
    const auto none = check_executable(p, missing);
    CHECK(none.has_value());
    check_text(lines({*none}), "E 12 executable: participant 'Engine': the executable engine/engine_ecu does not exist "
                               "(looked for " +
                                   process::shown_path(missing) +
                                   "): build it, or correct the path (a relative path is from the directory of the "
                                   "configuration file)\n");
    CHECK(none->participant == "Engine");
    fs::create_directories(missing);
    check_text(lines({*check_executable(p, missing)}),
               "E 12 executable: participant 'Engine': the executable engine/engine_ecu is a directory (" +
                   process::shown_path(missing) + "): name the executable file in it\n");
    const fs::path file = base / "file";
    std::ofstream(file) << "#!/bin/sh\n";
#ifndef _WIN32
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    check_text(lines({*check_executable(p, file)}),
               "E 12 executable: participant 'Engine': the executable engine/engine_ecu is not executable (" +
                   process::shown_path(file) + "): make it executable (chmod +x), or name the right file\n");
    fs::permissions(file, fs::perms::owner_exec, fs::perm_options::add);
#endif
    CHECK(!check_executable(p, file).has_value());
}

TEST_CASE(a_missing_executable_gets_a_hint_for_path_and_arguments)
{
    const fs::path base = scratch("hints");
    ParticipantConfig p;
    p.name = "Tool";
    p.command_line = 4;
    p.command = {"python3", "tool.py"};
    const auto bare = check_executable(p, resolve_executable("python3", base));
    CHECK(bare.has_value());
    CHECK(bare->message.ends_with("; it is not on PATH either"));
    p.command = {"tools/engine --fast"};
    const auto spaced = check_executable(p, resolve_executable(p.command.front(), base));
    CHECK(spaced.has_value());
    CHECK(spaced->message.ends_with("; if the text after a space is an argument, write the command as a list, e.g. "
                                    "command: [tools/engine, ...]"));
    p.command = {"tools/engine"};
    CHECK(check_executable(p, resolve_executable(p.command.front(), base))->message.ends_with("configuration file)"));
}

TEST_CASE(a_batch_file_is_not_an_executable)
{
    const fs::path base = scratch("batch");
    std::ofstream(base / "run.CMD") << "@echo off\n";
#ifndef _WIN32
    fs::permissions(base / "run.CMD", fs::perms::owner_all);
#endif
    ParticipantConfig p;
    p.name = "Tool";
    p.command = {"run.CMD"};
    p.command_line = 4;
    const auto found = check_executable(p, resolve_executable("run.CMD", base));
    CHECK(found.has_value());
    check_text(found->message, "participant 'Tool': the executable run.CMD is a batch file (" +
                                   process::shown_path(base / "run.CMD") +
                                   "), which runs through cmd.exe with other quoting rules: name the executable it starts");
}

TEST_CASE(a_bare_name_that_is_not_next_to_the_file_is_looked_for_on_path)
{
    const fs::path base = scratch("path base");
    const fs::path bin = scratch("path bin");
    const fs::path other = scratch("path other");
    const auto make = [](const fs::path& file) {
        std::ofstream(file) << "#!/bin/sh\n";
        fs::permissions(file, fs::perms::owner_all);
    };
#ifdef _WIN32
    const std::string tool = "tool.exe";
    const char separator = ';';
#else
    const std::string tool = "tool";
    const char separator = ':';
#endif
    make(bin / tool);
    const std::string path = std::string("relative") + separator + other.string() + separator + bin.string();
#ifdef _WIN32
    _putenv_s("PATH", path.c_str());
#else
    ::setenv("PATH", path.c_str(), 1);
#endif
    FoundExecutable found = find_executable("tool", base);
    CHECK(found.on_path);
    CHECK(fs::equivalent(found.path, bin / tool));
    // Next to the file it wins over PATH.
    make(base / tool);
    found = find_executable("tool", base);
    CHECK(!found.on_path);
    CHECK(fs::equivalent(found.path, base / tool));
    // A path with a separator is never looked for on PATH (bin/sub/tool exists).
    fs::create_directories(bin / "sub");
    make(bin / "sub" / tool);
    found = find_executable("sub/tool", base);
    CHECK(!found.on_path);
    CHECK(process::shown_path(found.path) == process::shown_path(base / "sub/tool"));
    // Not anywhere: the path next to the file, for the message.
    found = find_executable("nothing", base);
    CHECK(!found.on_path);
    CHECK(process::shown_path(found.path) == process::shown_path(base / "nothing"));
#ifndef _WIN32
    // A file without the execute permission on PATH is not taken.
    std::ofstream(other / "plain") << "x";
    found = find_executable("plain", base);
    CHECK(!found.on_path);
#endif
}

TEST_CASE(a_file_next_to_the_configuration_that_is_not_executable_does_not_hide_the_program_on_path)
{
#ifndef _WIN32
    const fs::path base = scratch("path data base");
    const fs::path bin = scratch("path data bin");
    std::ofstream(base / "tool") << "data, not a program";
    std::ofstream(bin / "tool") << "#!/bin/sh\n";
    fs::permissions(bin / "tool", fs::perms::owner_all);
    ::setenv("PATH", bin.string().c_str(), 1);
    const FoundExecutable found = find_executable("tool", base);
    CHECK(found.on_path);
    CHECK(fs::equivalent(found.path, bin / "tool"));
#else
    // Windows has no execute permission: a file next to the configuration is the program.
    throw vehicle_sil::unit::Skipped("every file is executable on Windows");
#endif
}
