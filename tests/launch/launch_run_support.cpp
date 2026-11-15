// SPDX-License-Identifier: MIT
//
// The definitions of the shared code that launch_run_support.hpp declares.
#include "launch_run_support.hpp"

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lf(std::string text)
{
    std::erase(text, '\r');
    return text;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(static_cast<bool>(out), "cannot write " + path.string());
}

std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

std::string without_versions(std::string text)
{
    const std::string key = "\"vehicle_sil\": \"";
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
    {
        const auto from = at + key.size();
        const auto to = text.find('"', from);
        require(to != std::string::npos, "a \"vehicle_sil\" value does not end");
        text.replace(from, to - from, "<version>");
    }
    return text;
}

std::string without_timing(std::string text)
{
    for (const std::string key : {"wall_ms", "actual_speed"})
    {
        const std::string head = "\"" + key + "\":";
        for (auto at = text.find(head); at != std::string::npos; at = text.find(head, at + 1))
        {
            auto from = at + head.size();
            if (from < text.size() && text[from] == ' ')
            {
                ++from;
            }
            const auto to = text.find_first_of(",}\n", from);
            text.replace(from, to - from, "<" + key + ">");
        }
    }
    return text;
}

fs::path marked_file(const fs::path& docs, const std::string& marker)
{
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(docs))
    {
        if (entry.path().extension() == ".md" && lf(read_text(entry.path())).find(marker + "\n") != std::string::npos)
        {
            found.push_back(entry.path());
        }
    }
    require(!found.empty(), "no document in " + docs.string() + " has the line " + marker);
    if (found.size() > 1)
    {
        require(false, "two documents in " + docs.string() + " have the line " + marker + ": " +
                           found[0].filename().string() + ", " + found[1].filename().string() +
                           " (keep the line in one of them)");
    }
    return found[0];
}

std::string block_after(const fs::path& path, const std::string& marker)
{
    if (fs::is_directory(path))
    {
        return block_after(marked_file(path, marker), marker);
    }
    const std::string text = lf(read_text(path));
    const auto at = text.find(marker + "\n");
    require(at != std::string::npos, path.string() + " has no line " + marker);
    const auto open = text.find('\n', text.find("```", at)) + 1;
    const auto close = text.find("\n```", open);
    require(close != std::string::npos, path.string() + ": the block after " + marker + " does not end");
    return text.substr(open, close - open + 1);
}

void expect_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        throw std::runtime_error(what + " differs\n--- got ---\n" + actual + "--- expected ---\n" + expected + "---");
    }
    log("CHECK " + what + " -> ok");
}

Captured launch(const Options& o, const std::vector<std::string>& args, const fs::path& dir)
{
    vehicle_sil::process::CaptureOptions options;
    options.timeout = scaled_timeout(240s);
    options.working_directory = dir;
    return vehicle_sil::process::run_captured(o.launch, args, options);
}

void expect_code(const Captured& run, int code, const std::string& what)
{
    require(run.exit_code.has_value(), what + ": vehicle_sil_launch did not end");
    require(*run.exit_code == code, what + ": exit code " + std::to_string(*run.exit_code) + ", not " +
                                        std::to_string(code) + "\n--- stdout ---\n" + run.standard_output +
                                        "--- stderr ---\n" + run.standard_error);
}

bool can_request_stop()
{
#ifdef _WIN32
    return GetConsoleWindow() != nullptr || GetConsoleCP() != 0;
#else
    return true;
#endif
}

std::string run_part(const std::string& text)
{
    const auto at = text.find("run: ");
    require(at != std::string::npos, "the output has no run: line\n" + text);
    return text.substr(at);
}

fs::path example_copy(const Options& o)
{
    const fs::path dir = o.work_dir / "example";
    for (const std::string sub : {"engine", "dashboard"})
    {
        fs::create_directories(dir / sub);
        for (const fs::directory_entry& entry : fs::directory_iterator(o.example_dir / sub))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path to = dir / sub / entry.path().filename();
            fs::remove(to);
            std::error_code no_link;
            fs::create_hard_link(entry.path(), to, no_link);
            if (no_link)
            {
                fs::copy_file(entry.path(), to);
            }
        }
    }
    fs::copy_file(o.example_dir / "simulation.yaml", dir / "simulation.yaml", fs::copy_options::overwrite_existing);
    return dir;
}

fs::path scenario_dir(const Options& o, const std::string& config, const std::vector<std::pair<std::string, std::string>>& behaviours)
{
    const fs::path dir = o.work_dir / "run";
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", config);
    for (const auto& [name, behaviour] : behaviours)
    {
        write_file(dir / (name + ".behaviour"), behaviour + "\n");
        fs::remove(dir / (name + ".hanging"));
    }
    return dir;
}

void wait_hanging(const fs::path& dir, const std::string& name)
{
    require(pid_written((dir / (name + ".hanging")).string(), 60s),
            name + " did not reach its end after the run within " +
                std::to_string(vehicle_sil::test::scaled_timeout(60s).count()) + " ms (no " + name + ".hanging)");
}

std::string ecu(const Options& o) { return yaml_quote(shown_path(o.test_ecu)); }

std::string helper(const Options& o) { return yaml_quote(shown_path(o.helper)); }

bool all_gone(const fs::path& dir, const std::vector<std::string>& pid_files, std::chrono::milliseconds timeout)
{
    bool gone = true;
    for (const auto& file : pid_files)
    {
        const long pid = read_pid((dir / file).string(), 0ms);
        if (pid > 0 && !wait_gone(pid, timeout))
        {
            log("process " + std::to_string(pid) + " (" + file + ") is still running: ending it");
            kill_one(pid);
            gone = false;
        }
    }
    return gone;
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', from))
    {
        out.push_back(text.substr(from, end - from));
        from = end + 1;
    }
    require(from == text.size(), "the last line has no line end:\n" + text.substr(from));
    return out;
}

std::string value_of(const std::string& line, const std::string& key)
{
    const std::string head = "\"" + key + "\":";
    const auto at = line.find(head);
    if (at == std::string::npos)
    {
        return {};
    }
    const auto from = at + head.size();
    if (line.compare(from, 1, "\"") == 0)
    {
        return line.substr(from + 1, line.find('"', from + 1) - from - 1);
    }
    return line.substr(from, line.find_first_of(",}", from) - from);
}

std::string members_of(const std::string& line)
{
    const auto at = line.find(",\"t_ms\":");
    const auto from = at == std::string::npos ? at : line.find_first_of(",}", at + 8);
    return from == std::string::npos || line[from] == '}' ? std::string{} : line.substr(from + 1, line.size() - from - 2);
}

void check_frame(const std::string& line, std::size_t i)
{
    const std::string event = value_of(line, "event");
    const std::string head = "{\"event\":\"" + event + "\",\"seq\":" + std::to_string(i) + ",\"t_ms\":";
    require(line.rfind(head, 0) == 0 && line.back() == '}', "event " + std::to_string(i) + " is not framed:\n" + line);
    require((event == "hello") == (i == 0), "hello is the first event and only it:\n" + line);
}

std::vector<std::string> check_events(const std::vector<std::string>& lines)
{
    require(!lines.empty(), "no events");
    static const std::vector<std::string> kPhases{"checking", "starting", "waiting", "running", "stopping", "ended"};
    std::size_t phase = 0;
    std::vector<std::string> started;
    std::vector<std::string> findings;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& line = lines[i];
        check_frame(line, i);
        const std::string event = value_of(line, "event");
        require((event == "result") == (i + 1 == lines.size()), "result is the last event and only it:\n" + line);
        if (event == "phase")
        {
            const auto at = std::find(kPhases.begin(), kPhases.end(), value_of(line, "phase"));
            require(at != kPhases.end() && static_cast<std::size_t>(at - kPhases.begin()) >= phase,
                    "the phases go forward: event " + std::to_string(i) + "\n" + line);
            phase = static_cast<std::size_t>(at - kPhases.begin());
        }
        else if (event == "participant" && value_of(line, "state") == "started")
        {
            started.push_back(value_of(line, "name"));
        }
        else if (event == "log" && value_of(line, "source") != "vehicle_sil_launch")
        {
            require(std::find(started.begin(), started.end(), value_of(line, "source")) != started.end(),
                    "a participant's line comes after its started event: event " + std::to_string(i) + "\n" + line);
        }
        else if (event == "finding")
        {
            require(std::find(findings.begin(), findings.end(), members_of(line)) == findings.end(),
                    "a finding is written once:\n" + line);
            findings.push_back(members_of(line));
        }
    }
    for (const auto& finding : findings)
    {
        require(contains(lines.back(), "{" + finding + "}"),
                "a finding that was written as an event is in the result as it was written:\n" + finding + "\n" + lines.back());
    }
    return lines;
}

std::vector<std::string> check_events(const std::string& text) { return check_events(lines_of(text)); }

std::vector<std::string> events_with(const std::vector<std::string>& lines, const std::vector<std::string>& parts)
{
    std::vector<std::string> out;
    for (const auto& line : lines)
    {
        if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(line, p); }))
        {
            out.push_back(line);
        }
    }
    return out;
}

std::string code_text(const std::optional<vehicle_sil::process::ExitStatus>& status)
{
    return status ? "exit code " + std::to_string(status->code) : std::string("still running (killed by the test)");
}

std::string scenario_json(const std::string& json)
{
    const auto at = json.rfind("\n  \"scenario\": {");
    require(at != std::string::npos, "the result has no scenario:\n" + json);
    const auto end = json.find("\n  }", at + 1);
    require(end != std::string::npos, "the scenario object does not end:\n" + json);
    return json.substr(at + 1, end + 4 - at - 1) + "\n";
}

fs::path scenario_ecus(const Options& o, const std::string& keys, const std::string& scenario, const std::string& body)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n" + keys +
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Body\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Door\n"
                                          "    command: [" + ecu(o) + "]\n",
                                      {{"Body", "scenario-variables " + body}, {"Door", "scenario-variables"}});
    write_file(dir / "s.yaml", scenario);
    return dir;
}

HandCommand hand_command(const std::string& err, const std::string& name)
{
    const auto at = err.find("[vehicle_sil_launch] not starting " + name + ": ");
    const auto end = at == std::string::npos ? at : err.find('\n', at);
    const auto next = end == std::string::npos ? end : err.find('\n', end + 1);
    if (next == std::string::npos)
    {
        return {};
    }
    const std::string first = err.substr(at, end - at);
    const std::string second = err.substr(end + 1, next - end - 1);
    const std::string in = ". In the directory ";
    const std::string tail = ", run:";
    const std::string tag = "[vehicle_sil_launch]   ";
    const auto from = first.rfind(in);
    require(from != std::string::npos && first.size() > tail.size() &&
                first.compare(first.size() - tail.size(), tail.size(), tail) == 0 && second.rfind(tag, 0) == 0,
            "the lines about " + name + " say where and how to start it:\n" + first + "\n" + second);
    return {first.substr(from + in.size(), first.size() - tail.size() - from - in.size()), second.substr(tag.size()),
            first + "\n" + second + "\n"};
}

/// The first port that the system hands out for port 0 and for outgoing connections: on Linux
/// what /proc/sys/net/ipv4/ip_local_port_range says (32768 by default), elsewhere 49152 (the
/// default of Windows).
int first_ephemeral_port()
{
#ifndef _WIN32
    std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
    int low = 0;
    if (range >> low && low > 0)
    {
        return low;
    }
#endif
    return 49152;
}

/// Whether a TCP socket can be bound to `port` on 127.0.0.1 now (0: any port; the port bound is
/// stored back into it).
bool bindable(int& port)
{
#ifdef _WIN32
    WSADATA data;
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET, "cannot make a socket");
    int size = sizeof(sockaddr_in);
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    require(s >= 0, "cannot make a socket");
    socklen_t size = sizeof(sockaddr_in);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    const bool bound = ::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                       ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0;
#ifdef _WIN32
    closesocket(s);
    WSACleanup();
#else
    ::close(s);
#endif
    if (bound)
    {
        port = ntohs(address.sin_port);
    }
    return bound;
}

int free_port()
{
    const int below = std::min(first_ephemeral_port(), 65536);
    constexpr int kLowest = 20000;
    if (below - kLowest >= 1000)
    {
        // The clock as well: random_device of an old MinGW gives the same numbers in every process.
        std::random_device seed;
        std::mt19937 pick(seed() ^ static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int> port_in(kLowest, below - 1);
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            int port = port_in(pick);
            if (bindable(port))
            {
                return port;
            }
        }
    }
    // The system hands out (nearly) every port by itself: the first free one it gives.
    int port = 0;
    require(bindable(port), "cannot find a free port");
    return port;
}

std::string joined_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines)
    {
        out += line + "\n";
    }
    return out;
}

fs::path hand_dir(const Options& o, const std::string& part, const std::string& config,
                  const std::vector<std::pair<std::string, std::string>>& behaviours)
{
    const fs::path dir = o.work_dir / part;
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", config);
    for (const auto& [name, behaviour] : behaviours)
    {
        write_file(dir / (name + ".behaviour"), behaviour + "\n");
    }
    return dir;
}

std::string out_of(const std::vector<std::string>& lines) { return joined_lines(lines); }
