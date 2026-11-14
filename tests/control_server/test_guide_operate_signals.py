# SPDX-License-Identifier: MIT
"""The guide docs/howto/operate-signals.md against what it shows: the examples of
examples/launch_control, the CLI (python -m vehicle_sil_control) and the commands of the launcher's
standard input, with the two ECUs of examples/ecu_main. Each block of the guide is the fenced one
after a line "<!-- ... -->" there:

- examples/launch_control/write_and_read.py            the file without its first line (the SPDX line)
- <an example>: the output                             what the example prints (write_and_read.py and
                                                       write_before_start.py: but for the port; pause_and_step.py:
                                                       but for the numbers, which follow the real time)
- <a file>: an excerpt                                 consecutive lines of the file, whatever their indentation
- vehicle_sil_control <arguments>                      what the CLI prints for these arguments, the blocks one
                                                       after the other against one run that stands still before
                                                       its first step (but for the operation's number and the
                                                       widths of the columns; ": the first lines": its first lines)
- standard input of run --start-paused --stdin-commands: the input, and: the lines of the Dashboard
                                                       the lines given on standard input, and the lines the
                                                       dashboard then writes

Environment: VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_EXAMPLE_DIR (examples/ecu_main in the build tree),
VEHICLE_SIL_REPOSITORY (the source tree), PYTHONPATH with the repository's python/ directory,
VEHICLE_SIL_CONTROL_STUBS.
"""
import json
import os
import pathlib
import re
import subprocess
import sys
import unittest

from vehicle_sil_control import Launch

LAUNCH = CONFIG = REPOSITORY = GUIDE = None
MARKER = re.compile(r"^<!-- ([^\n]+) -->\n```[a-z]*\n(.*?)^```$", re.MULTILINE | re.DOTALL)


def setUpModule():
    # Read here, not on import (scripts/verify-python.py imports the module without them).
    global LAUNCH, CONFIG, REPOSITORY, GUIDE
    LAUNCH = os.environ["VEHICLE_SIL_LAUNCH_EXE"]
    CONFIG = str(pathlib.Path(os.environ["VEHICLE_SIL_EXAMPLE_DIR"]) / "simulation.yaml")
    REPOSITORY = pathlib.Path(os.environ["VEHICLE_SIL_REPOSITORY"])
    GUIDE = REPOSITORY / "docs" / "howto" / "operate-signals.md"


def blocks():
    """(marker, block) of the guide, in its order."""
    found = MARKER.findall(GUIDE.read_text(encoding="utf-8"))
    if not found:
        raise AssertionError(f"{GUIDE} has no block after a line <!-- ... -->")
    return found


def block(marker: str) -> str:
    texts = [text for name, text in blocks() if name == marker]
    if len(texts) != 1:
        raise AssertionError(f"{GUIDE} has {len(texts)} blocks after a line <!-- {marker} -->, not 1: put the line "
                             "back before the block it marks")
    return texts[0]


def run_example(name: str, *arguments: str) -> str:
    done = subprocess.run([sys.executable, str(REPOSITORY / "examples" / "launch_control" / name), *arguments],
                          capture_output=True, text=True, timeout=120)
    if done.returncode != 0:
        raise AssertionError(f"{name} ended with {done.returncode}\n{done.stdout}\n{done.stderr}")
    return done.stdout


class TheGuide(unittest.TestCase):
    maxDiff = None

    def same(self, actual: str, expected: str, what: str) -> None:
        self.assertEqual(actual, expected, f"{what} and {GUIDE} differ: make them the same (the guide follows the "
                                           "examples and the tools)")

    def test_the_first_script_is_the_example_and_prints_what_the_guide_shows(self):
        text = (REPOSITORY / "examples" / "launch_control" / "write_and_read.py").read_text(encoding="utf-8")
        self.same(text.split("\n", 1)[1], block("examples/launch_control/write_and_read.py"), "write_and_read.py")
        self.same(run_example("write_and_read.py", LAUNCH, CONFIG),
                  block("examples/launch_control/write_and_read.py: the output"), "the output of write_and_read.py")

    def test_the_examples_print_what_the_guide_shows(self):
        port = lambda text: re.sub(r":\d+\n", ":<port>\n", text, count=1)
        self.same(port(run_example("write_before_start.py", "--launch", LAUNCH, CONFIG)),
                  port(block("examples/launch_control/write_before_start.py: the output")),
                  "the output of write_before_start.py")
        numbers = lambda text: re.sub(r"\d+", "<n>", text)
        self.same(numbers(run_example("pause_and_step.py", "--launch", LAUNCH, CONFIG)),
                  numbers(block("examples/launch_control/pause_and_step.py: the output")),
                  "the output of pause_and_step.py but for its numbers")

    def test_the_excerpts_are_lines_of_the_files(self):
        excerpts = [(name[:-len(": an excerpt")], text) for name, text in blocks() if name.endswith(": an excerpt")]
        self.assertGreaterEqual(len(excerpts), 1)
        for path, text in excerpts:
            with self.subTest(path=path, starts_with=text.splitlines()[0]):
                lines = [line.strip() for line in (REPOSITORY / path).read_text(encoding="utf-8").splitlines()]
                wanted = [line.strip() for line in text.splitlines()]
                at = [i for i in range(len(lines) - len(wanted) + 1) if lines[i:i + len(wanted)] == wanted]
                self.assertTrue(at, f"these lines of {GUIDE} are not consecutive lines of {path}: copy them from "
                                    f"the file again\n{text}")

    def test_the_cli_prints_what_the_guide_shows(self):
        commands = [(name, text) for name, text in blocks() if name.startswith("vehicle_sil_control ")]
        self.assertGreaterEqual(len(commands), 1)
        normal = lambda text: re.sub(r"seq \d+", "seq <n>", "\n".join(" ".join(line.split()) for line in text.splitlines()))
        with Launch(LAUNCH, CONFIG, start_paused=True) as launch:
            for name, expected in commands:
                with self.subTest(command=name):
                    first_lines = name.endswith(": the first lines")
                    arguments = name.removesuffix(": the first lines").split()[1:]
                    done = subprocess.run([sys.executable, "-m", "vehicle_sil_control", "--server", launch.address, *arguments],
                                          capture_output=True, text=True, timeout=60)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    actual = normal(done.stdout)
                    if first_lines:
                        actual = "\n".join(actual.splitlines()[:len(expected.splitlines())])
                    self.same(actual, normal(expected), f"what `vehicle_sil_control {' '.join(arguments)}` prints")
            self.assertEqual(launch.wait(60)["result"], "passed", "the run of the guide's commands (the last one resumes it)")

    def test_the_commands_of_standard_input_do_what_the_guide_shows(self):
        name = "standard input of run --start-paused --stdin-commands"
        commands = block(f"{name}: the input").splitlines()
        expected = block(f"{name}: the lines of the Dashboard").splitlines()
        run = subprocess.Popen([LAUNCH, "run", "--events", "--start-paused", "--stdin-commands", CONFIG],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            seen, sent = [], 0
            for line in run.stdout:
                event = json.loads(line)
                if event["event"] == "log" and event["source"] == "Dashboard":
                    seen.append(f"[Dashboard] {event['line']}")
                # Each command once the run stands still: a step is refused otherwise.
                if event["event"] == "time" and event["paused"] and sent < len(commands):
                    run.stdin.write(commands[sent] + "\n")
                    run.stdin.flush()
                    sent += 1
            self.assertEqual(run.wait(60), 0)
        finally:
            if run.poll() is None:
                run.kill()
            run.stdin.close()
            run.stdout.close()
        self.same("\n".join(seen), "\n".join(expected), "the dashboard's lines")


if __name__ == "__main__":
    unittest.main()
