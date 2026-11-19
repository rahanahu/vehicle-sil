# SPDX-License-Identifier: MIT
"""Tests of .github/workflows/ci.yml: the job `all checks` (the one required check of the branch protection) must
wait for every other job and must run even when one of them failed or was cancelled.

    python tests/scripts/test_ci_workflow.py      (exit code 0 when every test passes; needs PyYAML)
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
JOBS = yaml.safe_load((ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8"))["jobs"]
GATE = "all-checks"
# Jobs that `all checks` does not wait for: the thread sanitizer, which does not hold a pull request (its result there
# is a success because of continue-on-error), and which fails the run on main (release.yml reads the run).
NOT_WAITED = {"linux-tsan"}


class AllChecks(unittest.TestCase):
    def test_the_name_is_the_one_the_protection_uses(self):
        self.assertEqual(JOBS[GATE]["name"], "all checks")

    def test_needs_every_other_job(self):
        needs = JOBS[GATE]["needs"]
        needs = [needs] if isinstance(needs, str) else needs
        others = sorted(name for name in JOBS if name != GATE and name not in NOT_WAITED)
        self.assertEqual(sorted(needs), others,
                         f"add a new job to `needs` of {GATE} in .github/workflows/ci.yml (and to its RESULTS list)")

    def test_lists_every_result(self):
        results = JOBS[GATE]["steps"][0]["env"]["RESULTS"]
        for name in JOBS:
            if name != GATE and name not in NOT_WAITED:
                self.assertIn(f"needs.{name}.result", results, f"RESULTS of {GATE} does not list {name}")

    def test_a_job_not_waited_for_holds_only_main(self):
        # Not waited for, it must not fail a pull request's run silently, and must count on main.
        for name in NOT_WAITED:
            self.assertEqual(str(JOBS[name]["continue-on-error"]).strip(),
                             "${{ github.event_name == 'pull_request' }}", name)

    def test_runs_after_a_failure_and_a_cancel(self):
        # `!cancelled()` would leave the job skipped in a cancelled run, and a skipped required check passes.
        self.assertEqual(str(JOBS[GATE]["if"]).strip(), "always()")


if __name__ == "__main__":
    unittest.main()
