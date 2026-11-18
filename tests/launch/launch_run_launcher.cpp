// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run as a process, with the ECU executables of examples/ecu_main, an
// ecu_main executable that misbehaves as a file in its working directory says (run_test_ecu.cpp)
// and the helper of tests/process.
//
// The scenarios are in launch_run_<topic>.cpp, one file for each topic, and each file lists its
// scenarios at the top. The code they share is in launch_run_support.hpp. main() runs the scenario
// that --scenario names.
#include "launch_run_support.hpp"

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.launch = args.required("launch");
    o.test_ecu = args.required("test-ecu");
    o.helper = args.required("helper");
    o.example_dir = args.required("example-dir");
    o.docs = args.required("docs");
    o.work_dir = args.required("work-dir");
    o.scenario = args.required("scenario");
    o.connect_tools = args.str("connect-tools", "");
    o.bridge_ecu = args.str("bridge-ecu", "");
    o.fake_pcap_dir = args.str("fake-pcap-dir", "");
    return o;
}

int main(int argc, char** argv)
{
#ifndef _WIN32
    // A write to the input of a launcher that has ended fails instead of ending this test.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    set_log_tag("launch-run");
    try
    {
        vehicle_sil::test::check_timeout_scale();
        const Options o = parse(argc, argv);
        fs::remove_all(o.work_dir);
        fs::create_directories(o.work_dir);
        log("scenario " + o.scenario);
        // One a line, in the order of the names (the order has no meaning: the name is looked up).
        const std::vector<std::pair<std::string, void (*)(const Options&)>> scenarios = {
            {"busy_port", busy_port},
            {"check", check},
            {"connect_dies_while_waiting", connect_dies_while_waiting},
            {"connect_lost_exit0", connect_lost_exit0},
            {"connect_lost_kill", connect_lost_kill},
            {"connect_no_adapter", connect_no_adapter},
            {"connect_nothing_received", connect_nothing_received},
            {"connect_roundtrip", connect_roundtrip},
            {"connect_speed", connect_speed},
            {"connect_time_changed", connect_time_changed},
            {"control", control},
            {"crash", crash},
            {"disabled", disabled},
            {"docs", docs},
            {"ended_early", ended_early},
            {"ethernet", ethernet},
            {"events", events},
            {"events_findings", events_findings},
            {"events_nonblocking", events_nonblocking},
            {"events_output", events_output},
            {"events_slow", events_slow},
            {"events_stalled", events_stalled},
            {"events_stderr", events_stderr},
            {"events_usage", events_usage},
            {"grandchild", grandchild},
            {"hang", hang},
            {"interrupt", interrupt},
            {"interrupt_ending", interrupt_ending},
            {"interrupt_twice", interrupt_twice},
            {"j1939_nodes", j1939_nodes},
            {"killed", killed},
            {"late", late},
            {"live", live},
            {"manual", manual},
            {"manual_again", manual_again},
            {"manual_fails", manual_fails},
            {"manual_frozen", manual_frozen},
            {"manual_late", manual_late},
            {"manual_stop", manual_stop},
            {"manual_stranger", manual_stranger},
            {"manual_thawed", manual_thawed},
            {"manual_timeout", manual_timeout},
            {"network", network},
            {"network_names", network_names},
            {"traffic", traffic},
            {"no_console", no_console},
            {"no_console_stdin", no_console_stdin},
            {"not_joined", not_joined},
            {"on_time", on_time},
            {"optional", optional},
            {"output_closed", output_closed},
            {"path", path},
            {"pause_step", pause_step},
            {"run_until_stdin", run_until_stdin},
            {"paused_failures", paused_failures},
            {"quitter", quitter},
            {"realtime", realtime},
            {"scenario", scenario},
            {"scenario_disconnect", scenario_disconnect},
            {"scenario_each_write", scenario_each_write},
            {"scenario_failures", scenario_failures},
            {"scenario_same", scenario_same},
            {"scenario_wave", scenario_wave},
            {"scenario_dither", scenario_dither},
            {"timeline", timeline},
            {"scenario_expect_beside", scenario_expect_beside},
            {"speed_change", speed_change},
            {"stall", stall},
            {"stand_still", stand_still},
            {"stdin_ending", stdin_ending},
            {"stdin_eof", stdin_eof},
            {"stdin_killed", stdin_killed},
            {"stdin_stop", stdin_stop},
            {"stdin_twice", stdin_twice},
            {"step", step},
            {"step_width", step_width},
            {"stop_once", stop_once},
            {"test_command", test_command},
            {"test_interrupt", test_interrupt},
            {"time_usage", time_usage},
            {"timeout", timeout},
            {"usage", usage},
            {"variable_map", variable_map},
            {"variants", variants},
        };
        const auto it = std::find_if(scenarios.begin(), scenarios.end(), [&](const auto& s) { return s.first == o.scenario; });
        if (it == scenarios.end())
        {
            throw std::invalid_argument("unknown scenario " + o.scenario);
        }
        it->second(o);
        log("PASS");
        return 0;
    }
    catch (const Skip& e)
    {
        log(std::string("SKIP: ") + e.what());
        return 77;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
