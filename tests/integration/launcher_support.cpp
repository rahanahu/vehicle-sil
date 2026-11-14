// SPDX-License-Identifier: MIT
#include "launcher_support.hpp"

#include "test_timeout_scale.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::test {
namespace {

std::string& log_tag()
{
    static std::string tag = "launcher";
    return tag;
}

} // namespace

void set_log_tag(std::string tag)
{
    log_tag() = std::move(tag);
}

void log(const std::string& text)
{
    std::printf("[%s] %s\n", log_tag().c_str(), text.c_str());
    std::fflush(stdout);
}

void check_timeout_scale()
{
    if (const auto warning = timeout_scale_warning())
    {
        log(*warning);
    }
}

bool expect(bool condition, const std::string& what)
{
    log("CHECK " + what + " -> " + (condition ? "ok" : "MISMATCH"));
    return condition;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void dump_log(const std::filesystem::path& path)
{
    std::printf("----- %s -----\n%s\n----- end of %s -----\n", path.string().c_str(), read_file(path).c_str(),
                path.filename().string().c_str());
    std::fflush(stdout);
}

std::string exit_code_text(const std::optional<int>& code)
{
    return code ? std::to_string(*code) : std::string("none (killed)");
}

std::optional<int> wait_or_kill(ChildProcess& process, std::chrono::steady_clock::time_point deadline)
{
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    auto code = process.wait_for(std::max(remaining, std::chrono::milliseconds::zero()));
    if (!code)
    {
        process.kill();
    }
    return code;
}

RegistryProcess::RegistryProcess(const std::filesystem::path& executable, const std::filesystem::path& work_dir) :
  log_(work_dir / "registry.log")
{
    const auto generated = work_dir / "registry-generated.yaml";
    std::filesystem::remove(generated);
    // Port 0: the OS picks a free port, so parallel test runs do not collide.
    process_ = std::make_unique<ChildProcess>(
        executable,
        std::vector<std::string>{"--listen-uri", "silkit://127.0.0.1:0", "--generate-configuration", generated.string()},
        log_);

    const auto startTimeout = scaled_timeout(std::chrono::seconds(15));
    const auto deadline = std::chrono::steady_clock::now() + startTimeout;
    const std::regex uriPattern(R"(silkit://[A-Za-z0-9.\-\[\]:]+)");
    while (true)
    {
        if (std::filesystem::exists(generated))
        {
            const std::string text = read_file(generated);
            std::smatch match;
            if (std::regex_search(text, match, uriPattern))
            {
                uri_ = match.str();
                return;
            }
        }
        if (const auto code = process_->poll())
        {
            dump_log(log_);
            throw std::runtime_error("sil-kit-registry exited early with code " + std::to_string(*code));
        }
        if (std::chrono::steady_clock::now() > deadline)
        {
            dump_log(log_);
            throw std::runtime_error("sil-kit-registry did not publish its URI within " +
                                     std::to_string(startTimeout.count()) + " ms");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace vehicle_sil::test
