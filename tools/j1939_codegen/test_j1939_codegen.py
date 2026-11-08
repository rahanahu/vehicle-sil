#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of j1939_codegen.py: the checks of a definition and the shape of its output.

The generated C++ itself is tested by tests/j1939/test_j1939_codec*.cpp.
Run: python3 test_j1939_codegen.py
"""

import os
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import j1939_codegen  # noqa: E402


def run(source):
    """Generates from YAML text: (exit code, header or the error message)."""
    with tempfile.TemporaryDirectory() as directory:
        definition = os.path.join(directory, "messages.yaml")
        header = os.path.join(directory, "out", "messages.hpp")
        with open(definition, "w", encoding="utf-8") as f:
            f.write(textwrap.dedent(source))
        stderr = sys.stderr
        try:
            import io
            sys.stderr = io.StringIO()
            code = j1939_codegen.main([definition, "-o", header])
            error = sys.stderr.getvalue()
        finally:
            sys.stderr = stderr
        if code != 0:
            return code, error
        with open(header, encoding="utf-8") as f:
            return code, f.read()


def message(signals="", extra="pgn: 0xFF00"):
    return "namespace: test\nmessages:\n  - name: M\n    %s\n    signals:\n%s" % (
        extra, textwrap.indent(textwrap.dedent(signals), "      "))


class Errors(unittest.TestCase):
    def assertError(self, source, *fragments):
        code, text = run(source)
        self.assertEqual(code, 1, text)
        for fragment in fragments:
            self.assertIn(fragment, text)

    def test_overlap(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 9}
            - {name: b, start_bit: 8, length: 8}
        """), "signal 'b' overlaps signal 'a' (bit 8)")

    def test_overlap_big_endian(self):
        # b: bits 7..0 of byte 0 and 15..14 of byte 1
        self.assertError(message("""
            - {name: a, start_bit: 14, length: 1}
            - {name: b, start_bit: 7, length: 10, byte_order: big}
        """), "overlaps signal 'a' (bit 14)")

    def test_selector_overlap(self):
        self.assertError(message("""
            - {name: a, start_bit: 4, length: 8}
        """, "proprietary_a: {}\n    selector: {start_bit: 0, length: 8, value: 1}"), "overlaps the selector")

    def test_outside_the_message(self):
        self.assertError(message("""
            - {name: a, start_bit: 60, length: 8}
        """), "does not fit in the message (8 bytes)")
        self.assertError(message("""
            - {name: a, start_bit: 57, length: 3, byte_order: big}
        """), "does not fit")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 65}
        """), "65 is outside 1 .. 64")

    def test_names(self):
        self.assertError(message("""
            - {name: class, start_bit: 0, length: 8}
        """), "'class' is a C++ keyword")
        self.assertError(message("""
            - {name: 2a, start_bit: 0, length: 8}
        """), "is not a C++ identifier")
        self.assertError(message("""
            - {name: _Reserved, start_bit: 0, length: 8}
        """), "reserved in C++")
        self.assertError(message("""
            - {name: encode, start_bit: 0, length: 8}
        """), "used by the generated type")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8}
            - {name: a, start_bit: 8, length: 8}
        """), "clashes")
        self.assertError(message("""
            - {name: run_state, start_bit: 0, length: 2, values: {a: 0}}
            - {name: RunState, start_bit: 8, length: 8}
        """), "clashes")
        self.assertError("namespace: a::2b\nmessages: []\n", "is not a C++ identifier")

    def test_duplicate_keys_and_unknown_keys(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, length: 9}
        """), "duplicate key 'length'")
        self.assertError(message("""
            - {name: a, start_bit: 0, lenght: 8}
        """), "'length' is missing")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scael: 2}
        """), "unknown key 'scael'")

    def test_pgn(self):
        self.assertError(message("", "pgn: 0xEF12"), "PDU1 group: its low byte must be 0")
        self.assertError(message("", "pgn: 0x40000"), "outside")
        self.assertError(message("", "pgn: 0xFF00\n    proprietary_b: 1"), "give one of")
        self.assertError(message("", "priority: 6"), "give one of")
        self.assertError(message("", "proprietary_b: 256"), "outside 0 .. 255")

    def test_values(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: 0}
        """), "must be above 0")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: -1}
        """), "must be above 0")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, min: 300}
        """), "the valid range is empty")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: abc}
        """), "expected a number")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, not_available: 256}
        """), "outside 0 .. 255")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, error: [3, 2]}
        """), "first > last")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, indicators: none, error: 3}
        """), "either 'indicators' or")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, error: [0xE0, 0xF0], not_available: [0xF0, 0xFF]}
        """), "ranges overlap")

    def test_enumerations(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 2, values: {x: 0, y: 2}}
        """), "raw value 2 is in the error range")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 2, values: {x: 0, y: 0}}
        """), "appears twice")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 2, scale: 2, values: {x: 0}}
        """), "without scale")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 2, signed: true, values: {x: 0}}
        """), "is unsigned")

    def test_generated_names(self):
        for name in ("name", "unit", "layout", "spn", "enumerators", "output_bytes", "visitor", "M"):
            self.assertError(message("""
                - {name: %s, start_bit: 0, length: 8}
            """ % name), "signal name '%s' is used by the generated type" % name)
        self.assertError("namespace: n\nmessages:\n  - {name: Signals, pgn: 0xFF00}\n", "is used by the generated type")
        self.assertError('namespace: n\nmessages:\n  - {name: "a\\n", pgn: 0xFF00}\n', "is not a C++ identifier")

    def test_yaml_numbers(self):
        self.assertError(message("""
            - {name: a, start_bit: 010, length: 8}
        """), "expected an integer, got '010'")
        self.assertError(message("""
            - {name: a, start_bit: 1:10, length: 8}
        """), "expected an integer")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: "1e400"}
        """), "does not fit in a double")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: "1e-400"}
        """), "does not fit in a double")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, offset: %s}
        """ % ("9" * 400)), "does not fit in a double")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, scale: 1:30.0}
        """), "expected a number, got '1:30.0'")
        self.assertError(message("", "proprietary_b: 0x_"), "expected an integer")
        self.assertError(message("", "proprietary_b: 0b_"), "expected an integer")
        code, header = run(message("""
            - {name: a, start_bit: 0x08, length: 0b100, values: {x: 0, y: 1_0}}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn(".start_bit = 8, .length = 4,", header)

    def test_merge_keys(self):
        source = message("""
            - &base {name: a, start_bit: 0, length: 8, scale: 2}
            - {<<: *base, name: b, start_bit: 8}
        """)
        code, header = run(source)
        self.assertEqual(code, 0, header)
        self.assertIn("struct b", header)
        self.assertError(message("""
            - &base {name: a, start_bit: 0, length: 8}
            - {<<: *base, name: b, start_bit: 8, start_bit: 9}
        """), "duplicate key 'start_bit'")

    def test_special_values_inside_the_valid_range(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, not_available: 0x80}
        """), "lie inside the valid range")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, not_available: [0, 255]}
        """), "leave no valid value")

    def test_messages_of_one_pgn(self):
        self.assertError("namespace: n\nmessages:\n  - {name: A, proprietary_b: 1}\n  - {name: B, proprietary_b: 1}\n",
                         "messages A, B share PGN 0xFF01: each needs a selector")
        two = ("namespace: n\nmessages:\n"
               "  - {name: A, proprietary_a: {}, selector: {start_bit: 0, length: 8, value: %s}}\n"
               "  - {name: B, pgn: 0xEF00, selector: {start_bit: %s, length: 8, value: 2}}\n")
        self.assertError(two % (2, 0), "their selector values must differ")
        self.assertError(two % (1, 8), "their selectors must be in the same place")
        code, header = run(two % (1, 0))
        self.assertEqual(code, 0, header)

    def test_messages(self):
        self.assertError("namespace: n\nmessages:\n  - {name: A, pgn: 0xFF00}\n  - {name: A, pgn: 0xFF01}\n",
                         "message 'A' appears twice")
        self.assertError("namespace: n\nmessages:\n  - {name: A, pgn: 0xFF00, length: 1786}\n", "outside 1 .. 1785")
        self.assertError("messages: []\n", "'namespace' is missing")
        self.assertError("namespace: n\nmessages: [\n", "while parsing")


class Output(unittest.TestCase):
    def test_on_and_off_stay_names(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 2, values: {off: 0, on: 1}}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn("enum class A : ::std::uint8_t { off = 0, on = 1 };", header)

    def test_indicators(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 16}
            - {name: b, start_bit: 16, length: 4}
            - {name: c, start_bit: 20, length: 1}
            - {name: d, start_bit: 24, length: 8, signed: true}
            - {name: e, start_bit: 32, length: 8, indicators: none}
            - {name: f, start_bit: 40, length: 16, scale: 1/8, offset: -10, min: -5, max: 100.06}
            - {name: g, start_bit: 56, length: 8, scale: 0.5, min: 1.2, max: 3.9}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFAFFull,\n", header)
        self.assertIn(".error = {0xFE00ull, 0xFEFFull}, .not_available = {0xFF00ull, 0xFFFFull}}", header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xDull,\n", header)
        self.assertIn(".error = {0xEull, 0xEull}, .not_available = {0xFull, 0xFull}}", header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0x1ull};", header)  # 1 bit: none
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF80ull, .raw_max = 0x7Full};", header)  # signed: none
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFFull};", header)  # none
        # f: (-5 + 10) * 8 = 40 .. floor(110.06 * 8) = 880
        self.assertIn(".scale = 0.125, .offset = -10.0, .raw_min = 0x28ull, .raw_max = 0x370ull,", header)
        # g: ceil(2.4) = 3 .. floor(7.8) = 7
        self.assertIn(".scale = 0.5, .offset = 0.0, .raw_min = 0x3ull, .raw_max = 0x7ull,", header)

    def test_explicit_special_values_narrow_the_valid_range(self):
        code, header = run(message("""
            - {name: u, start_bit: 0, length: 8, not_available: 0xFF, error: 0xFE}
            - {name: s, start_bit: 8, length: 8, signed: true, not_available: 0x7F}
            - {name: t, start_bit: 16, length: 8, signed: true, not_available: 0x80, error: [0x7E, 0x7F]}
            - {name: v, start_bit: 24, length: 8, not_available: [0, 9], min: 5}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFDull,\n", header)                   # u
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF80ull, .raw_max = 0x7Eull,\n", header)    # s: -128 .. 126
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF81ull, .raw_max = 0x7Dull,\n", header)    # t: -127 .. 125
        self.assertIn(".raw_min = 0xAull, .raw_max = 0xFFull,\n", header)                   # v: 10 .. 255

    def test_proprietary_pgns(self):
        for extra, pgn in (("proprietary_a: {}", "0xEF00u"), ("proprietary_a: {data_page: 1}", "0x1EF00u"),
                           ("proprietary_b: 0x42", "0xFF42u"),
                           ("proprietary_b: {group_extension: 7, data_page: 1}", "0x1FF07u"),
                           ("pgn: 0xEA00", "0xEA00u")):
            code, header = run(message("", extra))
            self.assertEqual(code, 0, header)
            self.assertIn("kPgn{%s}" % pgn, header)

    def test_text_is_escaped(self):
        code, header = run(message("""
            - name: a
              start_bit: 0
              length: 8
              unit: 'a"b\\c°'
              comment: "ends with a backslash \\\\"
        """))
        self.assertEqual(code, 0, header)
        self.assertIn(r'unit{"a\"b\\c\302\260"}', header)
        self.assertIn("/// ends with a backslash\n", header)

    def test_output_is_deterministic(self):
        source = message("""
            - {name: a, start_bit: 0, length: 16, scale: 0.05}
        """)
        self.assertEqual(run(source), run(source))


if __name__ == "__main__":
    unittest.main()
