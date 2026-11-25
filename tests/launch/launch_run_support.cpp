// SPDX-License-Identifier: MIT
//
// The definitions of the shared code that launch_run_support.hpp declares.
#include "launch_run_support.hpp"

#include "vehicle_sil/process/paths.hpp"

#include <cstddef>
#include <string_view>

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
    // The port of the run's registry (registry_uri of the result): a free one, different in every run.
    const std::string registry = "\"registry_uri\":";
    for (auto at = text.find(registry); at != std::string::npos; at = text.find(registry, at + 1))
    {
        const std::string uri = "\"silkit://127.0.0.1:";
        auto from = at + registry.size();
        if (from < text.size() && text[from] == ' ')
        {
            ++from;
        }
        if (text.compare(from, uri.size(), uri) == 0)
        {
            from += uri.size();
            const auto to = text.find('"', from);
            text.replace(from, to - from, "<port>");
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

namespace {

struct TextLine
{
    std::string_view text{};
    bool present = false;
    bool has_newline = false;
};

TextLine next_line(std::string_view text, std::size_t& pos)
{
    if (pos >= text.size())
    {
        return {};
    }
    const std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos)
    {
        const TextLine out{text.substr(pos), true, false};
        pos = text.size();
        return out;
    }
    const TextLine out{text.substr(pos, nl - pos), true, true};
    pos = nl + 1;
    return out;
}

std::string format_summary_line(const TextLine& line, bool show_newline_status)
{
    if (!line.present)
    {
        return "<end of text>";
    }
    std::string out;
    out.reserve(line.text.size());
    for (const char c : line.text)
    {
        if (c == '\r')
        {
            out += "\\r";
        }
        else
        {
            out += c;
        }
    }
    if (show_newline_status)
    {
        out += line.has_newline ? " (with trailing newline)" : " (no trailing newline)";
    }
    return out;
}

std::string first_difference_summary(std::string_view actual, std::string_view expected)
{
    std::size_t a_pos = 0;
    std::size_t e_pos = 0;
    std::size_t line_no = 1;
    while (a_pos < actual.size() || e_pos < expected.size())
    {
        const TextLine a = next_line(actual, a_pos);
        const TextLine e = next_line(expected, e_pos);
        if (a.present != e.present || a.text != e.text || a.has_newline != e.has_newline)
        {
            const bool only_newline = a.present && e.present && a.text == e.text && a.has_newline != e.has_newline;
            return "first difference at line " + std::to_string(line_no) + ":\n  got:      " +
                   format_summary_line(a, only_newline) + "\n  expected: " + format_summary_line(e, only_newline) +
                   "\n";
        }
        ++line_no;
    }
    return {};
}

} // namespace

void expect_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        throw std::runtime_error(what + " differs\n" + first_difference_summary(actual, expected) + "--- got ---\n" +
                                 actual + "--- expected ---\n" + expected + "---");
    }
    log("CHECK " + what + " -> ok");
}

std::vector<std::string> fixture_run_arguments(const fs::path& dir, const std::vector<std::string>& args)
{
    if (args.empty() ||
        (args.front() != "check" && args.front() != "run" && args.front() != "serve" && args.front() != "test"))
    {
        return args;
    }
    // These comments are test harness data and have no meaning to the launcher. The fixture is
    // still valid format 2 YAML. Keep explicit command-line arguments in control of each test.
    for (const std::string& arg : args)
    {
        const fs::path file = dir / vehicle_sil::process::path_from_utf8(arg);
        if (file.extension() != ".yaml" || !fs::is_regular_file(file))
        {
            continue;
        }
        const std::string text = read_text(file);
        if (!contains(text, "vehicle_sil_simulation"))
        {
            continue;
        }
        std::istringstream lines(text);
        std::string line;
        std::vector<std::string> options;
        while (std::getline(lines, line))
        {
            constexpr std::string_view prefix = "# run-option ";
            if (!line.starts_with(prefix))
            {
                continue;
            }
            std::istringstream setting(line.substr(prefix.size()));
            std::string flag;
            std::string value;
            if (setting >> flag >> value &&
                std::none_of(args.begin(), args.end(), [&](const std::string& option) {
                    return option == flag || option.starts_with(flag + "=");
                }))
            {
                options.insert(options.end(), {flag, value});
            }
        }
        std::vector<std::string> all = args;
        const std::size_t insert_at =
            (all.size() >= 2 && all[0] == "serve" && all[1] == "--once") ? 2 : 1;
        all.insert(all.begin() + static_cast<std::ptrdiff_t>(insert_at), options.begin(), options.end());
        return all;
    }
    return args;
}

namespace {

std::string json_quote(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '\\' || c == '"')
        {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string json_string_array(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            out += ',';
        }
        out += json_quote(items[i]);
    }
    out += ']';
    return out;
}

} // namespace

ServeOnceInvocation split_serve_once_arguments(const fs::path& dir, const std::vector<std::string>& raw_args)
{
    const std::vector<std::string> all = fixture_run_arguments(dir, raw_args);
    ServeOnceInvocation out;
    out.cli_args = {"serve", "--once"};

    std::optional<std::string> request_file;
    std::optional<std::string> timeline;
    std::optional<std::string> variant;
    std::optional<std::string> scenario;
    std::optional<std::string> speed;
    std::optional<std::string> stop_at_ms;
    std::optional<std::string> late_tolerance_ms;
    std::optional<std::string> time_policy;
    bool strict = false;
    bool start_paused = false;
    std::vector<std::string> disable;
    std::vector<std::string> enable;
    std::vector<std::string> manual;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::pair<std::string, std::string>> connect;
    std::vector<std::string> positionals;

    bool after_dashdash = false;
    for (std::size_t i = 1; i < all.size(); ++i)
    {
        const std::string& a = all[i];
        if (after_dashdash)
        {
            positionals.push_back(a);
            continue;
        }
        if (a == "--")
        {
            after_dashdash = true;
            continue;
        }
        if (a == "--once" || a == "--stdin-commands")
        {
            continue;
        }
        if (a == "--strict")
        {
            strict = true;
            continue;
        }
        if (a == "--start-paused")
        {
            start_paused = true;
            continue;
        }
        const auto take_val = [&](std::string& dst) -> bool {
            if (i + 1 >= all.size())
            {
                out.passthrough = true;
                return false;
            }
            dst = all[++i];
            return true;
        };
        if (a == "--log-dir" || a == "--timeout-ms" || a == "--describe-timeout-ms" ||
            a == "--participant-start-timeout-ms" || a == "--participant-stop-timeout-ms" ||
            a == "--events-stall-ms" || a == "--control" || a == "--control-credentials" ||
            a == "--control-tls-cert" || a == "--control-tls-key" || a == "--control-audit-log")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            if (a == "--events-stall-ms")
            {
                char* end = nullptr;
                errno = 0;
                const long long ms = std::strtoll(val.c_str(), &end, 10);
                if (val.empty() || *end != '\0' || errno != 0 || ms < 1000 || ms > 86'400'000)
                {
                    out.passthrough = true;
                    break;
                }
            }
            out.cli_args.push_back(a);
            out.cli_args.push_back(val);
            continue;
        }
        if (a == "--request")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            request_file = std::move(val);
            continue;
        }
        if (a == "--timeline")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            timeline = std::move(val);
            continue;
        }
        if (a == "--variant")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            variant = std::move(val);
            continue;
        }
        if (a == "--scenario")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            scenario = std::move(val);
            continue;
        }
        if (a == "--speed")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            speed = std::move(val);
            continue;
        }
        if (a == "--stop-at-ms")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            stop_at_ms = std::move(val);
            continue;
        }
        if (a == "--late-tolerance-ms")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            late_tolerance_ms = std::move(val);
            continue;
        }
        if (a == "--time-policy")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            time_policy = std::move(val);
            continue;
        }
        if (a == "--disable")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            disable.push_back(std::move(val));
            continue;
        }
        if (a == "--enable")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            enable.push_back(std::move(val));
            continue;
        }
        if (a == "--manual")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            manual.push_back(std::move(val));
            continue;
        }
        if (a == "--param")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            const auto eq = val.find('=');
            if (eq == std::string::npos)
            {
                out.passthrough = true;
                break;
            }
            params.emplace_back(val.substr(0, eq), val.substr(eq + 1));
            continue;
        }
        if (a == "--connect")
        {
            std::string val;
            if (!take_val(val))
            {
                break;
            }
            const auto eq = val.find('=');
            if (eq == std::string::npos)
            {
                out.passthrough = true;
                break;
            }
            connect.emplace_back(val.substr(0, eq), val.substr(eq + 1));
            continue;
        }
        if (!a.empty() && a.front() == '-')
        {
            out.passthrough = true;
            break;
        }
        positionals.push_back(a);
    }

    if (out.passthrough || positionals.size() != 1)
    {
        out.passthrough = true;
        out.cli_args = all;
        out.run_command_line.clear();
        return out;
    }

    out.cli_args.push_back(positionals.front());

    std::vector<std::string> intent_fields;
    if (variant)
    {
        intent_fields.push_back("\"variant\":" + json_quote(*variant));
    }
    if (scenario)
    {
        intent_fields.push_back("\"scenario\":" + json_quote(*scenario));
    }
    if (!params.empty())
    {
        std::string obj = "{";
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            if (i > 0)
            {
                obj += ',';
            }
            obj += json_quote(params[i].first) + ':' + json_quote(params[i].second);
        }
        obj += '}';
        intent_fields.push_back("\"params\":" + obj);
    }
    if (!disable.empty())
    {
        intent_fields.push_back("\"disable\":" + json_string_array(disable));
    }
    if (!enable.empty())
    {
        intent_fields.push_back("\"enable\":" + json_string_array(enable));
    }
    if (!manual.empty())
    {
        intent_fields.push_back("\"manual\":" + json_string_array(manual));
    }
    if (!connect.empty())
    {
        std::string obj = "{";
        for (std::size_t i = 0; i < connect.size(); ++i)
        {
            if (i > 0)
            {
                obj += ',';
            }
            obj += json_quote(connect[i].first) + ':' + json_quote(connect[i].second);
        }
        obj += '}';
        intent_fields.push_back("\"connect\":" + obj);
    }
    if (time_policy)
    {
        intent_fields.push_back("\"time_policy\":" + json_quote(*time_policy));
    }
    if (speed)
    {
        if (*speed == "max")
        {
            intent_fields.push_back("\"speed\":\"max\"");
        }
        else
        {
            intent_fields.push_back("\"speed\":" + *speed);
        }
    }
    if (start_paused)
    {
        intent_fields.push_back("\"start_paused\":true");
    }
    if (strict)
    {
        intent_fields.push_back("\"strict\":true");
    }
    if (stop_at_ms)
    {
        intent_fields.push_back("\"stop_at_ms\":" + *stop_at_ms);
    }
    if (late_tolerance_ms)
    {
        intent_fields.push_back("\"late_tolerance_ms\":" + *late_tolerance_ms);
    }

    std::string cmd = "{\"command\":\"run\"";
    if (request_file)
    {
        cmd += ",\"request\":" + json_quote(*request_file);
    }
    else if (!intent_fields.empty())
    {
        cmd += ",\"intent\":{";
        for (std::size_t i = 0; i < intent_fields.size(); ++i)
        {
            if (i > 0)
            {
                cmd += ',';
            }
            cmd += intent_fields[i];
        }
        cmd += '}';
    }
    if (timeline)
    {
        cmd += ",\"timeline\":" + json_quote(*timeline);
    }
    cmd += "}\n";
    out.run_command_line = std::move(cmd);
    return out;
}

Captured launch(const Options& o, const std::vector<std::string>& args, const fs::path& dir)
{
    vehicle_sil::process::CaptureOptions options;
    options.timeout = scaled_timeout(240s);
    options.working_directory = dir;
    if (!args.empty() && args.front() == "serve")
    {
        const ServeOnceInvocation inv = split_serve_once_arguments(dir, args);
        if (inv.passthrough)
        {
            return vehicle_sil::process::run_captured(o.launch, inv.cli_args, options);
        }
        std::mutex mu;
        std::string out_bytes;
        std::string err_bytes;
        vehicle_sil::process::ProcessOptions proc_opts;
        proc_opts.working_directory = dir;
        proc_opts.input = true;
        proc_opts.max_line = std::size_t{1} << 20;
        proc_opts.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
            const std::lock_guard lock(mu);
            (stream == vehicle_sil::process::Stream::Output ? out_bytes : err_bytes) += std::string(line) + "\n";
        };
        vehicle_sil::process::Process proc(o.launch, inv.cli_args, std::move(proc_opts));
        static_cast<void>(proc.write_input(inv.run_command_line));
        const auto status = proc.wait_for(options.timeout);
        if (!status)
        {
            proc.kill_tree();
        }
        proc.finish_output(10s);
        Captured cap;
        if (status)
        {
            cap.exit_code = status->code;
            cap.signal = status->signal;
        }
        const std::lock_guard lock(mu);
        cap.standard_output = std::move(out_bytes);
        cap.standard_error = std::move(err_bytes);
        return cap;
    }
    return vehicle_sil::process::run_captured(o.launch, fixture_run_arguments(dir, args), options);
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
    write_file(dir / "simulation.yaml", lf(read_text(o.example_dir / "simulation.yaml")));
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
    if (from == std::string::npos || line[from] == '}')
    {
        return {};
    }
    std::string out = line.substr(from + 1, line.size() - from - 2);
    if (out.rfind("\"run\":\"", 0) == 0)
    {
        const auto end_quote = out.find('"', 7);
        if (end_quote != std::string::npos)
        {
            if (end_quote + 1 == out.size())
            {
                return {};
            }
            if (out[end_quote + 1] == ',')
            {
                out.erase(0, end_quote + 2);
            }
        }
    }
    return out;
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
                                      "vehicle_sil_simulation: 2\n" + keys +
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
