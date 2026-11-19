// SPDX-License-Identifier: MIT
//
// compare_commands() (check --compare-commands): what counts as the commands that run (each
// participant's command, describe, manual and enabled, in the common part and in every variant; the
// participants, the variants, registry_uri), what does not count, the note that says what differs,
// and the texts that are not compared because what they say does not read.
#include "launch_test_util.hpp"

#include "vehicle_sil/config/check.hpp"

#include <cstdio>
#include <string>
#include <utility>

using namespace vehicle_sil;
using namespace vehicle_sil::config;

namespace {

constexpr const char* kBase = R"(vehicle_sil_simulation: 1
networks:
  - name: CAN1
    kind: can
participants:
  - name: Engine
    command: [engine/engine_ecu, --mode, a]
  - name: Brake
    command: [brake_ecu]
)";

constexpr const char* kVariants = R"(vehicle_sil_simulation: 1
networks:
  - name: CAN1
    kind: can
participants:
  - name: Engine
    command: [engine/engine_ecu]
  - name: Brake
    command: [brake_ecu]
variants:
  small: {}
  big:
    participants:
      Engine: { command: [engine/engine_big] }
)";

/// `text` with `from` replaced by `to` (once; `from` must be there).
std::string with(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos)
    {
        text.replace(at, from.size(), to);
    }
    return text;
}

CommandsComparison compare(const std::string& text, const std::string& other)
{
    return compare_commands(text, other, "saved.yaml");
}

/// The message of the note, or "" without one.
std::string note_of(const CommandsComparison& c) { return c.note ? c.note->message : std::string{}; }

/// true if the result is Changed with the note commands-changed saying `differences`.
bool changed(const CommandsComparison& c, const std::string& differences)
{
    const bool ok = c.result == CommandsCompared::Changed && c.note && c.note->code == "commands-changed" &&
                    c.note->severity == Severity::Note &&
                    c.note->message == "the commands that run differ from saved.yaml: " + differences;
    if (!ok)
    {
        std::fprintf(stderr, "  note: %s\n", note_of(c).c_str());
    }
    return ok;
}

/// true if the result is NotCompared with the note commands-not-compared giving `why`.
bool not_compared(const CommandsComparison& c, const std::string& why)
{
    const bool ok = c.result == CommandsCompared::NotCompared && c.note && c.note->code == "commands-not-compared" &&
                    c.note->severity == Severity::Note &&
                    c.note->message == "the commands that run are not compared with saved.yaml: " + why;
    if (!ok)
    {
        std::fprintf(stderr, "  note: %s\n", note_of(c).c_str());
    }
    return ok;
}

} // namespace

TEST_CASE(commands_of_the_same_text_are_the_same)
{
    const CommandsComparison same = compare(kBase, kBase);
    CHECK(same.result == CommandsCompared::Same && !same.note);
    CHECK(compare(kVariants, kVariants).result == CommandsCompared::Same);
}

TEST_CASE(commands_ignore_what_does_not_say_what_runs)
{
    // Networks, external, network_names, variables, params, the times, the order of the
    // participants and of the keys, comments: none of them is a command that runs.
    const std::string edited = R"(vehicle_sil_simulation: 1
# a comment
stop_at_ms: 1000
params: { idle_rpm: 800 }
networks:
  - name: CAN1
    kind: can
    external: { bitrate: 500000 }
  - name: ETH1
    kind: ethernet
participants:
  - name: Brake
    command: [brake_ecu]
    step_ms: 10
  - command: [engine/engine_ecu, --mode, a]
    name: Engine
    network_names: { CAN1: J1939_CAN }
    variables: { engine.turbo: turbo_bar }
)";
    const CommandsComparison same = compare(edited, kBase);
    if (same.note)
    {
        std::fprintf(stderr, "  note: %s\n", same.note->message.c_str());
    }
    CHECK(same.result == CommandsCompared::Same && !same.note);
}

TEST_CASE(commands_differ_in_a_command_describe_manual_or_enabled)
{
    CHECK(changed(compare(with(kBase, "--mode, a]", "--mode, b]"), kBase), "participant 'Engine': command differs"));
    // The whole list counts: an argument more.
    CHECK(changed(compare(with(kBase, "--mode, a]", "--mode, a, --verbose]"), kBase),
                  "participant 'Engine': command differs"));
    CHECK(changed(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    describe: false\n"), kBase),
                  "participant 'Brake': describe differs"));
    CHECK(changed(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    manual: true\n"), kBase),
                  "participant 'Brake': manual differs"));
    CHECK(changed(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    enabled: false\n"), kBase),
                  "participant 'Brake': enabled differs"));
    CHECK(changed(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    describe: false\n    manual: true\n    enabled: false\n"),
                          kBase),
                  "participant 'Brake': describe, manual and enabled differ"));
    // Written as it is by default: the same.
    CHECK(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    describe: true\n    manual: false\n    enabled: true\n"), kBase)
              .result == CommandsCompared::Same);
}

TEST_CASE(commands_differ_in_the_participants_and_registry_uri)
{
    CHECK(changed(compare(std::string(kBase) + "  - name: Gateway\n    command: [gateway]\n", kBase),
                  "participant 'Gateway' is new"));
    CHECK(changed(compare(with(kBase, "  - name: Brake\n    command: [brake_ecu]\n", ""), kBase),
                  "participant 'Brake' was removed"));
    // A participant renamed is one removed and one new.
    CHECK(changed(compare(with(kBase, "name: Brake", "name: Brakes"), kBase),
                  "participant 'Brakes' is new; participant 'Brake' was removed"));
    const std::string loopback = with(kBase, "networks:", "registry_uri: silkit://127.0.0.1:8500\nnetworks:");
    CHECK(changed(compare(loopback, kBase), "registry_uri differs"));
    CHECK(changed(compare(with(loopback, "127.0.0.1", "0.0.0.0"), loopback), "registry_uri differs"));
    CHECK(compare(loopback, loopback).result == CommandsCompared::Same);
}

TEST_CASE(commands_are_compared_in_every_variant)
{
    // In one variant only (and not in the variant that is chosen for a check: none is).
    CHECK(changed(compare(with(kVariants, "[engine/engine_big]", "[engine/engine_big, --fast]"), kVariants),
                  "participant 'Engine': command differs (variant 'big')"));
    CHECK(changed(compare(with(kVariants, "small: {}", "small:\n    participants:\n      Brake: { enabled: false }"), kVariants),
                  "participant 'Brake': enabled differs (variant 'small')"));
    // In the common part: each variant that does not write its own differs too, said once with
    // the places, so that the note has room for the other differences.
    CHECK(changed(compare(with(kVariants, "[brake_ecu]", "[brake_ecu, -v]"), kVariants),
                  "participant 'Brake': command differs (the common part, variant 'small' and variant 'big')"));
    // A variant's command over a common one that changed: that variant runs the same.
    CHECK(changed(compare(with(kVariants, "[engine/engine_ecu]", "[engine/engine_other]"), kVariants),
                  "participant 'Engine': command differs (the common part and variant 'small')"));
    // Another difference in one place is said apart.
    CHECK(changed(compare(with(with(kVariants, "[brake_ecu]", "[brake_ecu, -v]"), "small: {}",
                               "small:\n    participants:\n      Brake: { enabled: false }"),
                          kVariants),
                  "participant 'Brake': command differs (the common part and variant 'big'); participant 'Brake': command "
                  "and enabled differ (variant 'small')"));
    // The names of the variants.
    CHECK(changed(compare(with(kVariants, "small: {}", "small: {}\n  medium: {}"), kVariants), "variant 'medium' is new"));
    CHECK(changed(compare(with(kVariants, "small: {}", "tiny: {}"), kVariants),
                  "variant 'tiny' is new; variant 'small' was removed"));
    // A variant added, with the common part as it is.
    CHECK(changed(compare(kVariants, with(kVariants, "  small: {}\n", "")), "variant 'small' is new"));
}

TEST_CASE(commands_compare_what_run_adds_for_an_executable_that_is_not_ecu_main)
{
    // describe: false: run adds --map-network and --j1939-node to any executable, which reads them
    // as it reads any argument.
    const std::string tool = std::string(kBase) + "  - name: Tool\n    command: [tool, \"{registry_uri}\"]\n    describe: false\n";
    CHECK(changed(compare(tool + "    network_names: { CAN1: anything }\n", tool),
                  "participant 'Tool': the arguments that run adds differ"));
    CHECK(changed(compare(tool + "    j1939_nodes:\n      - { network: CAN1, address: 0x20 }\n", tool),
                  "participant 'Tool': the arguments that run adds differ"));
    CHECK(changed(compare(with(tool + "    network_names: { CAN1: anything }\n", "[tool,", "[other,"), tool),
                  "participant 'Tool': command and the arguments that run adds differ"));
    // A line that renames nothing adds nothing.
    CHECK(compare(tool + "    network_names: { CAN1: CAN1 }\n", tool).result == CommandsCompared::Same);
    // In a variant too.
    const std::string withVariant = tool + "variants:\n  small: {}\n";
    CHECK(changed(compare(with(withVariant, "small: {}", "small:\n    participants:\n      Tool: { network_names: { CAN1: x } }"),
                          withVariant),
                  "participant 'Tool': the arguments that run adds differ (variant 'small')"));
    // An ecu_main executable (describe: true) reads them in their fixed form: editing them is what a
    // screen of the networks does, and the commands stay the same.
    CHECK(compare(with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    network_names: { CAN1: J1939_CAN }\n"), kBase).result ==
          CommandsCompared::Same);
}

TEST_CASE(commands_that_differ_much_are_counted)
{
    std::string many = with(kBase, "--mode, a]", "--mode, b]");
    many = with(many, "[brake_ecu]\n", "[brake_ecu]\n    manual: true\n");
    many += "  - name: Gateway\n    command: [gateway]\n  - name: Logger\n    command: [logger]\n";
    many = with(many, "networks:", "registry_uri: silkit://127.0.0.1:8500\nnetworks:");
    CHECK(changed(compare(many, kBase), "registry_uri differs; participant 'Gateway' is new; participant 'Logger' is new; and 2 "
                                        "more differences"));
    CHECK(changed(compare(with(many, "registry_uri: silkit://127.0.0.1:8500\n", ""), kBase),
                  "participant 'Gateway' is new; participant 'Logger' is new; participant 'Engine': command differs; and 1 "
                  "more difference"));
}

TEST_CASE(commands_that_do_not_read_are_not_compared)
{
    const std::string ours = "the configuration being checked has errors in ";
    const std::string fixOurs = ", so what it runs is not known for sure: see the errors, and correct them";
    const std::string theirs = "it has errors in ";
    const std::string fixTheirs =
        ", so what it runs is not known for sure: check it with vehicle_sil_launch check to see them, and correct them";
    // A participant without a command, on either side; the other side is the same text.
    const std::string noCommand = with(kBase, "    command: [brake_ecu]\n", "");
    CHECK(not_compared(compare(noCommand, noCommand), ours + "participants" + fixOurs));
    CHECK(not_compared(compare(kBase, noCommand), theirs + "participants" + fixTheirs));
    // An error in a key that does not make the participant invalid (manual) still counts.
    const std::string badManual = with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    manual: maybe\n");
    CHECK(not_compared(compare(badManual, kBase), ours + "participants" + fixOurs));
    // A participant entry that is not one, which the reader leaves out.
    CHECK(not_compared(compare(std::string(kBase) + "  - just a text\n", kBase), ours + "participants" + fixOurs));
    // registry_uri, and a key written twice.
    CHECK(not_compared(compare(with(kBase, "networks:", "registry_uri: localhost\nnetworks:"), kBase),
                       ours + "registry_uri" + fixOurs));
    CHECK(not_compared(compare(std::string(kBase) + "participants: []\n", kBase), ours + "participants" + fixOurs));
    // In a variant: a command that is not one, an enabled that is not true or false, a participant
    // the file does not have, a variant that is not a mapping, a name that is not one.
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"[engine/engine_big]", "[]"},
                                   {"{ command: [engine/engine_big] }", "{ enabled: maybe }"},
                                   {"Engine: {", "Engines: {"},
                                   {"small: {}", "small: 3"},
                                   {"small: {}", "-small: {}"}})
    {
        CHECK(not_compared(compare(with(kVariants, from, to), kVariants), ours + "variants" + fixOurs));
    }
    // Errors in two of them: both are named.
    CHECK(not_compared(compare(with(noCommand, "networks:", "registry_uri: localhost\nnetworks:"), kBase),
                       ours + "registry_uri and participants" + fixOurs));
    // An error that a rule after the reading finds: the participant is invalid in that variant.
    CHECK(not_compared(compare(with(kVariants, "{ command: [engine/engine_big] }", "{ network_names: { CAN9: X } }"),
                               kVariants),
                       ours + "participant 'Engine' in variant 'big'" + fixOurs));
    CHECK(not_compared(compare(kBase, with(kBase, "[brake_ecu]\n", "[brake_ecu]\n    network_names: { CAN9: X }\n")),
                       theirs + "participant 'Brake'" + fixTheirs));
    // A text that is not a configuration.
    CHECK(not_compared(compare("just a text\n", kBase),
                       "the configuration being checked cannot be read: see the errors"));
    CHECK(not_compared(compare(kBase, "vehicle_sil_simulation: 99\n"),
                       "it cannot be read as a configuration: check it with vehicle_sil_launch check to see why"));
    // An error elsewhere does not stop the comparison.
    CHECK(compare(with(kBase, "kind: can", "kind: canbus"), kBase).result == CommandsCompared::Same);
    CHECK(compare(with(kBase, "networks:", "stop_at_ms: -1\nnetworks:"), kBase).result == CommandsCompared::Same);
}
