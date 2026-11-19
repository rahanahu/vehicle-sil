// SPDX-License-Identifier: MIT
//
// find_tool(): where the launcher looks for the tool of a connection (the bridge), through the
// launcher's probe. The rest of the connection's rules are in test_connect.cpp.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;

namespace {

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "connect" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// A file `name` in `dir` (on Windows "<name>.exe"), runnable or not.
fs::path tool_file(const fs::path& dir, const std::string& name, bool runnable = true)
{
#ifdef _WIN32
    const fs::path path = dir / (name + ".exe");
#else
    const fs::path path = dir / name;
#endif
    std::ofstream(path) << "#!/bin/sh\n";
    fs::permissions(path, runnable ? fs::perms::owner_all : fs::perms::owner_read | fs::perms::owner_write);
    return path;
}

/// Sets PATH to `value` and the working directory to `cwd` (if not empty) until it ends.
class Environment
{
public:
    Environment(const std::string& value, const fs::path& cwd = {}) : cwd_(fs::current_path())
    {
#ifdef _WIN32
        const wchar_t* old = _wgetenv(L"PATH");
        old_ = old != nullptr ? std::optional<std::wstring>(old) : std::nullopt;
        _wputenv_s(L"PATH", process::path_from_utf8(value).wstring().c_str());
#else
        const char* old = std::getenv("PATH");
        old_ = old != nullptr ? std::optional<std::string>(old) : std::nullopt;
        setenv("PATH", value.c_str(), 1);
#endif
        if (!cwd.empty())
        {
            fs::current_path(cwd);
        }
    }
    ~Environment()
    {
        fs::current_path(cwd_);
#ifdef _WIN32
        _wputenv_s(L"PATH", old_ ? old_->c_str() : L"");
#else
        if (old_)
        {
            setenv("PATH", old_->c_str(), 1);
        }
        else
        {
            unsetenv("PATH");
        }
#endif
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

private:
    fs::path cwd_;
#ifdef _WIN32
    std::optional<std::wstring> old_;
#else
    std::optional<std::string> old_;
#endif
};

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

} // namespace

TEST_CASE(find_tool_looks_next_to_the_launcher_first_then_on_path)
{
    const fs::path base = scratch("order");
    const fs::path launcher = base / "launcher";
    const fs::path onPath = base / "on_path";
    fs::create_directories(launcher);
    fs::create_directories(onPath);
    const fs::path beside = tool_file(launcher, "vehicle_sil_ethernet_bridge");
    const fs::path listed = tool_file(onPath, "vehicle_sil_ethernet_bridge");
    const Environment path(process::utf8_from_path(onPath));
    CHECK(find_tool("vehicle_sil_ethernet_bridge", launcher) == beside);
    CHECK(find_tool("vehicle_sil_ethernet_bridge", {}) == listed);
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base / "elsewhere") == listed);
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", launcher).has_value());
}

TEST_CASE(find_tool_skips_relative_path_entries_and_the_working_directory)
{
    const fs::path base = scratch("relative");
    fs::create_directories(base / "rel");
    static_cast<void>(tool_file(base / "rel", "vehicle_sil_vector_xl_bridge"));
    static_cast<void>(tool_file(base, "vehicle_sil_vector_xl_bridge"));
    // "rel" and "." would find the tool from the working directory: they are not looked at.
    const Environment path(std::string("rel") + kPathSeparator + "." + kPathSeparator, base);
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", {}).has_value());
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", base / "launcher").has_value());
}

TEST_CASE(find_tool_takes_a_runnable_file_only)
{
#ifndef _WIN32
    // A file without an execute permission next to the launcher does not hide the one on PATH.
    const fs::path base = scratch("not_runnable");
    fs::create_directories(base / "launcher");
    fs::create_directories(base / "bin");
    static_cast<void>(tool_file(base / "launcher", "vehicle_sil_ethernet_bridge", false));
    const fs::path listed = tool_file(base / "bin", "vehicle_sil_ethernet_bridge");
    const Environment path(process::utf8_from_path(base / "bin"));
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base / "launcher") == listed);
#else
    // The name without ".exe" finds the executable.
    const fs::path base = scratch("exe");
    const fs::path beside = tool_file(base, "vehicle_sil_ethernet_bridge");
    const Environment path("");
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base) == beside);
#endif
}
