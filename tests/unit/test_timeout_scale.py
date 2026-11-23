# SPDX-License-Identifier: MIT
"""Unit tests for tests/common/timeout_scale.py and tests/serve/serve_support.py parity."""

from __future__ import annotations

import os
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = str(ROOT / "tests" / "common")
SERVE = str(ROOT / "tests" / "serve")
for d in (COMMON, SERVE):
    if d not in sys.path:
        sys.path.insert(0, d)

import serve_support  # noqa: E402
import timeout_scale  # noqa: E402


class TimeoutScaleTest(unittest.TestCase):
    def test_valid_values(self) -> None:
        cases = [
            ("3", 3.0),
            ("0.5", 0.5),
            (".5", 0.5),
            ("5.", 5.0),
            ("007", 7.0),
            ("0.001", 0.001),
            ("2.5", 2.5),
        ]
        for text, expected in cases:
            with self.subTest(text=text):
                self.assertEqual(timeout_scale.parse_timeout_scale(text), expected)

    def test_invalid_values(self) -> None:
        invalid = [
            "",
            "0",
            "0.0",
            "000",
            "000.0",
            ".",
            "-1",
            "+3",
            "abc",
            "3x",
            " ",
            " 3",
            "3 ",
            "3\n",
            "1e3",
            "1e-3",
            "0x10",
            "1_000",
            "1.2.3",
            "inf",
            "nan",
            "Infinity",
            "NaN",
            "٢",
            "²",
            "9" * 400,
            "0." + ("0" * 400) + "1",
        ]
        for text in invalid:
            with self.subTest(text=repr(text[:20])):
                self.assertIsNone(timeout_scale.parse_timeout_scale(text))

    def test_read_timeout_scale_precedence_and_errors(self) -> None:
        self.assertEqual(timeout_scale.read_timeout_scale("3", "4"), 3.0)
        self.assertEqual(timeout_scale.read_timeout_scale("0.5", "4"), 0.5)
        self.assertEqual(timeout_scale.read_timeout_scale(None, "4"), 4.0)
        self.assertEqual(timeout_scale.read_timeout_scale("", "4"), 4.0)
        self.assertEqual(timeout_scale.read_timeout_scale(None, None), 1.0)
        self.assertEqual(timeout_scale.read_timeout_scale("", ""), 1.0)

        with self.assertRaises(ValueError) as cm:
            timeout_scale.read_timeout_scale(" 3", "1")
        self.assertIn('VEHICLE_SIL_TEST_TIMEOUT_SCALE=" 3" is not a number greater than 0 in digits', str(cm.exception))
        self.assertIn("or unset it", str(cm.exception))

        with self.assertRaises(ValueError) as cm_def:
            timeout_scale.read_timeout_scale(None, "x")
        self.assertIn('VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT="x" is not a number greater than 0 in digits',
                      str(cm_def.exception))
        self.assertIn("and configure the build again", str(cm_def.exception))

    def test_env_and_scaled(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(timeout_scale.timeout_scale(), 1.0)
            self.assertEqual(timeout_scale.scaled(20.0), 20.0)
        with mock.patch.dict(os.environ, {"VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT": "4"}, clear=True):
            self.assertEqual(timeout_scale.timeout_scale(), 4.0)
            self.assertEqual(timeout_scale.scaled(5.0), 20.0)
        with mock.patch.dict(
            os.environ,
            {"VEHICLE_SIL_TEST_TIMEOUT_SCALE": "2.5", "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT": "4"},
            clear=True,
        ):
            self.assertEqual(timeout_scale.timeout_scale(), 2.5)
            self.assertEqual(timeout_scale.scaled(4.0), 10.0)

    def test_serve_support_scaled_uses_factor(self) -> None:
        with mock.patch.object(serve_support, "SCALE", 3.0):
            self.assertEqual(serve_support.scaled(5.0), 15.0)


if __name__ == "__main__":
    unittest.main()
