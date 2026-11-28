// SPDX-License-Identifier: MIT
//
// The scenario files that the launcher reads beside a scenario (files_beside()) and the timeline
// of a run as the launcher writes it (format_timeline_json()): the same model as the scenario's
// tests (tests/scenario), compared with the files of tests/launch/timeline.
#include "launch_test_util.hpp"
#include "scenario_model.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/timeline_format.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_check.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/scenario/timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using namespace vehicle_sil::scenario::test::scenario_model;

std::string file_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw unit::Failure("cannot read " + path.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string out = text.str();
    std::erase(out, '\r');
    return out;
}

TEST_CASE(scenario_files_are_read_beside_the_scenario_file)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "points-files";
    fs::remove_all(dir);
    fs::create_directories(dir / "traces");
    // A name that is not ASCII: the path is UTF-8 on every OS.
    const std::string name = "traces/\xE8\xB5\xB0\xE8\xA1\x8C.csv";
    std::ofstream(dir / process::path_from_utf8(name), std::ios::binary) << "time,level\n0,1\n";
    const auto read = files_beside(dir / "s.yaml");
    std::string problem;
    CHECK(read(name, problem) == std::optional<std::string>{"time,level\n0,1\n"});
    CHECK(!read("traces", problem).has_value());
    check_text(problem, "is not a regular file: name the CSV file");
    CHECK(!read("none.csv", problem).has_value());
    CHECK(problem.rfind("does not exist (looked for ", 0) == 0);
    CHECK(problem.find("): the path is from the scenario file's directory; check it") != std::string::npos);

    const auto readTpl = files_beside(dir / "s.yaml", "the template file");
    CHECK(!readTpl("traces", problem).has_value());
    check_text(problem, "is not a regular file: name the template file");
}

/// The timeline file of `text` run against the model, as `vehicle_sil_launch run --timeline`
/// makes it (the findings: those of the failed steps, as the run's, then `findings`).
std::string timeline_of(const std::string& text, SimDuration tick, const std::function<void(World&)>& setup = {},
                        const ScenarioCatalog& described = catalog(), TimelineLimits limits = {}, const ScenarioContext& context = {},
                        const std::vector<Finding>& findings = {})
{
    ScenarioRead r = read_scenario(text, described, context);
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(r.findings));
    }
    const Scenario s = *r.scenario;
    World world;
    world.tick = tick;
    if (setup)
    {
        setup(world);
    }
    TimelineRecord record(s, limits);
    world.record = &record;
    ScenarioRun run = world.run(s, world.ends_after.value_or(5s));
    run.file = "scenario.yaml";
    TimelineInput input;
    input.file = "simulation.yaml";
    input.time_step_ms = ms_of(at(tick));
    input.scenario = &s;
    input.run = &run;
    input.catalog = &described;
    input.record = &record;
    for (const ScenarioStepRun& step : run.steps)
    {
        if (step.status == StepStatus::Failed)
        {
            input.findings.push_back(Finding{Severity::Error, step.code, step.text + ": " + step.message, step.line, "", "", "", run.file});
        }
    }
    input.findings.insert(input.findings.end(), findings.begin(), findings.end());
    return format_timeline_json(input);
}

/// A timeline file without what is not compared between runs with different steps of the
/// controller and between builds: the version, the files' names, when the controller saw the
/// scenario over, its step.
std::string compared(const std::string& timeline)
{
    std::istringstream in(timeline);
    std::string out;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const bool left_out = std::ranges::any_of(
            std::vector<std::string>{"  \"vehicle_sil\": ", "  \"file\": ", "  \"time_step_ms\": ", "    \"file\": ", "    \"over_at_ms\": "},
            [&line](const std::string& start) { return line.rfind(start, 0) == 0; });
        if (!left_out)
        {
            out += line + "\n";
        }
    }
    return out;
}

/// The part of a timeline file from `from` up to (not with) `to`.
std::string part(const std::string& timeline, const std::string& from, const std::string& to)
{
    const auto begin = timeline.find(from);
    const auto end = begin == std::string::npos ? begin : timeline.find(to, begin);
    if (end == std::string::npos)
    {
        throw unit::Failure("the timeline has no '" + from + "' ... '" + to + "':\n" + timeline);
    }
    return timeline.substr(begin, end - begin);
}

TEST_CASE(timeline_of_a_run_does_not_depend_on_the_controllers_step)
{
    // A scenario that passes, one that fails at a sample and one whose wait runs out: the file is
    // the same whether the controller steps every 10, 20, 30 or 4 ms, though the engine then looks
    // at the samples and learns the results at other times. Door steps every 4 ms (it samples
    // every 20 ms then). None has a step that waits and becomes true: such a step ends one step
    // of the controller after the engine knows (ScenarioEngine), so it and the steps after it
    // have other times with another step.
    const std::string passes = kHead + "  - set: { Body.level: 10, Door.level: 3 }\n"
                                       "  - at_ms: 20\n"
                                       "    expect: { Body.body.speed: { ge: 100 } }\n"
                                       "  - after_ms: 15\n"
                                       "    ramp: { Door.level: { to: 9, over_ms: 50 }, Body.level: { to: 12, over_ms: 35 } }\n"
                                       "    expect: { Door.level: { ge: 3 }, Body.body.speed: { le: 120 } }\n"
                                       "  - set: { Door.lamp: true }\n"
                                       "  - after_ms: 20\n"
                                       "    expect: { Door.level: 9 }\n"
                                       "    hold_for_ms: 45\n"
                                       "  - set: { Body.level: 0 }\n";
    const std::string fails = kHead + "  - set: { Body.level: 10 }\n"
                                      "  - at_ms: 15\n"
                                      "    expect: { Body.body.speed: { le: 60 }, Door.level: 0 }\n"
                                      "    hold_for_ms: 100\n"
                                      "  - set: { Body.level: 0 }\n";
    const std::string times_out = kHead + "  - wave: { Door.level: { square: { low: 1, high: 2, period_ms: 16 }, for_ms: 64 } }\n"
                                          "    expect: { Door.level: { ge: 0 } }\n"
                                          "  - wait_until: { Body.body.speed: { gt: 0 }, Door.level: 2 }\n"
                                          "    timeout_ms: 75\n";
    const auto door4 = [](World& w) { w.ecus[1].step = 4ms; };
    for (const std::string& text : {passes, fails, times_out})
    {
        const std::string first = compared(timeline_of(text, 10ms, door4, catalog(10ms, 4ms)));
        CHECK(first.find("\"recorded_to_ms\": null") == std::string::npos);
        for (const SimDuration tick : {SimDuration{20ms}, SimDuration{30ms}, SimDuration{4ms}, SimDuration{5ms}})
        {
            check_text(compared(timeline_of(text, tick, door4, catalog(10ms, 4ms))), first, "the timeline with another step");
        }
        // Nor on when what a step makes arrives.
        for (const auto& [name, policy] : policies())
        {
            check_text(compared(timeline_of(text, 10ms, [&](World& w) { door4(w); w.policy = policy; }, catalog(10ms, 4ms))), first,
                       name.c_str());
        }
    }
    // The lines of the inputs are the same too (they are in the files compared above). Here the
    // expect fails at 15 ms. The ramp at 25 ms is put on its line by a controller that steps
    // every 30 ms (it sends the writes up to 30 ms at 0 ms), and not by one that steps every 5 or
    // 10 ms (it knows of the failure at 20 ms, before the writes for 25 ms). The file has the
    // lines put up to "recorded to" (15 ms): the set, and with no step the ramp.
    const std::string ahead = kHead + "  - set: { Body.level: 10 }\n"
                                      "  - at_ms: 15\n"
                                      "    expect: { Body.body.speed: { le: 60 } }\n"
                                      "  - at_ms: 25\n"
                                      "    ramp: { Door.level: { from: 1, to: 9, over_ms: 30 } }\n";
    const auto lines_of = [](const std::string& timeline) { return part(timeline, "  \"lines\": [\n", "  \"findings\""); };
    for (const SimDuration tick : {SimDuration{5ms}, SimDuration{10ms}, SimDuration{30ms}})
    {
        const std::string made = timeline_of(ahead, tick);
        CHECK(made.find("    \"recorded_to_ms\": 15,\n") != std::string::npos);
        check_text(lines_of(made),
                   "  \"lines\": [\n    {\n      \"variable\": \"Body.level\",\n      \"kind\": \"set\",\n      \"line\": 3,\n"
                   "      \"step\": 1,\n      \"from_ms\": 0,\n      \"to_ms\": 0,\n      \"base\": 10,\n"
                   "      \"base_source\": \"written\",\n      \"shape\": null,\n      \"text\": \"Body.level = 10\"\n    }\n  ],\n",
                   "the lines with another step");
        // (The step's own list follows the lines of the file.)
        CHECK(made.find("      \"lines\": [0],\n") != std::string::npos && made.find("      \"lines\": [1],\n") == std::string::npos);
    }
    // The watch beside the ramp goes to the step of Door that applies the end value (88 ms), past
    // the step's end (85 ms); the last step passed at 150 ms, and that is where the file ends.
    const std::string passed = timeline_of(passes, 20ms, door4, catalog(10ms, 4ms));
    check_text(part(passed, "        \"mode\": \"during\",", "        \"conditions\""),
               "        \"mode\": \"during\",\n        \"from_ms\": 35,\n        \"to_ms\": 88,\n");
    CHECK(passed.find("      \"start_ms\": 35,\n      \"at_ms\": 85,\n      \"end_ms\": 85,\n") != std::string::npos);
    check_text(part(passed, "  \"scenario\": {", "  \"variables\""),
               "  \"scenario\": {\n    \"file\": \"scenario.yaml\",\n    \"name\": null,\n    \"status\": \"passed\",\n"
               "    \"over_at_ms\": 160,\n    \"recorded_to_ms\": 150,\n    \"end_ms\": null\n  },\n");
    // What is in it: the hold from 15 ms fails at once with the sample of 10 ms, so the file has
    // what was written and sampled up to 15 ms and nothing after it, though the controller (every
    // 30 ms) saw the scenario over at 30 ms, with the samples of 20 ms there.
    const std::string failed = timeline_of(fails, 30ms, door4, catalog(10ms, 4ms));
    CHECK(failed.find("    \"over_at_ms\": 30,\n    \"recorded_to_ms\": 15,\n    \"end_ms\": null\n") != std::string::npos);
    CHECK(failed.find("      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
                      "      \"columns\": [\"at_ms\", \"value\"],\n      \"samples\": null,\n      \"first_ms\": null,\n      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": [\n        [0, 10]\n      ]") != std::string::npos);
    CHECK(failed.find("      \"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\",\n      \"encoding\": \"changes\",\n"
                      "      \"columns\": [\"at_ms\", \"value\"],\n"
                      "      \"samples\": 2,\n"
                      "      \"first_ms\": 0,\n"
                      "      \"last_ms\": 10,\n"
                      "      \"not_applied_from\": null,\n"
                      "      \"points\": [\n"
                      "        [0, 50],\n        [10, 100]\n      ]") != std::string::npos);
    CHECK(failed.find("      \"failed\": {\n        \"at_ms\": 15,\n        \"code\": \"scenario-failed\",\n        \"conditions\": [\n"
                      "          {\n            \"condition\": 0,\n            \"value\": 100,\n            \"sample_ms\": 10\n          }\n"
                      "        ],\n        \"writes\": [],\n        \"write\": null\n      }") != std::string::npos);
}

TEST_CASE(timeline_files_are_the_references)
{
    // tests/launch/timeline: each scenario with the timeline file the engine writes for it against
    // the model (README.md there: what is not compared, and how to write the references again).
    // The second list: the other steps of the controller that give the same file (none for a
    // scenario with a step that waits and becomes true, or with a write that an ECU refuses: what
    // the engine learns first then depends on the step).
    const std::filesystem::path directory = std::filesystem::path(VEHICLE_SIL_SOURCE_DIR) / "tests" / "launch" / "timeline";
    const char* update = std::getenv("VEHICLE_SIL_UPDATE_TIMELINE_REFERENCES");
    const bool writing = update != nullptr && std::string(update) == "1";
    const std::vector<std::pair<std::string, std::vector<SimDuration>>> references = {
        {"motions", {20ms, 30ms}},
        {"fails_beside", {20ms, 30ms}},
        {"fails_wait", {20ms, 30ms}},
        {"waits", {}},
        {"write_refused", {}},
        {"points_file", {20ms, 30ms}},
        {"dither", {5ms, 20ms, 30ms}},
    };
    for (const auto& [name, steps] : references)
    {
        const std::string scenario = file_text(directory / (name + ".yaml"));
        const std::filesystem::path reference = directory / (name + ".timeline.json");
        // The points of a wave may be in a CSV file beside the scenario: <name>.csv.
        ScenarioContext context;
        if (std::filesystem::exists(directory / (name + ".csv")))
        {
            context = with_files({{name + ".csv", file_text(directory / (name + ".csv"))}});
        }
        const auto timeline = [&](SimDuration step) { return timeline_of(scenario, step, {}, catalog(), {}, context); };
        const std::string made = timeline(10ms);
        if (writing)
        {
            std::ofstream out(reference, std::ios::binary | std::ios::trunc);
            out << made;
            if (!out.flush())
            {
                throw unit::Failure("cannot write " + reference.string());
            }
        }
        check_text(compared(made), compared(file_text(reference)),
                   (name + ".timeline.json (if the change is meant: tests/launch/timeline/README.md says how to write it again)").c_str());
        for (const SimDuration step : steps)
        {
            check_text(compared(timeline(step)), compared(made), (name + " with another step of the controller").c_str());
        }
    }
}

TEST_CASE(timeline_keeps_the_newest_points_at_its_limits)
{
    // A ramp of 300 ms (a write every 10 ms) between two sets, and its speed in every sample: with
    // 8 points a series, the 8 newest of the ramp stay, and both sets; of the samples the 8 newest.
    const std::string text = kHead + "  - set: { Body.level: 2 }\n"
                                     "  - ramp: { Body.level: { to: 92, over_ms: 300 } }\n"
                                     "    expect: { Body.body.speed: { ge: 0 } }\n"
                                     "  - set: { Body.level: 0 }\n"
                                     "  - after_ms: 20\n"
                                     "    expect: { Body.body.speed: { ge: 0 } }\n";
    const std::string limited = timeline_of(text, 10ms, {}, catalog(), TimelineLimits{8, 1000});
    check_text(part(limited, "  \"series\": [", "  \"lines\""),
               "  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [0, 2],\n"
               "        [230, 71],\n"
               "        [240, 74],\n"
               "        [250, 77],\n"
               "        [260, 80],\n"
               "        [270, 83],\n"
               "        [280, 86],\n"
               "        [290, 89],\n"
               "        [300, 92],\n"
               "        [300, 0]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.body.speed\",\n"
               "      \"source\": \"sampled\",\n"
               "      \"encoding\": \"changes\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": 33,\n"
               "      \"first_ms\": 0,\n"
               "      \"last_ms\": 320,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [250, 770],\n"
               "        [260, 800],\n"
               "        [270, 830],\n"
               "        [280, 860],\n"
               "        [290, 890],\n"
               "        [300, 840],\n"
               "        [310, 790],\n"
               "        [320, 740]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
    check_text(part(limited, "  \"limits\": {", "\n}\n"),
               "  \"limits\": {\n"
               "    \"points_per_series\": 8,\n"
               "    \"points_total\": 1000,\n"
               "    \"dropped\": [\n"
               "      {\n"
               "        \"variable\": \"Body.level\",\n"
               "        \"source\": \"written\",\n"
               "        \"dropped\": 23,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 230\n"
               "      },\n"
               "      {\n"
               "        \"variable\": \"Body.body.speed\",\n"
               "        \"source\": \"sampled\",\n"
               "        \"dropped\": 25,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 250\n"
               "      }\n"
               "    ]\n"
               "  }");
    // The same with another step of the controller, which has looked further ahead when the
    // scenario is over.
    check_text(compared(timeline_of(text, 30ms, {}, catalog(), TimelineLimits{8, 1000})), compared(limited), "limited, another step");
    // All series together: the ones with the most points give first, down to the same number.
    const std::string total = timeline_of(text, 10ms, {}, catalog(), TimelineLimits{1000, 10});
    check_text(part(total, "  \"limits\": {", "\n}\n"),
               "  \"limits\": {\n"
               "    \"points_per_series\": 1000,\n"
               "    \"points_total\": 10,\n"
               "    \"dropped\": [\n"
               "      {\n"
               "        \"variable\": \"Body.level\",\n"
               "        \"source\": \"written\",\n"
               "        \"dropped\": 26,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 260\n"
               "      },\n"
               "      {\n"
               "        \"variable\": \"Body.body.speed\",\n"
               "        \"source\": \"sampled\",\n"
               "        \"dropped\": 28,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 280\n"
               "      }\n"
               "    ]\n"
               "  }");
    check_text(compared(timeline_of(text, 30ms, {}, catalog(), TimelineLimits{1000, 10})), compared(total), "total, another step");
}

TEST_CASE(timeline_counts_the_samples_up_to_the_time_it_is_closed_at)
{
    // Samples of one value are kept as one point with their number; closed at a time between
    // them, the record has the samples up to that time, also where they do not come evenly.
    const Scenario s = read(kHead + "  - expect: { Body.level: 0, Body.body.speed: 0 }\n");
    const auto closed_at = [&s](SimDuration to) {
        TimelineRecord record(s);
        for (const std::int64_t t : {0, 10, 20, 25, 35, 45, 55})
        {
            record.sampled(0, at(std::chrono::milliseconds(t)), control::Value{std::int64_t{7}});
            record.sampled(1, at(std::chrono::milliseconds(t)), t < 35 ? control::Value{0.0} : control::Value{-0.0});
        }
        record.close(at(to));
        std::string out;
        for (const auto& series : record.series())
        {
            out += std::to_string(series.sample_count) + " samples, last " + time_text(series.last_sample) + ":";
            for (const auto& point : series.samples)
            {
                out += point.repeats ? "" : " " + std::to_string(ms_of(point.first)) + "=" + value_text(point.value);
            }
            out += "\n";
        }
        return out;
    };
    // 0 and -0 are written differently: a point each.
    check_text(closed_at(100ms), "7 samples, last 55: 0=7\n7 samples, last 55: 0=0 35=-0\n");
    check_text(closed_at(54ms), "6 samples, last 45: 0=7\n6 samples, last 45: 0=0 35=-0\n");
    check_text(closed_at(35ms), "5 samples, last 35: 0=7\n5 samples, last 35: 0=0 35=-0\n");
    check_text(closed_at(34ms), "4 samples, last 25: 0=7\n4 samples, last 25: 0=0\n");
    check_text(closed_at(19ms), "2 samples, last 10: 0=7\n2 samples, last 10: 0=0\n");
    check_text(closed_at(0ms), "1 samples, last 0: 0=7\n1 samples, last 0: 0=0\n");
    // Of the findings the file has those of the scenario, and that an ECU's values came late (its
    // code alone); nothing of the pace in real time, which differs from run to run.
    bool code_only = true;
    CHECK(timeline_takes_finding("scenario-failed", code_only) && !code_only);
    CHECK(timeline_takes_finding("control-link-late-values", code_only) && code_only);
    CHECK(!timeline_takes_finding("behind-real-time", code_only));
    CHECK(!timeline_takes_finding("timeline-write", code_only));
    CHECK(!timeline_takes_finding("exit-code", code_only));
    // Nothing is noted once it is closed.
    TimelineRecord record(s);
    record.close(at(10ms));
    record.sampled(0, at(0ms), control::Value{std::int64_t{1}});
    CHECK(record.series()[0].samples.empty());
}

TEST_CASE(timeline_writes_every_key_of_a_late_values_finding)
{
    // Its code and severity, and null for what may differ from run to run: the file has the same
    // keys in every finding.
    const std::string timeline =
        timeline_of(kHead + "  - set: { Body.level: 7 }\n", 10ms, {}, catalog(), {}, {},
                    {Finding{Severity::Warning, "control-link-late-values", "participant 'Body': 3 messages came late", 0, "Body", "", "", ""},
                     Finding{Severity::Warning, "behind-real-time", "the run fell behind real time", 0, "", "", "", ""}});
    check_text(part(timeline, "  \"findings\": [", "  \"limits\""), "  \"findings\": [\n"
                                                                    "    {\n"
                                                                    "      \"severity\": \"warning\",\n"
                                                                    "      \"code\": \"control-link-late-values\",\n"
                                                                    "      \"file\": null,\n"
                                                                    "      \"line\": null,\n"
                                                                    "      \"participant\": null,\n"
                                                                    "      \"network\": null,\n"
                                                                    "      \"message\": null,\n"
                                                                    "      \"detail\": null\n"
                                                                    "    }\n"
                                                                    "  ],\n");
}

TEST_CASE(timeline_marks_the_write_that_was_refused)
{
    // A written point is what was sent. The write a step failed of is a point too, and the series
    // says from where on its points were not applied; the failure names the point.
    // (A description without the range of level: the check lets the value through, as with an
    // ECU that says less than its agent checks.)
    ScenarioCatalog loose = catalog();
    loose[0].variables[0].max.reset();
    const auto refused = [&loose](const std::string& text, SimDuration tick) {
        const std::string timeline = timeline_of(text, tick, {}, loose);
        return part(timeline, "  \"scenario\": {", "  \"variables\"") + part(timeline, "        \"writes\": [", "\n      }\n") +
               part(timeline, "  \"series\": [", "  \"lines\"");
    };
    // Refused at once by the agent (out of range), between two steps of the controller: over at
    // 30 ms, the write is for 40 ms.
    check_text(refused(kHead + "  - set: { Body.level: 7 }\n"
                               "  - set: { Body.level: 95 }\n"
                               "  - at_ms: 30\n"
                               "    wave: { Body.level: { triangle: { amplitude: 10, period_ms: 40 }, for_ms: 40 } }\n",
                       10ms),
               "  \"scenario\": {\n"
               "    \"file\": \"scenario.yaml\",\n"
               "    \"name\": null,\n"
               "    \"status\": \"failed\",\n"
               "    \"over_at_ms\": 30,\n"
               "    \"recorded_to_ms\": 40,\n"
               "    \"end_ms\": null\n"
               "  },\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 40,\n"
               "            \"value\": 105,\n"
               "            \"point\": 3,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 0"
               "  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 3,\n"
               "      \"points\": [\n"
               "        [0, 7],\n"
               "        [0, 95],\n"
               "        [30, 95],\n"
               "        [40, 105]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
    // The same at a step of the controller: the write for 0 ms is refused at 0 ms, where the
    // scenario is over.
    check_text(refused(kHead + "  - set: { Body.level: 102 }\n", 10ms),
               "  \"scenario\": {\n"
               "    \"file\": \"scenario.yaml\",\n"
               "    \"name\": null,\n"
               "    \"status\": \"failed\",\n"
               "    \"over_at_ms\": 0,\n"
               "    \"recorded_to_ms\": 0,\n"
               "    \"end_ms\": null\n"
               "  },\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 0,\n"
               "            \"value\": 102,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 0"
               "  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [0, 102]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
}

TEST_CASE(timeline_says_which_writes_of_a_failed_set_were_applied)
{
    // An ECU applies the writes of one command one after the other and stops at the one whose
    // setter throws: the writes before it are applied. The file says so per write, marks only the
    // points that were not applied, and names the write that is the reason.
    const auto failed = [](const std::string& text, const ScenarioCatalog& described = catalog()) {
        const std::string timeline = timeline_of(text, 10ms, {}, described);
        return part(timeline, "      \"message\": \"the write", "\n") + "\n" + part(timeline, "        \"writes\": [", "\n      }\n") +
               part(timeline, "  \"series\": [", "  \"lines\"");
    };
    // The setter of the second of three writes throws (level 13); a write of another
    // participant in the same set is applied. The sample of 20 ms has the mode that was applied.
    check_text(failed(kHead + "  - set: { Body.level: 5, Body.lamp: false }\n"
                              "  - at_ms: 20\n"
                              "    set: { Body.mode: 2, Body.level: 13, Body.lamp: true, Door.level: 4 }\n"
                              "  - at_ms: 60\n"
                              "    expect: { Body.mode: auto }\n"),
               "      \"message\": \"the write for 20 ms was not applied by participant 'Body': the ECU rejected it (level: a setter threw: 13 is not a level); the ECU had already applied the write before it in that command (Body.mode = auto (2)), and it stays applied\",\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.mode\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 2,\n"
               "            \"point\": 0,\n"
               "            \"applied\": true\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 13,\n"
               "            \"point\": 1,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.lamp\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": true,\n"
               "            \"point\": 1,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Door.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 4,\n"
               "            \"point\": 0,\n"
               "            \"applied\": true\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 1  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 1,\n"
               "      \"points\": [\n"
               "        [0, 5],\n"
               "        [20, 13]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.lamp\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 1,\n"
               "      \"points\": [\n"
               "        [0, false],\n"
               "        [20, true]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.mode\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [20, 2]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.mode\",\n"
               "      \"source\": \"sampled\",\n"
               "      \"encoding\": \"changes\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": 3,\n"
               "      \"first_ms\": 0,\n"
               "      \"last_ms\": 20,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [0, 0],\n"
               "        [20, 2]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Door.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [20, 4]\n"
               "      ]\n"
               "    }\n"
               "  ],\n"
               "");
    // The first write of the command: nothing of it was applied.
    check_text(failed(kHead + "  - at_ms: 20\n"
                              "    set: { Body.level: 13, Body.mode: 2 }\n"),
               "      \"message\": \"the write for 20 ms was not applied by participant 'Body': the ECU rejected it (level: a setter threw: 13 is not a level); none of the 2 writes of that command was applied\",\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 13,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.mode\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 2,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 0  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [20, 13]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.mode\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [20, 2]\n"
               "      ]\n"
               "    }\n"
               "  ],\n"
               "");
    // A parameter and an input in one command, sent after the first step: the ECU's agent refuses
    // the command as a whole when it arrives. None of its writes was applied (the result says so:
    // 0), and no write is named as the reason (the refusal is about the command).
    // (Described as an input, so that the check lets it through: as with an ECU that says less
    // than its agent checks.)
    ScenarioCatalog unnamed = catalog();
    unnamed[0].variables[1].causality = control::Causality::Input;
    check_text(failed(kHead + "  - set: { Body.level: 5 }\n"
                              "  - at_ms: 20\n"
                              "    set: { Body.mode: 2, Body.gain: 3 }\n",
                      unnamed),
               "      \"message\": \"the write for 20 ms was not taken by participant 'Body': the ECU's agent refused it (a parameter is writable only before the first step); none of the 2 writes of that command was applied\",\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.mode\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 2,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.gain\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 3,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": null  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [0, 5]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.mode\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [20, 2]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.gain\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [20, 3]\n"
               "      ]\n"
               "    }\n"
               "  ],\n"
               "");
    // Refused by the agent's check when it got the command: none of its writes was applied, and
    // the reason names the write.
    ScenarioCatalog loose = catalog();
    loose[0].variables[0].max.reset();
    const std::string timeline = timeline_of(kHead + "  - at_ms: 20\n"
                                                     "    set: { Body.mode: 2, Body.level: 102 }\n",
                                             10ms, {}, loose);
    check_text(part(timeline, "      \"message\": \"the write", "\n") + "\n" + part(timeline, "        \"writes\": [", "\n      }\n"),
               "      \"message\": \"the write for 20 ms was not taken by participant 'Body': the ECU's agent refused it (level: out of range); none of the 2 writes of that command was applied\",\n"
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.mode\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 2,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 102,\n"
               "            \"point\": 0,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 1");
}

TEST_CASE(timeline_marks_the_point_of_the_failed_write_among_points_of_one_time)
{
    // Two steps write the same variable for the same time, as two commands. The first is the
    // one the ECU rejects: its point is marked, not the last point of that time.
    const std::string timeline = timeline_of(kHead + "  - set: { Body.level: 5 }\n"
                                                     "  - at_ms: 20\n"
                                                     "    set: { Body.level: 13 }\n"
                                                     "  - at_ms: 20\n"
                                                     "    set: { Body.level: 8 }\n",
                                             10ms);
    check_text(part(timeline, "        \"writes\": [", "\n      }\n") + part(timeline, "  \"series\": [", "  \"lines\""),
               "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 13,\n"
               "            \"point\": 1,\n"
               "            \"applied\": false\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 0  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 1,\n"
               "      \"points\": [\n"
               "        [0, 5],\n"
               "        [20, 13],\n"
               "        [20, 8]\n"
               "      ]\n"
               "    }\n"
               "  ],\n"
               "");
}

TEST_CASE(timeline_of_a_failed_run_has_the_steps_as_the_controllers_step_saw_them)
{
    // An expect that the sample of 10 ms fails, and a set for 20 ms after it. What becomes of the
    // set depends on the controller's step: with 5 ms the failure is seen before the set is sent;
    // with 10 ms the set is sent and not judged yet; with 30 ms it was sent at 0 ms and is seen
    // applied at 30 ms, before the sample is looked at.
    const std::string text = kHead + "  - set: { Body.level: 5 }\n"
                                     "  - at_ms: 10\n"
                                     "    expect: { Body.body.speed: 0 }\n"
                                     "  - at_ms: 20\n"
                                     "    set: { Body.level: 6 }\n";
    const auto steps = [&text](SimDuration tick) {
        const std::string timeline = timeline_of(text, tick);
        std::string out = part(timeline, "    \"recorded_to_ms\"", "  },");
        std::istringstream in(part(timeline, "  \"steps\": [", "  \"series\""));
        for (std::string line; std::getline(in, line);)
        {
            for (const char* key : {"\"index\"", "\"status\"", "\"start_ms\"", "\"at_ms\"", "\"end_ms\""})
            {
                if (line.rfind(std::string("      ") + key, 0) == 0)
                {
                    out += line.substr(6) + (std::string(key) == "\"end_ms\"" ? "\n" : " ");
                }
            }
        }
        return out + part(timeline, "  \"series\": [", "  \"lines\"");
    };
    // The series and where they end are the same: they go to the failure, and the set for 20 ms
    // is no point of them, also where its step passed.
    const std::string same =
        "    \"recorded_to_ms\": 10,\n    \"end_ms\": null\n"
        "\"index\": 1, \"status\": \"passed\", \"start_ms\": 0, \"at_ms\": 0, \"end_ms\": 0,\n"
        "\"index\": 2, \"status\": \"failed\", \"start_ms\": 10, \"at_ms\": 10, \"end_ms\": 10,\n";
    const std::string series =
        "  \"series\": [\n"
        "    {\n"
        "      \"variable\": \"Body.level\",\n"
        "      \"source\": \"written\",\n"
        "      \"encoding\": \"all\",\n"
        "      \"columns\": [\"at_ms\", \"value\"],\n"
        "      \"samples\": null,\n"
        "      \"first_ms\": null,\n"
        "      \"last_ms\": null,\n"
        "      \"not_applied_from\": null,\n"
        "      \"points\": [\n"
        "        [0, 5]\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"variable\": \"Body.body.speed\",\n"
        "      \"source\": \"sampled\",\n"
        "      \"encoding\": \"changes\",\n"
        "      \"columns\": [\"at_ms\", \"value\"],\n"
        "      \"samples\": 2,\n"
        "      \"first_ms\": 0,\n"
        "      \"last_ms\": 10,\n"
        "      \"not_applied_from\": null,\n"
        "      \"points\": [\n"
        "        [0, 50]\n"
        "      ]\n"
        "    }\n"
        "  ],\n";
    check_text(steps(5ms),
               same + "\"index\": 3, \"status\": \"not_run\", \"start_ms\": null, \"at_ms\": null, \"end_ms\": null,\n" + series);
    check_text(steps(10ms),
               same + "\"index\": 3, \"status\": \"unfinished\", \"start_ms\": 20, \"at_ms\": null, \"end_ms\": null,\n" + series);
    check_text(steps(30ms),
               same + "\"index\": 3, \"status\": \"passed\", \"start_ms\": 20, \"at_ms\": 20, \"end_ms\": 20,\n" + series);
}

TEST_CASE(a_scheduled_stop_can_close_an_open_watch_but_cannot_open_the_next_step)
{
    World ends;
    ends.stop_at = at(30ms);
    const auto run = ends.run(read(kHead + "  - expect: { Body.level: 0 }\n"
                                          "    hold_for_ms: 30\n"
                                          "  - set: { Body.level: 13 }\n"));
    CHECK(run.status == ScenarioStatus::Incomplete);
    CHECK(run.steps[0].status == StepStatus::Passed && run.steps[0].at == at(30ms));
    CHECK(run.steps[1].status == StepStatus::NotRun);
    CHECK(ends.log.empty());

    World rounded;
    rounded.stop_at = at(30ms);
    const auto pending = rounded.run(read(kHead + "  - at_ms: 29\n"
                                                   "    set: { Body.level: 13 }\n"));
    CHECK(pending.status == ScenarioStatus::Incomplete);
    CHECK(rounded.log.empty());

    World expires;
    expires.stop_at = at(30ms);
    const auto failed = expires.run(read(kHead + "  - wait_until: { Body.level: 13 }\n"
                                                  "    timeout_ms: 30\n"));
    CHECK(failed.status == ScenarioStatus::Failed);
    CHECK(failed.steps[0].at == at(30ms));
}

TEST_CASE(timeline_of_a_run_that_did_not_get_to_its_end_says_how_far_it_is_recorded)
{
    const auto head = [](const std::string& text, const std::function<void(World&)>& setup) {
        const std::string timeline = timeline_of(text, 10ms, setup);
        std::string out = part(timeline, "    \"status\"", "  },");
        std::istringstream in(part(timeline, "  \"series\": [", "  \"lines\""));
        for (std::string line; std::getline(in, line);)
        {
            // (Those that have a value: every series has both, null where they do not apply.)
            if ((line.rfind("      \"not_applied_from\"", 0) == 0 || line.rfind("      \"last_ms\"", 0) == 0) &&
                !line.ends_with(": null,"))
            {
                out += line.substr(6) + "\n";
            }
        }
        std::istringstream steps(part(timeline, "  \"steps\": [", "  \"series\""));
        for (std::string line; std::getline(steps, line);)
        {
            if (line.rfind("      \"failed\"", 0) == 0)
            {
                out += line.substr(6) + "\n";
            }
        }
        return out;
    };
    const std::string refused = kHead + "  - set: { Body.level: 7 }\n"
                                        "  - at_ms: 40\n"
                                        "    set: { Body.level: 13 }\n"
                                        "  - expect: { Body.level: 13 }\n"
                                        "    within_ms: 100\n";
    // A scheduled stop leaves the scenario incomplete and records the samples before it.
    check_text(head(refused, [](World& w) { w.stop_at = at(30ms); }),
               "    \"status\": \"incomplete\",\n"
               "    \"over_at_ms\": 30,\n"
               "    \"recorded_to_ms\": 20,\n"
               "    \"end_ms\": null\n"
               "\"last_ms\": 20,\n"
               "\"failed\": null\n"
               "\"failed\": null\n"
               "\"failed\": null\n");
    // A participant failed after a write to it had failed: the scenario is incomplete and no step
    // has failed, but the record ends where it was closed, at the write, and its point is marked.
    check_text(head(refused, [](World& w) { w.superseded = true; }),
               "    \"status\": \"incomplete\",\n"
               "    \"over_at_ms\": null,\n"
               "    \"recorded_to_ms\": 40,\n"
               "    \"end_ms\": null\n"
               "\"not_applied_from\": 1,\n"
               "\"last_ms\": 40,\n"
               "\"failed\": null\n"
               "\"failed\": null\n"
               "\"failed\": null\n");
    // The simulation ended for another reason: recorded to the last sample that was looked at.
    check_text(head(kHead + "  - set: { Body.level: 7 }\n"
                            "  - wait_until: { Body.level: 8 }\n"
                            "    timeout_ms: 9000\n",
                    [](World& w) { w.ends_after = 300ms; }),
               "    \"status\": \"incomplete\",\n"
               "    \"over_at_ms\": null,\n"
               "    \"recorded_to_ms\": 290,\n"
               "    \"end_ms\": null\n"
               "\"last_ms\": 290,\n"
               "\"failed\": null\n"
               "\"failed\": null\n");
}

TEST_CASE(timeline_keeps_the_writes_of_the_times_before_a_motion_that_finds_no_base)
{
    // The controller steps every 30 ms, so the writes for 10 and for 25 ms go out together at
    // 0 ms, before any sample. The ramp at 25 ms finds no value to start from; the set of the
    // other variable at 10 ms was sent before that is said, and is in its series.
    const std::string text = kHead + "  - at_ms: 10\n"
                                     "    set: { Door.level: 3 }\n"
                                     "  - at_ms: 25\n"
                                     "    ramp: { Body.level: { to: 5, over_ms: 20 } }\n";
    const std::string made = timeline_of(text, 30ms);
    check_text(part(made, "    \"status\": ", "  },\n  \"variables\""),
               "    \"status\": \"failed\",\n    \"over_at_ms\": 0,\n    \"recorded_to_ms\": 25,\n    \"end_ms\": null\n");
    // (Sent, not applied: the simulation ended at 0 ms.) The ramp wrote nothing.
    check_text(part(made, "  \"series\": [", "  \"lines\""),
               "  \"series\": [\n"
               "    {\n      \"variable\": \"Door.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [10, 3]\n"
               "      ]\n"
               "    },\n"
               "    {\n      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\"],\n      \"samples\": null,\n      \"first_ms\": null,\n      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": []\n    }\n  ],\n");
    CHECK(made.find("      \"status\": \"unfinished\",\n      \"start_ms\": 10,") != std::string::npos);
}

TEST_CASE(timeline_lines_say_where_each_motion_starts_and_where_that_is_from)
{
    // What each step put on the line of a variable, with the base of a motion and where it is
    // from: the step ("written"), the line, or a sample.
    const auto lines_of = [](const std::string& timeline) {
        // "<kind> <variable> <from>-<to> base=<base> <source>" per line of the file's lines.
        std::istringstream in(part(timeline, "  \"lines\": [\n", "  \"findings\""));
        std::string out;
        const auto value = [](const std::string& line) {
            std::string text = line.substr(line.find(": ") + 2);
            std::erase(text, '"');
            std::erase(text, ',');
            return text;
        };
        for (std::string line; std::getline(in, line);)
        {
            for (const auto& [key, after] : std::vector<std::pair<std::string, std::string>>{
                     {"kind", " "}, {"variable", " "}, {"from_ms", "-"}, {"to_ms", " base="}, {"base", " "}, {"base_source", "\n"}})
            {
                if (line.rfind("      \"" + key + "\": ", 0) == 0)
                {
                    out += value(line) + after;
                }
            }
        }
        return out;
    };
    // Door.level is 0 in the model, and the scenario has not written it: the first ramp starts
    // from the sample. The ramp of Body.level starts from the set, the wave after it swings around
    // where the ramp ends, the lag names its start, and the square has its own values.
    const std::string made =
        timeline_of(kHead + "  - set: { Body.level: 20 }\n"
                            "  - at_ms: 30\n"
                            "    ramp: { Body.level: { to: 80, over_ms: 25 }, Door.level: { to: 4, over_ms: 20 } }\n"
                            "  - wave: { Body.level: { triangle: { amplitude: 5, period_ms: 40 }, for_ms: 40 },\n"
                            "            Door.ratio: { lag: { from: 0.25, to: 1, tau_ms: 20 }, for_ms: 40 },\n"
                            "            Door.lamp: { square: { period_ms: 20 }, for_ms: 40 } }\n",
                    10ms);
    // (The variable comes before the kind in the file.)
    check_text(lines_of(made), "Body.level set 0-0 base=20 written\n"
                               "Body.level ramp 30-55 base=20 line\n"
                               "Door.level ramp 30-50 base=0 sample\n"
                               "Body.level wave 55-95 base=80 line\n"
                               "Door.ratio wave 55-95 base=0.25 written\n"
                               "Door.lamp wave 55-95 base=null null\n");
    CHECK(made.find("      \"shape\": {\n        \"type\": \"lag\",\n        \"to\": 1,\n        \"tau_ms\": 20,\n"
                    "        \"for_ms\": 40\n      },\n"
                    "      \"text\": \"Door.ratio lag from 0.25 to 1 tau 20 ms for 40 ms\"") != std::string::npos);
    CHECK(made.find("      \"lines\": [1, 2],\n") != std::string::npos && made.find("      \"lines\": [3, 4, 5],\n") != std::string::npos);

    // The file has what happened: a line of which no write was sent is not in it, and not in the
    // writes of its step. A set that its agent refuses at once (sent: its point is in the series,
    // marked), and at the same time a ramp: the writes of a tick go out before the engine looks at
    // their results, so the ramp's first write went out too, and its line is in the file, though
    // its step is not run.
    ScenarioCatalog loose = catalog();
    loose[0].variables[0].max.reset();
    const std::string refused = timeline_of(kHead + "  - at_ms: 40\n"
                                                    "    set: { Body.level: 105 }\n"
                                                    "  - ramp: { Body.level: { to: 50, over_ms: 20 } }\n",
                                            10ms, {}, loose);
    CHECK(refused.find("    \"status\": \"failed\",") != std::string::npos);
    check_text(lines_of(refused), "Body.level set 40-40 base=105 written\n"
                                  "Body.level ramp 40-60 base=105 line\n");
    const auto step_lines_of = [](const std::string& timeline) {
        std::istringstream in(timeline);
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"lines\": ", 0) == 0)
            {
                out += line.substr(15) + " ";
            }
        }
        return out;
    };
    check_text(step_lines_of(refused), "[0], [1], ");
    // One step moves three variables. The second finds no base (the scenario has not written it
    // and no sample is there at 0 ms): the first was sent and is a line, the second and the third
    // (which names its base) were not and are not. Whatever the controller's step.
    for (const SimDuration tick : {SimDuration{5ms}, SimDuration{10ms}, SimDuration{30ms}})
    {
        const std::string part_sent = timeline_of(kHead + "  - set: { Door.lamp: true }\n"
                                                          "  - ramp: { Door.ratio: { from: 0, to: 1, over_ms: 20 }, Body.level: { to: 5, over_ms: 20 },\n"
                                                          "            Door.level: { from: 0, to: 9, over_ms: 20 } }\n"
                                                          "  - set: { Body.level: 1 }\n",
                                                tick);
        check_text(lines_of(part_sent), "Door.lamp set 0-0 base=true written\n"
                                        "Door.ratio ramp 0-20 base=0 written\n",
                   "the lines of a step that sent a part of its writes");
        check_text(step_lines_of(part_sent), "[0], [1], [], ");
        CHECK(part_sent.find("      \"variable\": \"Door.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
                             "      \"columns\": [\"at_ms\", \"value\"],\n      \"samples\": null,\n      \"first_ms\": null,\n      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": []\n") != std::string::npos);
    }
    // A run that never began has no lines, and says so with an empty list.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n");
    ScenarioRun run = scenario_not_run(s);
    TimelineInput input;
    input.file = "simulation.yaml";
    input.scenario = &s;
    input.run = &run;
    const std::string none = format_timeline_json(input);
    CHECK(none.find("\n  \"lines\": [],\n") != std::string::npos && none.find("      \"lines\": [],\n") != std::string::npos);
}

TEST_CASE(timeline_line_after_a_motion_that_returns_to_a_sample_starts_from_that_sample_too)
{
    // Door's input is 20, and Door writes 60 to it in its step at 30 ms. The wave at 45 ms names no middle and
    // the scenario has not written the variable: it swings around a sample, and returns to it.
    // The ramp after it starts from the line, which is that sample still. A controller that steps
    // every 10 ms sends the writes for 45 ms at 40 ms and has looked at the sample of 30 ms (60);
    // one that steps every 30 ms sends them at 30 ms and has looked at the one of 20 ms (20).
    const std::string text = kHead + "  - at_ms: 45\n"
                                     "    wave: { Door.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 20 } }\n"
                                     "  - ramp: { Door.level: { to: 10, over_ms: 20 } }\n";
    const auto bases = [&text](SimDuration tick) {
        const std::string made = timeline_of(text, tick, [](World& w) {
            w.ecus[1].level = 20;
            w.ecus[1].last_level = 20;
            w.ecus[1].own_level = std::pair{at(30ms), std::int64_t{60}};
        });
        std::istringstream in(part(made, "  \"lines\": [\n", "  \"findings\""));
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"base", 0) == 0)
            {
                out += line.substr(6) + " ";
            }
        }
        return out;
    };
    check_text(bases(10ms), "\"base\": 60, \"base_source\": \"sample\", \"base\": 60, \"base_source\": \"line\", ");
    check_text(bases(30ms), "\"base\": 20, \"base_source\": \"sample\", \"base\": 20, \"base_source\": \"line\", ");
}

TEST_CASE(scenario_dither_write_that_is_refused_fails_the_first_step_that_is_not_decided)
{
    // 99 with a square of 2 that is low for 50 ms and then high: the write of 50 ms is 101, which
    // Body refuses when it gets it. It is no step's write. The set passed long before; the step
    // that fails is the first that is not decided when the launcher sends the write, which
    // depends on the controller's step: every 10 ms the expect of 30 ms has passed, every 30 ms
    // it has not (the write of 50 ms goes out at 30 ms, with those up to 60 ms).
    const std::string head = kDitherHead + "inputs:\n"
                                           "  Body.level: { dither: { square: { amplitude: 2, period_ms: 100, phase_deg: 180 } } }\n"
                                           "steps:\n"
                                           "  - set: { Body.level: 99 }\n"
                                           "  - at_ms: 30\n"
                                           "    expect: { Body.level: 97 }\n";
    const Scenario two = read(head + "  - at_ms: 60\n    expect: { Body.level: 97 }\n");
    const std::string message = "scenario-write: the dither of Body.level (line 3) wrote 101 at 50 ms, above its range 0 to 100 (level: "
                                "out of range): lower its amplitude, or keep the value further from the end of the range\n";
    {
        World world;
        world.tick = 10ms;
        check_text(text_of(world.run(two)), "failed over=40\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 passed start=30 at=30 end=30\n"
                                            "8 failed start=60 at=50 end=60 " + message);
    }
    {
        World world;
        world.tick = 30ms;
        check_text(text_of(world.run(two)), "failed over=30\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 failed start=30 at=50 end=30 " + message +
                                            "8 not_run start=- at=- end=-\n");
    }
    // The same in the last step of the controller before the scenario is over: with a step of
    // 10 ms the scenario has passed before the write of 50 ms is sent, with 30 ms it fails.
    const Scenario one = read(head);
    {
        World world;
        world.tick = 10ms;
        check_text(text_of(world.run(one)), "passed over=40\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 passed start=30 at=30 end=30\n");
    }
    {
        World world;
        world.tick = 30ms;
        check_text(text_of(world.run(one)), "failed over=30\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 failed start=30 at=50 end=30 " + message);
    }
    // Rejected by the ECU in its step (the setter of Body.level throws at 13): known when the
    // result is judged. The dithers of one time for one participant are one command, which the
    // ECU applies one write after the other until a setter throws: the message is about the
    // input the reason names, and says that the input before it in the command was written.
    const Scenario rejected = read(kDitherHead + "inputs:\n"
                                                 "  Body.ratio: { dither: { square: { amplitude: 0.1, period_ms: 20 } } }\n"
                                                 "  Body.level: { dither: { square: { amplitude: 1, period_ms: 40, phase_deg: 180 } } }\n"
                                                 "  Door.level: { dither: { square: { amplitude: 1, period_ms: 40 } } }\n"
                                                 "steps:\n"
                                                 "  - set: { Body.level: 12, Body.ratio: 0.5, Door.level: 5 }\n"
                                                 "  - at_ms: 100\n"
                                                 "    expect: { Body.level: 11 }\n");
    World world;
    check_text(text_of(world.run(rejected)), "failed over=30\n"
                                             "7 passed start=0 at=0 end=0\n"
                                             "8 failed start=100 at=20 end=100 scenario-write: the dither of Body.level (line 4) wrote 13 at 20 ms, "
                                             "and participant 'Body' did not apply the write: the ECU rejected it (level: a setter threw: 13 is not "
                                             "a level); the dither of 1 other input of that participant was in the same command; the ECU had "
                                             "already applied the write before it in that command (Body.ratio = 0.6), and it stays applied\n");
    // The model ECU has the ratio of that command (as a real one has: Agent::apply()), and not the level.
    check_text(written_to(world, "Body.ratio", 20ms), "0.6@0>0 0.4@10>10 0.6@20>20 ");
    check_text(written_to(world, "Body.level", 20ms), "11@0>0 11@10>10 ");
    check_text(written_to(world, "Door.level", 20ms), "6@0>0 6@10>10 4@20>20 ");
    // The timeline says the same: of the command's writes the one before the rejected one was
    // applied and has no mark; the rejected one is the reason and is marked. The write of the
    // other participant, sent for the same time, was applied.
    const std::string file = timeline_of(kDitherHead + "inputs:\n"
                                                       "  Body.ratio: { dither: { square: { amplitude: 0.1, period_ms: 20 } } }\n"
                                                       "  Body.level: { dither: { square: { amplitude: 1, period_ms: 40, phase_deg: 180 } } }\n"
                                                       "  Door.level: { dither: { square: { amplitude: 1, period_ms: 40 } } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.level: 12, Body.ratio: 0.5, Door.level: 5 }\n"
                                                       "  - at_ms: 100\n"
                                                       "    expect: { Body.level: 11 }\n",
                                         10ms);
    check_text(part(file, "        \"writes\": [", "\n      }\n"), "        \"writes\": [\n"
               "          {\n"
               "            \"variable\": \"Body.ratio\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 0.6,\n"
               "            \"point\": 2,\n"
               "            \"applied\": true\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Body.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 13,\n"
               "            \"point\": 2,\n"
               "            \"applied\": false\n"
               "          },\n"
               "          {\n"
               "            \"variable\": \"Door.level\",\n"
               "            \"at_ms\": 20,\n"
               "            \"value\": 4,\n"
               "            \"point\": 2,\n"
               "            \"applied\": true\n"
               "          }\n"
               "        ],\n"
               "        \"write\": 1");
    const auto series = [&file](const std::string& variable) {
        return part(file, "      \"variable\": \"" + variable + "\",\n      \"source\": \"written\",\n", "      \"points\"");
    };
    CHECK(series("Body.ratio").find("      \"not_applied_from\": null,\n") != std::string::npos);
    CHECK(series("Door.level").find("      \"not_applied_from\": null,\n") != std::string::npos);
    CHECK(series("Body.level").find("      \"not_applied_from\": 2,\n") != std::string::npos);
}

TEST_CASE(timeline_has_the_dithers_as_lines_and_the_base_of_every_point_they_were_on)
{
    const auto lines_of = [](const std::string& timeline) {
        // "<variable> <kind> line <line> step <step> <from>-<to> <text>" per line of the file's lines.
        std::istringstream in(part(timeline, "  \"lines\": [\n", "  \"findings\""));
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            for (const auto& [key, after] : std::vector<std::pair<std::string, std::string>>{
                     {"variable", " "}, {"kind", " line "}, {"line", " step "}, {"step", " "}, {"from_ms", "-"}, {"to_ms", " "}, {"text", "\n"}})
            {
                if (line.rfind("      \"" + key + "\": ", 0) == 0)
                {
                    std::string text = line.substr(line.find(": ") + 2);
                    std::erase(text, '"');
                    std::erase(text, ',');
                    out += text + after;
                }
            }
        }
        return out;
    };
    const auto step_lines_of = [](const std::string& timeline) {
        std::istringstream in(timeline);
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"lines\": ", 0) == 0)
            {
                out += line.substr(15) + " ";
            }
        }
        return out;
    };
    // The declared dither of Body.level is a line from the first write that carries it (the set
    // at 30 ms), without a step and without an end. The dither step at 20 ms puts one on
    // Body.ratio, which has no value: its line begins with the set at 60 ms, and is of that step.
    // none at 90 ms is a line too (no shape): where the dither before it ends. The dither of
    // Door.level never carried anything, and the one that the step at 20 ms put on Door.ratio is
    // replaced at 60 ms before a write: neither is a line.
    const std::string text = kDitherHead + "inputs:\n"
                                           "  Body.level: { dither: { triangle: { amplitude: 2, frequency_hz: 25 } } }\n"
                                           "  Door.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                           "params: { swing: 0.05, pick: 12345678901234567 }\n"
                                           "steps:\n"
                                           "  - at_ms: 20\n"
                                           "    dither: { Body.ratio: { noise: { amplitude: \"${swing}\", hold_ms: 10, seed: \"${pick}\" } },\n"
                                           "              Door.ratio: { square: { amplitude: 0.1, period_ms: 20 } } }\n"
                                           "  - at_ms: 30\n"
                                           "    set: { Body.level: 50 }\n"
                                           "  - at_ms: 60\n"
                                           "    dither: { Door.ratio: { sawtooth: { amplitude: 0.1, period_ms: 20, falling: true } } }\n"
                                           "  - at_ms: 60\n"
                                           "    set: { Body.ratio: 0.5, Door.ratio: 0.5 }\n"
                                           "  - at_ms: 90\n"
                                           "    dither: { Body.level: none }\n"
                                           "  - at_ms: 110\n"
                                           "    expect: { Body.level: 50 }\n";
    const std::string made = timeline_of(text, 10ms);
    CHECK(made.find("    \"status\": \"passed\",") != std::string::npos);
    // (After the lines that the steps put at a time come the dithers that begin at it, in the
    // order of their first writes: the step at 60 ms that puts Door.ratio's is before the set.)
    check_text(lines_of(made), "Body.level set line 10 step 2 30-30 Body.level = 50\n"
                               "Body.level dither line 3 step null 30-null Body.level triangle amplitude 2 frequency 25 Hz\n"
                               "Body.ratio set line 14 step 4 60-60 Body.ratio = 0.5\n"
                               "Door.ratio set line 14 step 4 60-60 Door.ratio = 0.5\n"
                               "Door.ratio dither line 12 step 3 60-null Door.ratio sawtooth amplitude 0.1 period 20 ms falling\n"
                               "Body.ratio dither line 7 step 1 60-null Body.ratio noise amplitude 0.05 hold 10 ms seed 12345678901234567\n"
                               "Body.level dither line 16 step 5 90-null Body.level none\n");
    // The steps' writes are the places of their lines; a dither step's are its dithers' lines.
    check_text(step_lines_of(made), "[5], [0], [4], [2, 3], [6], [], ");
    // A dither line has no base; its shape is what the text has, with the parameters put in (a
    // seed keeps every digit); none has no shape.
    CHECK(made.find("      \"kind\": \"dither\",\n      \"line\": 7,\n      \"step\": 1,\n      \"from_ms\": 60,\n      \"to_ms\": null,\n"
                    "      \"base\": null,\n      \"base_source\": null,\n      \"shape\": {\n        \"type\": \"noise\",\n"
                    "        \"amplitude\": 0.05,\n        \"hold_ms\": 10,\n        \"seed\": 12345678901234567\n      },\n") != std::string::npos);
    CHECK(made.find("      \"shape\": {\n        \"type\": \"triangle\",\n        \"amplitude\": 2,\n        \"frequency_hz\": 25\n      },\n") !=
          std::string::npos);
    CHECK(made.find("      \"shape\": {\n        \"type\": \"sawtooth\",\n        \"amplitude\": 0.1,\n        \"period_ms\": 20,\n"
                    "        \"falling\": true\n      },\n") != std::string::npos);
    CHECK(made.find("      \"to_ms\": null,\n      \"base\": null,\n      \"base_source\": null,\n      \"shape\": null,\n"
                    "      \"text\": \"Body.level none\"\n") != std::string::npos);
    // The series of an input that a dither was on has a third column: what each value was made
    // of, before the dither and the rounding. Also for the points after the dither was taken off.
    check_text(part(made, "      \"variable\": \"Body.level\",\n      \"source\": \"written\"", "    },\n"),
               "      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
               "      \"columns\": [\"at_ms\", \"value\", \"base\"],\n"
               "      \"samples\": null,\n"
               "      \"first_ms\": null,\n"
               "      \"last_ms\": null,\n"
               "      \"not_applied_from\": null,\n"
               "      \"points\": [\n"
               "        [30, 48, 50],\n        [40, 50, 50],\n        [50, 52, 50],\n        [60, 50, 50],\n        [70, 48, 50],\n"
               "        [80, 50, 50],\n        [90, 50, 50]\n      ]\n");
    // A file without a dither is as it was: two columns and no "columns".
    const std::string plain = timeline_of(kHead + "  - set: { Body.level: 50 }\n", 10ms);
    CHECK(plain.find("      \"columns\": [\"at_ms\", \"value\"],\n") != std::string::npos && plain.find("        [0, 50]\n") != std::string::npos);
    // A dither whose first write is refused was sent: it is a line, and the point is marked.
    const std::string refused = timeline_of(kDitherHead + "inputs:\n  Body.level: { dither: { square: { amplitude: 2, period_ms: 40 } } }\n"
                                                          "steps:\n  - at_ms: 20\n    set: { Body.level: 99 }\n"
                                                          "  - at_ms: 60\n    expect: { Body.level: 99 }\n",
                                            10ms);
    check_text(lines_of(refused), "Body.level set line 5 step 1 20-20 Body.level = 99\n"
                                  "Body.level dither line 3 step null 20-null Body.level square amplitude 2 period 40 ms\n");
    CHECK(refused.find("      \"not_applied_from\": 2,\n") != std::string::npos);
    check_text(part(refused, "      \"points\": [\n", "      ]\n"), "      \"points\": [\n        [20, 97, 99],\n        [30, 97, 99],\n        [40, 101, 99]\n");
    // The step that failed is the first that was not decided, with the dither's write as what failed.
    CHECK(refused.find("        \"writes\": [\n          {\n            \"variable\": \"Body.level\",\n            \"at_ms\": 40,\n"
                       "            \"value\": 101,\n            \"point\": 2,\n            \"applied\": false\n          }\n        ],\n"
                       "        \"write\": 0\n") != std::string::npos);
}

TEST_CASE(timeline_line_has_the_base_as_the_step_names_it_and_the_shape_with_the_parameters_put_in)
{
    // The base a step names is written as it is: 10.5 for an integer (the line and the sample
    // give what the variable's type has), and a bool as a bool. The shape has the values of the
    // text with the parameters put in: a number as a number, a seed with every digit, a word.
    const std::string made =
        timeline_of(kDitherHead + "params: { hz: 12.5, pick: 9007199254740993, bell: gaussian, hold: 20 }\n"
                                  "steps:\n"
                                  "  - ramp: { Body.level: { from: 10.5, to: 20, over_ms: 20 } }\n"
                                  "  - wave: { Body.lamp: { chatter: { to: true, interval_ms: 10, bounces: 1 } },\n"
                                  "            Body.ratio: { sine: { amplitude: 0.1, frequency_hz: \"${hz}\" }, around: 0.5, for_ms: 20 },\n"
                                  "            Body.level: { noise: { amplitude: 1, hold_ms: \"${hold}\", seed: \"${pick}\", distribution: \"${bell}\" }, for_ms: 20 } }\n",
                    10ms);
    CHECK(made.find("    \"status\": \"passed\",") != std::string::npos);
    CHECK(made.find("      \"kind\": \"ramp\",\n      \"line\": 4,\n      \"step\": 1,\n      \"from_ms\": 0,\n      \"to_ms\": 20,\n"
                    "      \"base\": 10.5,\n      \"base_source\": \"written\",\n") != std::string::npos);
    CHECK(made.find("      \"base\": false,\n      \"base_source\": \"written\",\n      \"shape\": {\n        \"type\": \"chatter\",\n") !=
          std::string::npos);
    CHECK(made.find("      \"base\": 20,\n      \"base_source\": \"line\",\n      \"shape\": {\n        \"type\": \"noise\",\n"
                    "        \"amplitude\": 1,\n        \"hold_ms\": 20,\n        \"seed\": 9007199254740993,\n"
                    "        \"distribution\": \"gaussian\",\n        \"for_ms\": 20\n      },\n") != std::string::npos);
    CHECK(made.find("        \"type\": \"sine\",\n        \"amplitude\": 0.1,\n        \"frequency_hz\": 12.5,\n        \"for_ms\": 20\n") !=
          std::string::npos);
}

TEST_CASE(scenario_dither_noise_is_the_same_in_every_run_and_differs_by_seed_and_by_variable)
{
    const auto text = [](const std::string& seedBody, const std::string& seedDoor) {
        return kDitherHead + "inputs:\n"
                             "  Body.ratio: { dither: { noise: { amplitude: 0.1, hold_ms: 10, seed: " + seedBody + " } } }\n"
                             "  Door.ratio: { dither: { noise: { amplitude: 0.1, hold_ms: 10, seed: " + seedDoor + " } } }\n"
                             "steps:\n"
                             "  - set: { Body.ratio: 0.5, Door.ratio: 0.5 }\n"
                             "  - at_ms: 200\n"
                             "    expect: { Body.ratio: { ge: 0.4, le: 0.6 }, Door.ratio: { ge: 0.4, le: 0.6 } }\n";
    };
    const auto values = [](const World& world, const std::string& variable) {
        // The values alone: "0.53 0.47 ...".
        std::string out;
        const std::string writes = written_to(world, variable, 200ms);
        for (std::size_t from = 0; from < writes.size(); from = writes.find(' ', from) + 1)
        {
            out += writes.substr(from, writes.find('@', from) - from) + " ";
        }
        return out;
    };
    // Two runs of one scenario write the same file, whatever the controller's step is.
    const std::string first = timeline_of(text("7", "7"), 10ms);
    CHECK(first.find("    \"status\": \"passed\",") != std::string::npos);
    check_text(timeline_of(text("7", "7"), 10ms), first, "the same scenario again");
    check_text(compared(timeline_of(text("7", "7"), 30ms)), compared(first), "with another step of the controller");
    // The name of the variable is part of the seed: one seed for two inputs is two sequences.
    World same;
    same.run(read(text("7", "7")));
    const std::string body = values(same, "Body.ratio");
    CHECK(std::count(body.begin(), body.end(), ' ') == 21 && body != values(same, "Door.ratio"));
    // Another seed is another sequence, and the input with the seed it had keeps its own.
    World other;
    other.run(read(text("8", "7")));
    CHECK(values(other, "Body.ratio") != body && values(other, "Door.ratio") == values(same, "Door.ratio"));
}

TEST_CASE(timeline_columns_of_a_dithered_input_do_not_depend_on_how_far_the_run_got)
{
    // A scenario that fails at a sample (the expect of 30 ms) before the dither step of 50 ms.
    // With a controller step of 30 ms the write of 50 ms went out before the failure was seen,
    // with 10 ms it did not; the file is the same. The series has its base column because the
    // scenario puts a dither on the input at all, not because a write carried one. (Compared:
    // the series and the lines. The status of the steps after the one that failed depends on the
    // controller's step, as in every run that fails.)
    const auto recorded = [](const std::string& timeline) { return part(timeline, "  \"series\": [", "  \"findings\""); };
    const std::string failing = kHead + "  - set: { Body.level: 50 }\n"
                                        "  - at_ms: 30\n"
                                        "    expect: { Body.level: 51 }\n"
                                        "  - at_ms: 50\n"
                                        "    dither: { Body.level: { square: { amplitude: 2, period_ms: 40 } } }\n";
    const std::string fine = timeline_of(failing, 10ms);
    CHECK(fine.find("    \"status\": \"failed\",") != std::string::npos);
    check_text(recorded(timeline_of(failing, 30ms)), recorded(fine), "with a controller step of 30 ms");
    CHECK(fine.find("      \"columns\": [\"at_ms\", \"value\", \"base\"],\n      \"samples\": null,\n      \"first_ms\": null,\n"
                    "      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": [\n        [0, 50, 50]\n      ]\n") !=
          std::string::npos);
    // The same for a declared dither whose input gets its first value after the failure.
    const std::string declared = kDitherHead + "inputs:\n  Body.level: { dither: { square: { amplitude: 2, period_ms: 40 } } }\n"
                                               "steps:\n  - at_ms: 30\n    expect: { Body.level: 51 }\n"
                                               "  - at_ms: 50\n    set: { Body.level: 50 }\n";
    check_text(recorded(timeline_of(declared, 30ms)), recorded(timeline_of(declared, 10ms)), "a declared dither");
    // And where the run decides whether the dither step is reached: after a step that waits.
    const std::string waiting = kHead + "  - set: { Body.level: 50 }\n"
                                        "  - wait_until: { Body.level: 51 }\n"
                                        "    timeout_ms: 40\n"
                                        "  - dither: { Body.level: { square: { amplitude: 2, period_ms: 40 } } }\n";
    const std::string timedOut = timeline_of(waiting, 10ms);
    check_text(recorded(timeline_of(waiting, 30ms)), recorded(timedOut), "a dither step after a wait that runs out");
    CHECK(timedOut.find("      \"columns\": [\"at_ms\", \"value\", \"base\"],\n") != std::string::npos);
}

TEST_CASE(timeline_json_omits_empty_provenance_and_emits_non_empty_provenance)
{
    Scenario s = read(kHead + "  - name: step one\n    set: { Body.level: 10 }\n");
    s.steps[0].file = "scenarios/templates/init.yaml";
    s.steps[0].line = 9;
    s.steps[0].call_stack.push_back(SourceFrame{
        "scenario.yaml",
        4,
        "call init",
        {SourceParam{"lvl", "10", false, ParamSource::TemplateCall},
         SourceParam{"label", "hi", true, ParamSource::TemplateCall}},
    });
    World world;
    world.tick = 10ms;
    TimelineRecord record(s);
    world.record = &record;
    ScenarioRun run = world.run(s);
    run.file = "scenario.yaml";

    TimelineInput input;
    input.file = "simulation.yaml";
    input.time_step_ms = 10.0;
    input.scenario = &s;
    input.run = &run;
    const auto desc = catalog();
    input.catalog = &desc;
    input.record = &record;

    const std::string tl = format_timeline_json(input);
    CHECK(tl.find("\"file\": \"scenarios/templates/init.yaml\"") != std::string::npos);
    CHECK(tl.find("\"name\": \"step one\"") != std::string::npos);
    CHECK(tl.find("\"call_stack\": [") != std::string::npos);
    CHECK(tl.find("\"lvl\": 10") != std::string::npos);
    CHECK(tl.find("\"label\": \"hi\"") != std::string::npos);
    // lines[] also carries file when non-empty.
    const auto linesPos = tl.find("  \"lines\": [");
    CHECK(linesPos != std::string::npos);
    CHECK(tl.substr(linesPos).find("\"file\": \"scenarios/templates/init.yaml\"") != std::string::npos);
}

TEST_CASE(timeline_json_records_real_expanded_template_provenance)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "template-timeline";
    fs::remove_all(dir);
    fs::create_directories(dir / "templates");
    std::ofstream(dir / "templates" / "ramp.yaml", std::ios::binary)
        << "vehicle_sil_scenario_template: 1\n"
        << "parameters:\n"
        << "  to_val: { type: integer }\n"
        << "steps:\n"
        << "  - name: ramp level\n"
        << "    ramp: { Body.level: { to: \"${to_val}\", over_ms: 20 } }\n";
    const std::string scenarioText =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - name: call ramp\n"
        "    template: templates/ramp.yaml\n"
        "    params: { to_val: 40 }\n";
    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/s.yaml";
    ctx.read_template = files_beside(dir / "s.yaml", "the template file");
    ScenarioCatalog cat = catalog();
    cat[1].absent.push_back("absent");
    const ScenarioRead read = read_scenario(scenarioText, cat, ctx);
    if (!read.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(read.findings));
    }

    World world;
    world.tick = 10ms;
    TimelineRecord record(*read.scenario);
    world.record = &record;
    ScenarioRun run = world.run(*read.scenario);
    run.file = "scenarios/s.yaml";

    TimelineInput input;
    input.file = "simulation.yaml";
    input.time_step_ms = 10.0;
    input.scenario = &*read.scenario;
    input.run = &run;
    const auto desc = catalog();
    input.catalog = &desc;
    input.record = &record;

    const std::string tl = format_timeline_json(input);
    CHECK(tl.find("\"file\": \"scenarios/templates/ramp.yaml\"") != std::string::npos);
    CHECK(tl.find("\"name\": \"ramp level\"") != std::string::npos);
    CHECK(tl.find("\"call_stack\": [") != std::string::npos);
    CHECK(tl.find("\"to_val\": 40") != std::string::npos);
    CHECK(tl.find("\"case\":") == std::string::npos);

    run.case_name = "定格";
    const std::string tlCased = format_timeline_json(input);
    CHECK(tlCased.find("\"case\": \"定格\"") != std::string::npos);
}

TEST_CASE(timeline_json_records_nested_template_fail_grid_skipped_and_case)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "nested-fail-grid-timeline";
    fs::remove_all(dir);
    fs::create_directories(dir / "templates");
    std::ofstream(dir / "templates" / "inner.yaml", std::ios::binary)
        << "vehicle_sil_scenario_template: 1\n"
        << "parameters:\n"
        << "  lvl: { type: integer }\n"
        << "steps:\n"
        << "  - name: skipped door step\n"
        << "    set: { Door.absent: 10 }\n"
        << "  - name: dither body\n"
        << "    dither: { Body.level: { square: { amplitude: 5, period_ms: 40 } } }\n"
        << "  - name: set near edge\n"
        << "    set: { Body.level: \"${lvl}\" }\n"
        << "  - at_ms: 10\n"
        << "    expect: { Body.level: { ge: 0 } }\n";
    std::ofstream(dir / "templates" / "outer.yaml", std::ios::binary)
        << "vehicle_sil_scenario_template: 1\n"
        << "parameters:\n"
        << "  target_lvl: { type: integer }\n"
        << "steps:\n"
        << "  - name: call inner\n"
        << "    template: inner.yaml\n"
        << "    params: { lvl: \"${target_lvl}\" }\n";
    const std::string scenarioText =
        "vehicle_sil_scenario: 1\n"
        "params: { target_lvl: 50 }\n"
        "cases:\n"
        "  c_grid: { params: { target_lvl: 99 } }\n"
        "steps:\n"
        "  - name: call outer\n"
        "    template: templates/outer.yaml\n"
        "    params: { target_lvl: \"${target_lvl}\" }\n";

    ScenarioContext ctx;
    ctx.variant = "small";
    ctx.case_name = "c_grid";
    ctx.scenario_file = "scenarios/s.yaml";
    ctx.read_template = files_beside(dir / "s.yaml", "the template file");
    ScenarioCatalog cat = catalog();
    cat[1].absent.push_back("absent");
    const ScenarioRead read = read_scenario(scenarioText, cat, ctx);
    if (!read.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(read.findings));
    }

    World world;
    world.tick = 10ms;
    TimelineRecord record(*read.scenario);
    world.record = &record;
    ScenarioRun run = world.run(*read.scenario);
    run.file = "scenarios/s.yaml";
    CHECK(run.status == ScenarioStatus::Failed);

    TimelineInput input;
    input.file = "simulation.yaml";
    input.time_step_ms = 10.0;
    input.scenario = &*read.scenario;
    input.run = &run;
    const auto desc = catalog();
    input.catalog = &desc;
    input.record = &record;

    const std::string tl = format_timeline_json(input);
    CHECK(tl.find("\"case\": \"c_grid\"") != std::string::npos);
    CHECK(tl.find("\"status\": \"skipped\"") != std::string::npos);
    CHECK(tl.find("\"file\": \"scenarios/templates/inner.yaml\"") != std::string::npos);
    CHECK(tl.find("\"file\": \"scenarios/templates/outer.yaml\"") != std::string::npos);
    CHECK(tl.find("\"not_applied_from\":") != std::string::npos);
}

TEST_CASE(timeline_json_records_template_dither_cancel_and_nested_every_step_series)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "template-dither-cancel-and-every-step";
    fs::remove_all(dir);
    fs::create_directories(dir / "templates");
    std::ofstream(dir / "templates" / "cancel_inner.yaml", std::ios::binary)
        << "vehicle_sil_scenario_template: 1\n"
        << "steps:\n"
        << "  - name: turn off dither\n"
        << "    after_ms: 20\n"
        << "    dither: { Body.level: none }\n"
        << "  - name: check speed\n"
        << "    after_ms: 10\n"
        << "    expect: { Body.body.speed: { ge: 0 } }\n";
    std::ofstream(dir / "templates" / "cancel_outer.yaml", std::ios::binary)
        << "vehicle_sil_scenario_template: 1\n"
        << "steps:\n"
        << "  - name: invoke inner\n"
        << "    template: cancel_inner.yaml\n";

    const std::string scenarioText =
        "vehicle_sil_scenario: 1\n"
        "samples: every_step\n"
        "inputs:\n"
        "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    set: { Body.level: 40 }\n"
        "  - name: invoke outer\n"
        "    template: templates/cancel_outer.yaml\n";

    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/main.yaml";
    ctx.read_template = files_beside(dir / "main.yaml", "the template file");
    const ScenarioRead read = read_scenario(scenarioText, catalog(), ctx);
    if (!read.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(read.findings));
    }

    World world;
    world.tick = 10ms;
    TimelineRecord record(*read.scenario);
    world.record = &record;
    ScenarioRun run = world.run(*read.scenario);
    run.file = "scenarios/main.yaml";
    CHECK(run.status == ScenarioStatus::Passed);

    TimelineInput input;
    input.file = "simulation.yaml";
    input.time_step_ms = 10.0;
    input.scenario = &*read.scenario;
    input.run = &run;
    const auto desc = catalog();
    input.catalog = &desc;
    input.record = &record;

    const std::string tl = format_timeline_json(input);
    // Dither cancel line carries template file attribution.
    CHECK(tl.find("\"kind\": \"dither\"") != std::string::npos);
    CHECK(tl.find("\"text\": \"Body.level none\"") != std::string::npos);
    CHECK(tl.find("\"file\": \"scenarios/templates/cancel_inner.yaml\"") != std::string::npos);
    // Nested 2-level call_stack (main.yaml -> cancel_outer.yaml) is preserved on inner template steps.
    CHECK(tl.find("\"file\": \"scenarios/main.yaml\"") != std::string::npos);
    CHECK(tl.find("\"file\": \"scenarios/templates/cancel_outer.yaml\"") != std::string::npos);
    CHECK(tl.find("\"name\": \"invoke outer\"") != std::string::npos);
    CHECK(tl.find("\"name\": \"invoke inner\"") != std::string::npos);
    // samples: every_step records sampled series points for Body.body.speed.
    CHECK(tl.find("\"variable\": \"Body.body.speed\"") != std::string::npos);
    CHECK(tl.find("\"source\": \"sampled\"") != std::string::npos);
}

TEST_CASE(timeline_json_records_variable_to_variable_watches_and_failures)
{
    // 1. Passing and failing variable-to-variable scenarios (including driven + dithered RHS):
    // watch.conditions[].compare[] has `"value": null` + `"variable": "Door.level"`,
    // failed.conditions[] has `"compared": [{ "variable", "value", "sample_ms" }]`,
    // RHS has `"watched": true` and a `"sampled"` series (plus `"written"` when driven),
    // and the timeline is independent of controller tick and delivery policy.
    const std::string text =
        "vehicle_sil_scenario: 1\n"
        "inputs:\n"
        "  Door.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    set: { Body.level: 20, Door.level: 20 }\n"
        "  - at_ms: 20\n"
        "    expect: { Body.level: { eq: { variable: Door.level }, le: 100 } }\n"
        "  - at_ms: 30\n"
        "    expect: { Body.level: { eq: { variable: Door.level } } }\n";
    const std::string first = compared(timeline_of(text, 10ms));
    // At t = 20 ms triangle dither (period 40 ms) is 0 -> Door.level = 20 (step 2 passes);
    // at t = 30 ms triangle dither is -2 -> Door.level = 18 != 20 (step 3 fails)!
    CHECK(first.find("\"status\": \"failed\"") != std::string::npos);
    CHECK(first.find("\"value\": null,\n                \"variable\": \"Door.level\"") != std::string::npos);
    CHECK(first.find("\"compared\": [\n              {\n                \"variable\": \"Door.level\",\n                \"value\": 18,\n"
                     "                \"sample_ms\": 30\n              }\n            ]") != std::string::npos);
    // Door.level has both written and sampled series, and watched: true!
    CHECK(first.find("\"name\": \"Door.level\"") != std::string::npos);
    CHECK(first.find("\"variable\": \"Door.level\",\n      \"source\": \"written\"") != std::string::npos);
    CHECK(first.find("\"variable\": \"Door.level\",\n      \"source\": \"sampled\"") != std::string::npos);
    for (const SimDuration tick : {SimDuration{20ms}, SimDuration{5ms}, SimDuration{15ms}})
    {
        check_text(compared(timeline_of(text, tick)), first, "variable-to-variable timeline with another tick");
    }
    for (const auto& [name, policy] : policies())
    {
        check_text(compared(timeline_of(text, 10ms, [&](World& w) { w.policy = policy; })), first, name.c_str());
    }

    // 2. During unseen RHS in timeline JSON: LHS has actual value & sample_ms, while compared[] has null value & sample_ms.
    const std::string unseenRhs =
        "vehicle_sil_scenario: 1\n"
        "samples: as_sent\n"
        "steps:\n"
        "  - at_ms: 20\n"
        "    ramp: { Body.level: { from: 10, to: 20, over_ms: 20 } }\n"
        "    expect: { Body.level: { ge: { variable: Door.level } } }\n";
    const std::string tlUnseen = timeline_of(
        unseenRhs, 10ms, [](World& w) {
            w.ecus[1].step = 100ms;
            w.ecus[1].level = 5;
        },
        catalog(10ms, 100ms));
    CHECK(tlUnseen.find("\"condition\": 0,\n            \"value\": 20,\n            \"sample_ms\": 40,\n            \"compared\": [\n"
                        "              {\n                \"variable\": \"Door.level\",\n                \"value\": null,\n"
                        "                \"sample_ms\": null\n              }\n            ]") != std::string::npos);

    // 3. Non-finite RHS in timeline JSON: both value and sample_ms are null in compared[], matching the "null with value" contract.
    const std::string rhsNan =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    expect: { Body.ratio: { ne: { variable: Door.ratio } } }\n";
    const std::string tlNan = timeline_of(rhsNan, 10ms, [](World& w) {
        w.ecus[0].ratio = 0.5;
        w.ecus[1].ratio = std::numeric_limits<double>::quiet_NaN();
    });
    CHECK(tlNan.find("\"condition\": 0,\n            \"value\": 0.5,\n            \"sample_ms\": 0,\n            \"compared\": [\n"
                     "              {\n                \"variable\": \"Door.ratio\",\n                \"value\": null,\n"
                     "                \"sample_ms\": null\n              }\n            ]") != std::string::npos);
}

TEST_CASE(timeline_extra_recorded_signals_preserve_verdict_and_emit_recorded_flag_only_when_requested)
{
    // Scenario only uses Body (writes Body.level, watches Body.body.speed). Door is NOT in the scenario at all!
    const std::string text =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - set: { Body.level: 10 }\n"
        "  - at_ms: 20\n"
        "    expect: { Body.body.speed: { ge: 100 } }\n"
        "  - at_ms: 40\n"
        "    set: { Body.level: 0 }\n";
    const ScenarioCatalog desc = catalog();
    const ScenarioRead readRes = read_scenario(text, desc);
    CHECK(readRes.scenario.has_value());
    const Scenario& s = *readRes.scenario;

    const auto run_with = [&](const std::vector<RecordedVariable>& extra, SimDuration tick,
                              const std::function<void(World&)>& setup = {}) -> std::pair<ScenarioRun, std::string> {
        World w;
        w.tick = tick;
        if (setup)
        {
            setup(w);
        }
        TimelineRecord rec(s, {}, extra);
        w.record = &rec;
        ScenarioRun run = w.run(s);
        run.file = "scenario.yaml";
        TimelineInput in;
        in.file = "simulation.yaml";
        in.time_step_ms = ms_of(at(tick));
        in.scenario = &s;
        in.run = &run;
        in.catalog = &desc;
        in.record = &rec;
        in.recorded_variables = extra;
        return {std::move(run), format_timeline_json(in)};
    };

    // 1. Without extra recorded variables: zero occurrences of "recorded" (byte-for-byte unchanged).
    const auto [baseRun, baseTimeline] = run_with({}, 10ms);
    CHECK(baseRun.status == ScenarioStatus::Passed);
    CHECK(baseTimeline.find("\"recorded\"") == std::string::npos);

    // 2. With extra recorded variables covering all 4 cases:
    //    - Body.level (written-only scenario var -> gains overlaid sampled series + recorded: true)
    //    - Body.body.speed (already-watched scenario var -> single sampled series + recorded: true)
    //    - Body.ratio (extra-only var on scenario participant -> written: false, watched: false, recorded: true)
    //    - Door.body.speed & Door.pulse (extra-only vars on NON-scenario participant -> appended + sampled)
    auto make_rec = [&](const char* p, const char* v) {
        const RecordedVariablesCheck checked = check_recorded_variables(std::vector<std::string>{std::string(p) + "." + v}, desc);
        CHECK(checked.findings.empty() && checked.variables.size() == 1);
        return checked.variables.front();
    };
    const std::vector<RecordedVariable> extras = {
        make_rec("Body", "level"),
        make_rec("Body", "body.speed"),
        make_rec("Body", "ratio"),
        make_rec("Door", "body.speed"),
        make_rec("Door", "pulse"),
    };
    const auto [recRun, recTimeline] = run_with(extras, 10ms);

    // Verdict invariance: identical ScenarioRun text across statuses, timestamps, messages!
    check_text(text_of(recRun), text_of(baseRun), "ScenarioRun invariant under extra recording");

    // Check variables[] metadata flags.
    const std::string varsPart = part(recTimeline, "  \"variables\": [", "  \"steps\": [");
    CHECK(varsPart.find("\"name\": \"Body.level\"") != std::string::npos);
    CHECK(varsPart.find("\"written\": true,\n      \"watched\": false,\n      \"recorded\": true") != std::string::npos);
    CHECK(varsPart.find("\"written\": false,\n      \"watched\": true,\n      \"recorded\": true") != std::string::npos);
    CHECK(varsPart.find("\"name\": \"Body.ratio\"") != std::string::npos);
    CHECK(varsPart.find("\"name\": \"Door.body.speed\"") != std::string::npos);
    CHECK(varsPart.find("\"name\": \"Door.pulse\"") != std::string::npos);
    CHECK(varsPart.find("\"written\": false,\n      \"watched\": false,\n      \"recorded\": true") != std::string::npos);

    // Check series[]: Body.level has both written and sampled; Body.body.speed has sampled exactly once;
    // Body.ratio, Door.body.speed, Door.pulse each have sampled with points!
    const std::string seriesPart = part(recTimeline, "  \"series\": [", "  \"lines\": [");
    CHECK(seriesPart.find("\"variable\": \"Body.level\",\n      \"source\": \"written\"") != std::string::npos);
    CHECK(seriesPart.find("\"variable\": \"Body.level\",\n      \"source\": \"sampled\"") != std::string::npos);
    const std::size_t firstSpeed = seriesPart.find("\"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\"");
    CHECK(firstSpeed != std::string::npos);
    CHECK(seriesPart.find("\"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\"", firstSpeed + 1) == std::string::npos);
    CHECK(seriesPart.find("\"variable\": \"Body.ratio\",\n      \"source\": \"sampled\"") != std::string::npos);
    CHECK(seriesPart.find("\"variable\": \"Door.body.speed\",\n      \"source\": \"sampled\"") != std::string::npos);
    CHECK(seriesPart.find("\"variable\": \"Door.pulse\",\n      \"source\": \"sampled\"") != std::string::npos);
    CHECK(seriesPart.find("[0, false]") != std::string::npos);

    // 3. Disconnected extra-only peer does NOT fail the scenario (verdict invariance)!
    const auto [discRun, discTimeline] = run_with(
        {make_rec("Door", "body.speed"), make_rec("Door", "pulse")}, 10ms, [](World& w) {
            w.ecus.erase(w.ecus.begin() + 1); // remove Door
        });
    check_text(text_of(discRun), text_of(baseRun), "ScenarioRun invariant when extra-only peer is disconnected");
    CHECK(discTimeline.find("\"name\": \"Door.body.speed\"") != std::string::npos);
    CHECK(discTimeline.find("\"name\": \"Door.pulse\"") != std::string::npos);

    // 4. Independent of controller tick and delivery policy.
    const std::string recCompared = compared(recTimeline);
    for (const SimDuration tick : {SimDuration{5ms}, SimDuration{20ms}, SimDuration{30ms}})
    {
        const auto [rTick, tTick] = run_with(extras, tick);
        check_text(compared(tTick), recCompared, "extra recorded timeline invariant across controller ticks");
    }
    for (const auto& [name, policy] : policies())
    {
        const auto [rPol, tPol] = run_with(extras, 10ms, [&](World& w) { w.policy = policy; });
        check_text(compared(tPol), recCompared, name.c_str());
    }
}

TEST_CASE(written_only_participant_with_faster_step_does_not_shift_recorded_to_ms_on_incomplete_run)
{
    // Regression test: Door is written (not read) by the scenario, has step 1 ms < default sample_period 10 ms,
    // while Body (step 10 ms) is watched by wait_until and cut short by stop_at = 50 ms.
    // Adding --record Door.body.speed enables 1 ms sampling on Door; recorded_to_ms and step results must
    // stay identical (recorded_to_ms == 40, not shifted to 49).
    const std::string text =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    set: { Door.level: 5, Body.level: 10 }\n"
        "  - wait_until: { Body.body.speed: { ge: 99999 } }\n"
        "    timeout_ms: 5000\n";
    const ScenarioCatalog desc = catalog(10ms, 1ms);
    const ScenarioRead readRes = read_scenario(text, desc);
    CHECK(readRes.scenario.has_value());
    const Scenario& s = *readRes.scenario;

    const auto run_case = [&](const std::vector<RecordedVariable>& extra, SimDuration doorSamplePeriod) {
        World w;
        w.tick = 10ms;
        w.stop_at = at(50ms);
        w.ecus[1].step = 1ms;
        w.ecus[1].sample_period = doorSamplePeriod;
        TimelineRecord rec(s, {}, extra);
        w.record = &rec;
        ScenarioRun run = w.run(s);
        run.file = "scenario.yaml";
        TimelineInput in;
        in.file = "simulation.yaml";
        in.time_step_ms = 10.0;
        in.scenario = &s;
        in.run = &run;
        in.catalog = &desc;
        in.record = &rec;
        in.recorded_variables = extra;
        return std::pair{std::move(run), format_timeline_json(in)};
    };

    const auto [baseRun, baseTl] = run_case({}, 10ms);
    const RecordedVariablesCheck checked = check_recorded_variables(std::vector<std::string>{"Door.body.speed"}, desc);
    CHECK(checked.findings.empty() && checked.variables.size() == 1);
    const auto [recRun, recTl] = run_case(checked.variables, 1ms);

    CHECK(baseRun.status == ScenarioStatus::Incomplete);
    check_text(text_of(recRun), text_of(baseRun), "incomplete ScenarioRun invariant under written-only --record");
    check_text(part(recTl, "  \"scenario\": {", "  \"variables\""),
               part(baseTl, "  \"scenario\": {", "  \"variables\""),
               "scenario header including recorded_to_ms invariant");
    CHECK(recTl.find("    \"recorded_to_ms\": 40,\n") != std::string::npos);
    const std::string doorSeries = part(recTl, "\"variable\": \"Door.body.speed\",\n      \"source\": \"sampled\"", "  \"lines\": [");
    CHECK(doorSeries.find("\"samples\": 41,\n      \"first_ms\": 0,\n      \"last_ms\": 40,") != std::string::npos);
}

TEST_CASE(write_only_incomplete_scenario_preserves_recorded_to_ms_and_extra_samples)
{
    // Regression test (Challenger B2): a write-only scenario (no conditions, no sample-based motions)
    // cut short by stop_at = 50 ms must keep recorded_to_ms from observed samples (not drop to 0 ms)
    // both without --record and with --record, preserving all recorded samples up to stop time.
    const std::string text =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    set: { Body.level: 10 }\n"
        "  - at_ms: 200\n"
        "    set: { Body.level: 0 }\n";
    const ScenarioCatalog desc = catalog(10ms, 5ms);
    const ScenarioRead readRes = read_scenario(text, desc);
    CHECK(readRes.scenario.has_value());
    const Scenario& s = *readRes.scenario;

    const auto run_case = [&](const std::vector<RecordedVariable>& extra, SimDuration doorSamplePeriod) {
        World w;
        w.tick = 10ms;
        w.stop_at = at(50ms);
        w.ecus[1].step = 5ms;
        w.ecus[1].sample_period = doorSamplePeriod;
        TimelineRecord rec(s, {}, extra);
        w.record = &rec;
        ScenarioRun run = w.run(s);
        run.file = "scenario.yaml";
        TimelineInput in;
        in.file = "simulation.yaml";
        in.time_step_ms = 10.0;
        in.scenario = &s;
        in.run = &run;
        in.catalog = &desc;
        in.record = &rec;
        in.recorded_variables = extra;
        return std::pair{std::move(run), format_timeline_json(in)};
    };

    // 1. Without --record: recorded_to_ms preserves 40 ms (last Body sample before stop_at = 50 ms), not 0 ms.
    const auto [baseRun, baseTl] = run_case({}, 10ms);
    CHECK(baseRun.status == ScenarioStatus::Incomplete);
    CHECK(baseTl.find("    \"recorded_to_ms\": 40,\n") != std::string::npos);

    // 2. With --record on both scenario participant (Body, 10 ms) and extra-only participant (Door, 5 ms):
    //    all samples up to stop_at are preserved (Body: 5 samples 0..40 ms, Door: 10 samples 0..45 ms).
    const RecordedVariablesCheck checked =
        check_recorded_variables(std::vector<std::string>{"Body.body.speed", "Door.body.speed"}, desc);
    CHECK(checked.findings.empty() && checked.variables.size() == 2);
    const auto [recRun, recTl] = run_case(checked.variables, 5ms);
    CHECK(recRun.status == ScenarioStatus::Incomplete);
    check_text(text_of(recRun), text_of(baseRun), "write-only incomplete ScenarioRun invariant under --record");
    CHECK(recTl.find("    \"recorded_to_ms\": 45,\n") != std::string::npos);
    const std::string bodySeries = part(recTl, "\"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\"",
                                        "\"variable\": \"Door.body.speed\"");
    CHECK(bodySeries.find("\"samples\": 5,\n      \"first_ms\": 0,\n      \"last_ms\": 40,") != std::string::npos);
    const std::string doorSeries = part(recTl, "\"variable\": \"Door.body.speed\",\n      \"source\": \"sampled\"",
                                        "  \"lines\": [");
    CHECK(doorSeries.find("\"samples\": 10,\n      \"first_ms\": 0,\n      \"last_ms\": 45,") != std::string::npos);
}

TEST_CASE(failing_tail_records_post_failure_samples_without_writes_or_dithers)
{
    const ScenarioCatalog desc = catalog();
    const auto make_rec = [&](const char* p, const char* v) {
        const RecordedVariablesCheck checked = check_recorded_variables(std::vector<std::string>{std::string(p) + "." + v}, desc);
        CHECK(checked.findings.empty() && checked.variables.size() == 1);
        return checked.variables.front();
    };
    const std::vector<RecordedVariable> extra = {make_rec("Door", "body.speed")};

    // 1. wait_until timeout (scenario-timeout) with subsequent step + extra signal + dither on Body.level:
    //    Body.level = 30 -> target speed 300 (+50/10ms: reaches 250 at 40ms, 300 at 50ms).
    //    Wait for Body.body.speed >= 290 with timeout_ms: 30 (fails at 30ms when speed=200).
    //    With record_after_fail = 100ms, tail runs to 130ms; step 3 (set Body.level = 0) never runs,
    //    and dither writes stop after p_fail!
    {
        const std::string text =
            "vehicle_sil_scenario: 1\n"
            "inputs:\n"
            "  Body.level: { dither: { square: { amplitude: 2, period_ms: 40 } } }\n"
            "steps:\n"
            "  - set: { Body.level: 30 }\n"
            "  - wait_until: { Body.body.speed: { ge: 290 } }\n"
            "    timeout_ms: 30\n"
            "  - after_ms: 10\n"
            "    set: { Body.level: 0 }\n";
        const Scenario s = read(text, desc);

        const auto run_case = [&](SimDuration tick, std::optional<std::chrono::milliseconds> tail,
                                  const std::function<void(World&)>& setup = {}) {
            World w;
            w.tick = tick;
            w.record_after_fail = tail;
            w.ecus[1].level = 20; // Door climbs to 200 during tail
            if (setup)
            {
                setup(w);
            }
            TimelineRecord rec(s, {}, extra);
            w.record = &rec;
            ScenarioRun run = w.run(s);
            run.file = "s.yaml";
            TimelineInput in;
            in.file = "sim.yaml";
            in.time_step_ms = ms_of(at(tick));
            in.scenario = &s;
            in.run = &run;
            in.catalog = &desc;
            in.record = &rec;
            in.recorded_variables = extra;
            return std::tuple{std::move(run), format_timeline_json(in), std::move(w.log)};
        };

        const auto [baseRun, baseTl, baseLog] = run_case(10ms, std::nullopt);
        CHECK(baseRun.status == ScenarioStatus::Failed);
        CHECK(baseTl.find("    \"recorded_to_ms\": 30,\n") != std::string::npos);

        const auto [tailRun, tailTl, tailLog] = run_case(10ms, 100ms);
        CHECK(tailRun.status == ScenarioStatus::Failed);
        CHECK(tailRun.over_at == std::optional<SimTime>(at(40ms)));
        CHECK(tailRun.steps[1].status == StepStatus::Failed && tailRun.steps[1].at == std::optional<SimTime>(at(30ms)) &&
              tailRun.steps[1].code == "scenario-timeout");
        CHECK(tailRun.steps[2].status == StepStatus::NotRun);
        CHECK(tailTl.find("    \"over_at_ms\": 40,\n    \"recorded_to_ms\": 130,\n") != std::string::npos);

        // Zero writes or dithers applied after p_fail (40ms):
        for (const std::string& entry : tailLog)
        {
            const std::size_t pos = entry.rfind(" applied at ");
            CHECK(pos != std::string::npos);
            const std::int64_t applied_ms = std::stoll(entry.substr(pos + 12));
            CHECK(applied_ms <= 40);
            CHECK(entry.find("Body.level=0") == std::string::npos);
        }
        // Both watched Body.body.speed and extra-only Door.body.speed recorded up to 130ms!
        const std::string bodySpeed =
            part(tailTl, "\"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\"", "\"variable\": \"Door.body.speed\"");
        CHECK(bodySpeed.find("\"last_ms\": 130,") != std::string::npos);
        CHECK(bodySpeed.find("[50, 280]") != std::string::npos || bodySpeed.find("[50, 300]") != std::string::npos ||
              bodySpeed.find("[60, ") != std::string::npos);
        const std::string doorSpeed =
            part(tailTl, "\"variable\": \"Door.body.speed\",\n      \"source\": \"sampled\"", "  \"lines\": [");
        CHECK(doorSpeed.find("\"last_ms\": 130,") != std::string::npos);
        CHECK(doorSpeed.find("[30, 200]") != std::string::npos);

        // Tick & delivery-policy independence for post-failure tail samples (no dither so writes don't depend on tick):
        const std::string plainWait =
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - set: { Body.level: 30 }\n"
            "  - wait_until: { Body.body.speed: { ge: 290 } }\n"
            "    timeout_ms: 30\n";
        const Scenario plainS = read(plainWait, desc);
        const auto run_plain = [&](SimDuration tick, const World::Policy& pol) {
            World w;
            w.tick = tick;
            w.policy = pol;
            w.record_after_fail = 100ms;
            w.ecus[1].level = 20;
            TimelineRecord rec(plainS, {}, extra);
            w.record = &rec;
            ScenarioRun run = w.run(plainS);
            run.file = "s.yaml";
            TimelineInput in;
            in.file = "sim.yaml";
            in.time_step_ms = ms_of(at(tick));
            in.scenario = &plainS;
            in.run = &run;
            in.catalog = &desc;
            in.record = &rec;
            in.recorded_variables = extra;
            return format_timeline_json(in);
        };
        const std::string refTail = compared(run_plain(10ms, policies().front().second));
        for (const SimDuration tick : {SimDuration{5ms}, SimDuration{20ms}, SimDuration{30ms}})
        {
            check_text(compared(run_plain(tick, policies().front().second)), refTail, "tail invariant across controller ticks");
        }
        for (const auto& [name, pol] : policies())
        {
            check_text(compared(run_plain(10ms, pol)), refTail, name.c_str());
        }
    }

    // 2. expect, hold_for_ms, and expect-beside-ramp failures all enter FailingTail and stop motions/writes!
    {
        const std::string expectText =
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - set: { Body.level: 10 }\n"
            "  - at_ms: 20\n"
            "    expect: { Body.level: 99 }\n"
            "  - at_ms: 50\n"
            "    set: { Body.level: 0 }\n";
        const Scenario s = read(expectText, desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 80ms;
        TimelineRecord rec(s, {}, extra);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed && run.over_at == std::optional<SimTime>(at(30ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(100ms)));
        CHECK(run.steps[2].status == StepStatus::NotRun);
        CHECK(w.log.size() == 1); // only initial set at 0 ms!
    }
    {
        const std::string holdText =
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - set: { Body.level: 10 }\n"
            "  - at_ms: 0\n"
            "    expect: { Body.body.speed: { le: 60 } }\n"
            "    hold_for_ms: 100\n";
        const Scenario s = read(holdText, desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 50ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        // speed is 50 at 0ms, 100 at 10ms -> hold fails at 10ms -> tail to 60ms.
        CHECK(run.status == ScenarioStatus::Failed && run.steps[1].at == std::optional<SimTime>(at(10ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(60ms)));
    }
    {
        const std::string rampBesideText =
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - at_ms: 0\n"
            "    ramp: { Body.level: { from: 0, to: 100, over_ms: 100 } }\n"
            "    expect: { Body.level: { le: 20 } }\n";
        const Scenario s = read(rampBesideText, desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 60ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        // Body.level hits 30 at 30ms -> fails at 30ms (p_fail = 40ms) -> tail to 90ms; no ramp writes > 40ms!
        CHECK(run.status == ScenarioStatus::Failed && run.steps[0].at == std::optional<SimTime>(at(30ms)));
        CHECK(run.over_at == std::optional<SimTime>(at(40ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(90ms)));
        for (const std::string& entry : w.log)
        {
            const std::size_t pos = entry.rfind(" applied at ");
            CHECK(pos != std::string::npos);
            CHECK(std::stoll(entry.substr(pos + 12)) <= 40);
        }
    }
}

TEST_CASE(failing_tail_is_not_entered_on_write_error_unusable_pass_or_no_timeline)
{
    const ScenarioCatalog desc = catalog();

    // 1. scenario-write (setter rejects level 13) -> does NOT enter FailingTail.
    {
        const Scenario s = read(kHead + "  - at_ms: 20\n    set: { Body.level: 13 }\n", desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 100ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed && run.steps[0].code == "scenario-write");
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(20ms)));
    }

    // 2. unusable() (disconnected peer on expect condition) -> does NOT enter FailingTail.
    {
        const Scenario s = read(kHead + "  - at_ms: 20\n    expect: { Body.level: 0 }\n", desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 100ms;
        w.ecus.erase(w.ecus.begin()); // remove Body so peer is disconnected
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed && run.steps[0].code == "scenario-failed");
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(20ms)));
    }

    // 3. Passed scenario -> does NOT enter FailingTail.
    {
        const Scenario s = read(kHead + "  - set: { Body.level: 10 }\n  - at_ms: 20\n    expect: { Body.level: 10 }\n", desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 100ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Passed);
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(20ms)));
    }

    // 4. record == nullptr with record_after_fail set -> stops immediately at p_fail (harmless no-op).
    {
        const Scenario s = read(kHead + "  - at_ms: 20\n    expect: { Body.level: 99 }\n", desc);
        World w;
        w.tick = 10ms;
        w.record_after_fail = 200ms;
        w.record = nullptr;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed && run.over_at == std::optional<SimTime>(at(30ms)));
        CHECK(w.ecus[0].last_step && *w.ecus[0].last_step <= at(30ms));
    }
}

TEST_CASE(failing_tail_handles_early_termination_and_p_fail_past_tail_end)
{
    const ScenarioCatalog desc = catalog();
    const RecordedVariablesCheck extraCheck = check_recorded_variables(std::vector<std::string>{"Door.body.speed"}, desc);
    CHECK(extraCheck.findings.empty() && extraCheck.variables.size() == 1);
    const std::vector<RecordedVariable> extra = extraCheck.variables;

    const std::string failAt20 =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - set: { Body.level: 30 }\n"
        "  - at_ms: 20\n"
        "    expect: { Body.level: 99 }\n";
    const Scenario s = read(failAt20, desc);

    // 1. stop_at_ms during FailingTail (fail at 20ms, p_fail = 30ms, tail_end = 320ms, stop_at = 80ms):
    //    preserves Failed verdict + failed step + over_at (30ms) and closes at 80ms.
    {
        World w;
        w.tick = 10ms;
        w.stop_at = at(80ms);
        w.record_after_fail = 300ms;
        TimelineRecord rec(s, {}, extra);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.over_at == std::optional<SimTime>(at(30ms)));
        CHECK(run.steps[1].status == StepStatus::Failed && run.steps[1].at == std::optional<SimTime>(at(20ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(80ms)));
    }

    // 2. cut_short() during FailingTail (user Stop / --timeout-ms / child crash during tail):
    //    preserves Failed verdict + failed step + over_at, drains queued samples <= tail_end,
    //    and reports in_failing_tail() == true before cut_short() and had_failing_tail() == true after.
    {
        struct AppliedPort : ScenarioPort
        {
            std::uint64_t next = 1;
            std::vector<control::OperationResult> results;
            std::uint64_t write(std::vector<control::PeerWrite> writes, SimTime at) override
            {
                control::OperationResult r;
                r.seq = next++;
                for (const auto& pw : writes)
                {
                    r.targets.push_back(control::TargetResult{pw.peer, control::TargetStatus::Applied, at, false, ""});
                }
                results.push_back(r);
                return r.seq;
            }
            std::optional<control::OperationResult> result(std::uint64_t ticket) override
            {
                return ticket > 0 && ticket <= results.size() ? std::optional(results[ticket - 1]) : std::nullopt;
            }
            std::optional<control::OperationResult> wait_done(std::uint64_t ticket) override { return result(ticket); }
            void settle() override {}
        } port;
        TimelineRecord rec(s, {}, extra);
        ScenarioEngine engine(s, 10ms, port, {}, &rec, std::nullopt, 300ms);
        std::vector<EnginePeer> peers(1);
        peers[0].connected = true;
        peers[0].schema = model_variables();
        std::vector<EnginePeer> extraPeers(1);
        extraPeers[0].connected = true;
        extraPeers[0].schema = model_variables();
        engine.begin_extra(extraPeers);
        engine.begin(peers);

        const auto snap = [](SimTime t, std::int64_t lvl, double spd) {
            auto sp = std::make_shared<control::Snapshot>();
            sp->time = t;
            sp->values = {control::Value{lvl}, control::Value{std::int64_t{0}}, control::Value{std::int64_t{0}},
                          control::Value{false}, control::Value{0.0}, control::Value{std::int64_t{0}},
                          control::Value{std::int64_t{0}}, control::Value{false}, control::Value{spd}};
            return sp;
        };
        engine.sample(0, snap(at(0ms), 30, 50.0));
        engine.extra_sample(0, snap(at(0ms), 0, 10.0));
        CHECK(!engine.tick(at(0ms)));
        engine.sample(0, snap(at(10ms), 30, 100.0));
        CHECK(!engine.tick(at(10ms)));
        engine.sample(0, snap(at(20ms), 30, 150.0));
        engine.extra_sample(0, snap(at(20ms), 0, 20.0));
        CHECK(!engine.tick(at(20ms)));
        // At tick(30ms), expect at 20ms fails -> enters FailingTail (tail_end = 320ms).
        CHECK(!engine.tick(at(30ms)));
        CHECK(engine.in_failing_tail() && engine.had_failing_tail() && !engine.over());
        CHECK(engine.run().status == ScenarioStatus::Failed && engine.run().over_at == std::optional<SimTime>(at(30ms)));

        // Advance one tail tick (40ms) and queue one more sample at 50ms before cut_short().
        engine.sample(0, snap(at(30ms), 30, 200.0));
        engine.extra_sample(0, snap(at(30ms), 0, 30.0));
        CHECK(!engine.tick(at(40ms)));
        engine.sample(0, snap(at(50ms), 30, 250.0));
        engine.extra_sample(0, snap(at(50ms), 0, 40.0));
        engine.cut_short();

        CHECK(engine.over() && !engine.in_failing_tail() && engine.had_failing_tail());
        CHECK(engine.run().status == ScenarioStatus::Failed);
        CHECK(engine.run().over_at == std::optional<SimTime>(at(30ms)));
        CHECK(engine.run().steps[1].status == StepStatus::Failed && engine.run().steps[1].at == std::optional<SimTime>(at(20ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(50ms)));

        // Calling superseded() directly after entering/closing FailingTail must be a no-op
        // that preserves the established Failed verdict and failed step.
        engine.superseded();
        CHECK(engine.run().status == ScenarioStatus::Failed);
        CHECK(engine.run().over_at == std::optional<SimTime>(at(30ms)));
        CHECK(engine.run().steps[1].status == StepStatus::Failed && engine.run().steps[1].at == std::optional<SimTime>(at(20ms)));
    }

    // 3. p_fail > tail_end_ edge case: tick = 30ms, expect fails at 10ms -> discovered at p_fail = 30ms;
    //    record_after_fail = 15ms -> tail_end = 25ms < p_fail = 30ms -> finishes immediately on tick(30ms)
    //    with recorded_to_ms == 25 and samples 0, 10, 20ms captured for both watched and extra signals!
    {
        const Scenario s10 = read("vehicle_sil_scenario: 1\n"
                                  "steps:\n"
                                  "  - set: { Body.level: 30 }\n"
                                  "  - at_ms: 10\n"
                                  "    expect: { Body.level: 99 }\n",
                                  desc);
        World w;
        w.tick = 30ms;
        w.record_after_fail = 15ms;
        w.ecus[1].level = 10;
        TimelineRecord rec(s10, {}, extra);
        w.record = &rec;
        ScenarioRun run = w.run(s10);
        run.file = "s.yaml";
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.over_at == std::optional<SimTime>(at(30ms)));
        CHECK(run.steps[1].at == std::optional<SimTime>(at(10ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(25ms)));

        TimelineInput in;
        in.file = "sim.yaml";
        in.time_step_ms = 30.0;
        in.scenario = &s10;
        in.run = &run;
        in.catalog = &desc;
        in.record = &rec;
        in.recorded_variables = extra;
        const std::string tl = format_timeline_json(in);
        const std::string doorSeries =
            part(tl, "\"variable\": \"Door.body.speed\",\n      \"source\": \"sampled\"", "  \"lines\": [");
        CHECK(doorSeries.find("\"samples\": 3,\n      \"first_ms\": 0,\n      \"last_ms\": 20,") != std::string::npos);
    }
}

TEST_CASE(failing_tail_defers_future_ecu_step_writes_across_pending_failure)
{
    // Reproduces the phys_leak counterexample: controller tick = 5ms, Door ECU step = 10ms.
    // Step 2 (expect at 100ms) fails at over_at_ms = 105ms. Step 3 (after_ms: 5, at = 105ms)
    // writes Door.level = 100 whose applied_step is 110ms (> 105ms). Because submit() defers
    // writes until applied_step <= p + tick_, Step 3 is NOT sent at tick(100) and never executes
    // during FailingTail!
    const auto desc = catalog(5ms, 10ms);
    {
        const Scenario s = read("vehicle_sil_scenario: 1\n"
                                "steps:\n"
                                "  - at_ms: 0\n"
                                "    set: { Door.level: 0 }\n"
                                "  - at_ms: 100\n"
                                "    expect: { Door.level: 99 }\n"
                                "  - after_ms: 5\n"
                                "    set: { Door.level: 100 }\n",
                                desc);
        World w;
        w.tick = 5ms;
        w.ecus[0].step = 5ms;
        w.ecus[1].step = 10ms;
        w.record_after_fail = 300ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.over_at == std::optional<SimTime>(at(105ms)));
        CHECK(run.steps[1].status == StepStatus::Failed && run.steps[1].at == std::optional<SimTime>(at(100ms)));
        CHECK(run.steps[2].status == StepStatus::NotRun);
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(400ms)));
        CHECK_EQ(w.ecus[1].level, std::int64_t{0});
        check_text(joined(w.log), "Door.level=0 for 0 applied at 0\n");
        CHECK_EQ(rec.series()[0].written.size(), std::size_t{1});
        CHECK(rec.series()[0].written[0].at == at(0ms));
    }

    // Mixed-ECU set step at after_ms: 5 targeting both Body (step 5ms -> applied 105ms) and
    // Door (step 10ms -> applied 110ms): Door's strictly-future write (110ms > over_at 105ms)
    // must still be deferred and never applied during FailingTail!
    {
        const Scenario s = read("vehicle_sil_scenario: 1\n"
                                "steps:\n"
                                "  - at_ms: 0\n"
                                "    set: { Body.level: 0, Door.level: 0 }\n"
                                "  - at_ms: 100\n"
                                "    expect: { Door.level: 99 }\n"
                                "  - after_ms: 5\n"
                                "    set: { Body.level: 77, Door.level: 100 }\n",
                                desc);
        World w;
        w.tick = 5ms;
        w.ecus[0].step = 5ms;
        w.ecus[1].step = 10ms;
        w.record_after_fail = 200ms;
        TimelineRecord rec(s);
        w.record = &rec;
        const ScenarioRun run = w.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.over_at == std::optional<SimTime>(at(105ms)));
        CHECK(rec.recorded_to() == std::optional<SimTime>(at(300ms)));
        CHECK_EQ(w.ecus[1].level, std::int64_t{0});
    }
}

TEST_CASE(timeline_lines_canonical_order_and_tick_invariance_with_mixed_ecu_steps)
{
    // Body (10ms) and Door (4ms) have different applied_step rounding for the same/nearby times:
    // - Cross-step: Step 1 at 35ms (Body due_at=40ms) before Step 2 at 36ms (Door due_at=36ms)
    // - Within-step set: Step 3 at 45ms writes Body (due_at=50ms) then Door (due_at=48ms)
    // - Within-step ramp: Step 4 at 55ms moves Body (due_at=60ms) then Door (due_at=56ms)
    // lines[] must keep canonical (from, dither, step, part) order regardless of controller tick!
    const auto desc = catalog(10ms, 4ms);
    const Scenario s = read("vehicle_sil_scenario: 1\n"
                            "steps:\n"
                            "  - at_ms: 35\n"
                            "    set: { Body.level: 10 }\n"
                            "  - at_ms: 36\n"
                            "    set: { Door.level: 5 }\n"
                            "  - at_ms: 45\n"
                            "    set: { Body.level: 20, Door.level: 15 }\n"
                            "  - at_ms: 55\n"
                            "    ramp: { Body.level: { from: 20, to: 30, over_ms: 10 }, Door.level: { from: 15, to: 25, over_ms: 8 } }\n",
                            desc);
    const auto run_with_tick = [&](SimDuration tick) {
        World w;
        w.tick = tick;
        w.ecus[0].step = 10ms;
        w.ecus[1].step = 4ms;
        TimelineRecord rec(s);
        w.record = &rec;
        ScenarioRun run = w.run(s);
        run.file = "s.yaml";
        CHECK(run.status == ScenarioStatus::Passed);
        TimelineInput in;
        in.file = "sim.yaml";
        in.time_step_ms = static_cast<double>(tick.count()) / 1'000'000.0;
        in.scenario = &s;
        in.run = &run;
        in.catalog = &desc;
        in.record = &rec;
        const std::string json = format_timeline_json(in);
        return std::pair{part(json, "  \"lines\": [", "  \"findings\": ["),
                         part(json, "  \"steps\": [", "  \"series\": [")};
    };
    const auto [lines4, steps4] = run_with_tick(4ms);
    const auto [lines10, steps10] = run_with_tick(10ms);
    check_text(lines4, lines10);
    // Verify canonical order: 35 Body, 36 Door, 45 Body, 45 Door, 55 Body, 55 Door.
    const auto pos = [&](const std::string& needle, std::size_t from = 0) {
        const auto p = lines4.find(needle, from);
        CHECK(p != std::string::npos);
        return p;
    };
    const std::size_t p0 = pos("\"variable\": \"Body.level\",\n      \"kind\": \"set\",\n      \"line\": 3,\n      \"step\": 1,\n      \"from_ms\": 35,");
    const std::size_t p1 = pos("\"variable\": \"Door.level\",\n      \"kind\": \"set\",\n      \"line\": 5,\n      \"step\": 2,\n      \"from_ms\": 36,", p0);
    const std::size_t p2 = pos("\"variable\": \"Body.level\",\n      \"kind\": \"set\",\n      \"line\": 7,\n      \"step\": 3,\n      \"from_ms\": 45,", p1);
    const std::size_t p3 = pos("\"variable\": \"Door.level\",\n      \"kind\": \"set\",\n      \"line\": 7,\n      \"step\": 3,\n      \"from_ms\": 45,", p2);
    const std::size_t p4 = pos("\"variable\": \"Body.level\",\n      \"kind\": \"ramp\",\n      \"line\": 9,\n      \"step\": 4,\n      \"from_ms\": 55,", p3);
    const std::size_t p5 = pos("\"variable\": \"Door.level\",\n      \"kind\": \"ramp\",\n      \"line\": 9,\n      \"step\": 4,\n      \"from_ms\": 55,", p4);
    CHECK(p0 < p1 && p1 < p2 && p2 < p3 && p3 < p4 && p4 < p5);
    CHECK(steps4.find("\"lines\": [0]") != std::string::npos);
    CHECK(steps4.find("\"lines\": [1]") != std::string::npos);
    CHECK(steps4.find("\"lines\": [2, 3]") != std::string::npos);
    CHECK(steps4.find("\"lines\": [4, 5]") != std::string::npos);
}

TEST_CASE(timeline_marks_not_applied_from_when_abandoned_write_has_at_le_over_at_lt_due_at)
{
    // Body (4ms) and Door (10ms) with controller tick = 5ms. At tick(5), submit(5, 10) submits
    // both Body (at=5ms, due_at=8ms) and Door (at=5ms, due_at=10ms). Body refuses immediately in
    // taken_at_once(), ending the scenario at over_at=5ms and abandoning Door's write (applied
    // step 10ms > over_at 5ms). Even though Door's point has at == over_at == 5ms, its applied
    // step (10ms) is strictly after over_at (5ms), so not_applied_from must be 0, not null!
    const auto desc = catalog(4ms, 10ms);
    const Scenario s = read("vehicle_sil_scenario: 1\n"
                            "steps:\n"
                            "  - at_ms: 5\n"
                            "    set: { Body.level: 10 }\n"
                            "  - at_ms: 5\n"
                            "    set: { Door.level: 20 }\n",
                            desc);
    World w;
    w.tick = 5ms;
    w.ecus[0].step = 4ms;
    w.ecus[1].step = 10ms;
    w.refuse = [](const std::string& peer, SimTime) -> std::optional<control::TargetResult> {
        if (peer != "Body") return std::nullopt;
        control::TargetResult r;
        r.peer = peer;
        r.status = control::TargetStatus::Invalid;
        r.reason = "refused at once";
        return r;
    };
    TimelineRecord rec(s);
    w.record = &rec;
    ScenarioRun run = w.run(s);
    run.file = "s.yaml";
    CHECK(run.status == ScenarioStatus::Failed);
    CHECK(run.over_at == std::optional<SimTime>(at(5ms)));
    CHECK_EQ(w.ecus[1].level, std::int64_t{0});

    TimelineInput in;
    in.file = "sim.yaml";
    in.time_step_ms = 5.0;
    in.scenario = &s;
    in.run = &run;
    in.catalog = &desc;
    in.record = &rec;
    const std::string json = format_timeline_json(in);
    const std::string doorSeries =
        part(json, "\"variable\": \"Door.level\",\n      \"source\": \"written\"", "  \"lines\": [");
    CHECK(doorSeries.find("\"not_applied_from\": 0,") != std::string::npos);
    CHECK(doorSeries.find("[5, 20]") != std::string::npos);
}

TEST_CASE(manual_timeline_record_and_json_format_without_scenario)
{
    const auto desc = catalog(10ms, 10ms);
    RecordedVariable v1;
    v1.participant = "Body";
    v1.name = "body.speed";
    v1.text = "Body.body.speed";
    v1.type = control::ValueType::Real;
    v1.step = 10ms;
    RecordedVariable v2;
    v2.participant = "Door";
    v2.name = "mode";
    v2.text = "Door.mode";
    v2.type = control::ValueType::Enum;
    v2.items = desc[1].variables[2].items;
    v2.step = 10ms;

    // 1. Manual TimelineRecord deduplicates inputs, creates sampled-only series, truncates at close(SimTime).
    const std::vector<RecordedVariable> requested{v1, v2, v1};
    TimelineRecord record(requested);
    CHECK_EQ(record.variable_count(), std::size_t{2});
    CHECK(!record.is_written(0) && !record.is_watched(0) && record.is_recorded(0));
    CHECK(!record.is_written(1) && !record.is_watched(1) && record.is_recorded(1));
    CHECK(record.watches().empty() && record.failures().empty() && record.lines().empty());

    record.sampled(0, at(0ms), control::Value{800.0});
    record.sampled(0, at(10ms), control::Value{800.0});
    record.sampled(0, at(20ms), control::Value{1200.0});
    record.sampled(0, at(30ms), control::Value{1500.0});
    record.sampled(1, at(0ms), control::Value{std::int64_t{0}});
    record.sampled(1, at(20ms), control::Value{std::int64_t{2}});
    record.close(at(20ms));
    // Further samples after close() are ignored.
    record.sampled(0, at(40ms), control::Value{2000.0});
    CHECK(record.recorded_to() == std::optional<SimTime>(at(20ms)));
    CHECK_EQ(record.series().size(), std::size_t{2});
    CHECK_EQ(record.series()[0].sample_count, std::uint64_t{3});
    CHECK(record.series()[0].last_sample == std::optional<SimTime>(at(20ms)));

    TimelineInput in;
    in.file = "sim.yaml";
    in.time_step_ms = 10;
    in.scenario = nullptr;
    in.run = nullptr;
    in.catalog = &desc;
    in.record = &record;
    in.recorded_variables = {v1, v2};
    in.findings = {
        Finding{Severity::Warning, "control-link-late-values", "late", 0, "Body", "", "", ""},
        Finding{Severity::Warning, "behind-real-time", "slow", 0, "", "", "", ""},
    };
    const std::string json = format_timeline_json(in);
    CHECK(json.find("  \"vehicle_sil_timeline\": 1,\n") != std::string::npos);
    CHECK(json.find("  \"stage\": \"run\",\n") != std::string::npos);
    CHECK(json.find("  \"params\": [],\n") != std::string::npos);
    CHECK(json.find("  \"scenario\": null,\n  \"recorded_to_ms\": 20,\n") != std::string::npos);
    CHECK(json.find("  \"steps\": [],\n") != std::string::npos);
    CHECK(json.find("  \"lines\": [],\n") != std::string::npos);
    CHECK(json.find("\"written\": false,\n      \"watched\": false,\n      \"recorded\": true") != std::string::npos);
    CHECK(json.find("\"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\"") != std::string::npos);
    CHECK(json.find("\"variable\": \"Door.mode\",\n      \"source\": \"sampled\"") != std::string::npos);
    CHECK(json.find("\"code\": \"control-link-late-values\"") != std::string::npos);
    CHECK(json.find("behind-real-time") == std::string::npos);

    // 2. Manual TimelineRecord::close(std::nullopt) handles zero samples cleanly and rejects later samples.
    TimelineRecord emptyRecord(std::span<const RecordedVariable>{&v1, 1});
    emptyRecord.close(std::nullopt);
    emptyRecord.sampled(0, at(10ms), control::Value{900.0});
    CHECK(!emptyRecord.recorded_to().has_value());
    CHECK_EQ(emptyRecord.series().size(), std::size_t{1});
    CHECK_EQ(emptyRecord.series()[0].sample_count, std::uint64_t{0});
    CHECK(emptyRecord.series()[0].samples.empty());

    TimelineInput emptyIn = in;
    emptyIn.record = &emptyRecord;
    emptyIn.findings.clear();
    const std::string emptyJson = format_timeline_json(emptyIn);
    CHECK(emptyJson.find("  \"scenario\": null,\n  \"recorded_to_ms\": null,\n") != std::string::npos);
}
