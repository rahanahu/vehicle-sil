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


def message(signals="", extra="pgn: 0xFF00", top=""):
    """One message M with `signals`; `top` adds lines at the top of the file (indicators, ...)."""
    return "namespace: test\n%smessages:\n  - name: M\n    %s\n    signals:\n%s" % (
        textwrap.dedent(top), extra, textwrap.indent(textwrap.dedent(signals), "      "))


def specials(header, signal):
    """The special_values initializer lines of `signal` in a generated header ('' if none)."""
    start = header.index("        struct %s\n" % signal)
    end = header.index("        };", start)
    body = header[start:end]
    return body[body.index("special_values"):] if "special_values" in body else ""


class Checks(unittest.TestCase):
    def assertError(self, source, *fragments):
        code, text = run(source)
        self.assertEqual(code, 1, text)
        for fragment in fragments:
            self.assertIn(fragment, text)


class Errors(Checks):
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
        """), "special value 'not_available' (the signal's own): 256 does not fit in a 8-bit unsigned signal")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, signed: true, not_available: -129}
        """), "-129 does not fit in a 8-bit signed signal")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, error: [3, 2]}
        """), "the range is reversed as raw values")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, signed: true, error: [0x7F, 0x80]}
        """), "a signed range across the sign is two special values")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, indicators: none, error: 3}
        """), "give either 'indicators' or the signal's own special values")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, error: [0xE0, 0xF0], not_available: [0xF0, 0xFF]}
        """), "special values 'error' and 'not_available' overlap")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, special_values: {error: 1}, error: 2}
        """), "'error' is given twice")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, special_values: {Bad: 1}}
        """), "'Bad' is not a special value name")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, special_values: {x: {range: [250, 255], send: 1}}}
        """), "send 1 is outside the range")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, special_values: {x: {value: 1, range: [1, 2]}}}
        """), "give either 'value' or 'range'")

    def test_enumerations(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 2, values: {x: 0, y: 2}}
        """, top="indicators: j1939\n"), "raw value 2 is the special value 'error'")
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
        """), "special value 'not_available' (0x80, the signal's own) lies inside the valid range 0x0 .. 0xFF")
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


class Output(Checks):
    def test_on_and_off_stay_names(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 2, values: {off: 0, on: 1}}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn("enum class A : ::std::uint8_t { off = 0, on = 1 };", header)

    def test_without_a_choice_every_bit_pattern_is_a_value(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 16}
            - {name: b, start_bit: 16, length: 2, values: {x: 0, y: 2, z: 3}}
        """, "proprietary_b: 0x10"))
        self.assertEqual(code, 0, header)
        self.assertNotIn("special_values", header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFFFFull}", header)

    def test_the_j1939_profile(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 16}
            - {name: b, start_bit: 16, length: 4}
            - {name: c, start_bit: 20, length: 1}
            - {name: d, start_bit: 24, length: 8, signed: true}
            - {name: e, start_bit: 32, length: 8, indicators: none}
            - {name: f, start_bit: 40, length: 16, scale: 1/8, offset: -10, min: -5, max: 100.06}
            - {name: g, start_bit: 56, length: 2}
        """, top="indicators: j1939\n"))
        self.assertEqual(code, 0, header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFAFFull}", header)
        self.assertEqual(specials(header, "a"), 'special_values{{\n'
                         '                {"parameter_specific", 0xFB00ull, 0xFBFFull, 0xFB00ull},\n'
                         '                {"reserved", 0xFC00ull, 0xFDFFull, 0xFC00ull},\n'
                         '                {"error", 0xFE00ull, 0xFEFFull, 0xFE00ull},\n'
                         '                {"not_available", 0xFF00ull, 0xFFFFull, 0xFFFFull}}};\n')
        # No rule of the profile for 4 bits, 1 bit, signed: no special values (not guessed).
        for name in "bcd":
            self.assertEqual(specials(header, name), "", name)
        self.assertEqual(specials(header, "e"), "")
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFull}", header)                   # b
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF80ull, .raw_max = 0x7Full}", header)  # d
        # f: (-5 + 10) * 8 = 40 .. floor(110.06 * 8) = 880
        self.assertIn(".scale = 0.125, .offset = -10.0, .raw_min = 0x28ull, .raw_max = 0x370ull}", header)
        self.assertIn('{"error", 0x2ull, 0x2ull, 0x2ull},\n                {"not_available", 0x3ull, 0x3ull, 0x3ull}}};',
                      specials(header, "g"))
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0x1ull}", header)                   # g: 0, 1

    def test_min_and_max(self):
        code, header = run(message("""
            - {name: g, start_bit: 0, length: 8, scale: 0.5, min: 1.2, max: 3.9}
        """))
        self.assertEqual(code, 0, header)
        # ceil(2.4) = 3 .. floor(7.8) = 7
        self.assertIn(".scale = 0.5, .offset = 0.0, .raw_min = 0x3ull, .raw_max = 0x7ull}", header)

    def test_a_profile_chosen_for_a_signal_must_have_a_rule(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 24, indicators: j1939}
        """), "profile 'j1939' has no rule for a 24-bit unsigned signal")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, indicators: vendr}
        """), "unknown profile 'vendr' (known: j1939, none)")
        self.assertError(message("", top="indicators: nope\n"), "unknown profile 'nope'")

    def test_explicit_special_values_narrow_the_valid_range(self):
        code, header = run(message("""
            - {name: u, start_bit: 0, length: 8, not_available: 0xFF, error: 0xFE}
            - {name: s, start_bit: 8, length: 8, signed: true, not_available: 0x7F}
            - {name: t, start_bit: 16, length: 8, signed: true, not_available: -128, error: [0x7E, 0x7F]}
            - {name: v, start_bit: 24, length: 8, not_available: [0, 9], min: 5}
            - {name: w, start_bit: 32, length: 8, signed: true, special_values: {sensor_error: -1}, min: 0}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0xFDull}", header)                   # u
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF80ull, .raw_max = 0x7Eull}", header)    # s: -128 .. 126
        self.assertIn(".raw_min = 0xFFFFFFFFFFFFFF81ull, .raw_max = 0x7Dull}", header)    # t: -127 .. 125
        self.assertIn(".raw_min = 0xAull, .raw_max = 0xFFull}", header)                   # v: 10 .. 255
        self.assertIn(".raw_min = 0x0ull, .raw_max = 0x7Full}", header)                   # w: 0 .. 127
        # Values as raw values or bit patterns: -1 and 0xFF are the same signed value.
        self.assertIn('{"sensor_error", 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}',
                      specials(header, "w"))
        self.assertIn('{"not_available", 0x7Full, 0x7Full, 0x7Full}', specials(header, "s"))
        # A range sends its first value unless `send` says otherwise.
        self.assertIn('{"not_available", 0x0ull, 0x9ull, 0x0ull}', specials(header, "v"))

    def test_profiles_inheritance_and_overrides(self):
        source = """\
            namespace: test
            indicators: vendor                  # every signal of the file
            indicator_profiles:
              vendor:
                - when: {length: 8}
                  values: {sensor_error: 0xFE, not_available: 0xFF}
                - when: {length: 16, signed: false}
                  values:
                    sensor_error: [0xFE00, 0xFEFF]
                    not_available: {range: [0xFF00, 0xFFFF], send: 0xFF00}
                - when: {length: 16, signed: true}
                  values: {sensor_error: -32768}
              calibrating:
                values: {calibrating: 0, not_available: 0xFF}
            messages:
              - name: A
                proprietary_b: 1
                signals:
                  - {name: a8, start_bit: 0, length: 8}
                  - {name: a16, start_bit: 8, length: 16}
                  - {name: s16, start_bit: 24, length: 16, signed: true}
                  - {name: a4, start_bit: 40, length: 4}           # no rule: inherited, so none
                  - {name: off, start_bit: 48, length: 8, indicators: none}
                  - {name: own, start_bit: 56, length: 8, special_values: {busy: 0x80}, max: 0x7F}
              - name: B
                proprietary_b: 2
                indicators: calibrating         # the whole message, replacing the file's choice
                signals:
                  - {name: b8, start_bit: 0, length: 8}
                  - {name: v8, start_bit: 8, length: 8, indicators: vendor}
                  - {name: j8, start_bit: 16, length: 8, indicators: j1939}
        """
        code, header = run(source)
        self.assertEqual(code, 0, header)
        self.assertIn('{"sensor_error", 0xFEull, 0xFEull, 0xFEull},\n                {"not_available", 0xFFull, 0xFFull, 0xFFull}}};',
                      specials(header, "a8"))
        self.assertIn('{"not_available", 0xFF00ull, 0xFFFFull, 0xFF00ull}', specials(header, "a16"))
        self.assertIn('{"sensor_error", 0xFFFFFFFFFFFF8000ull, 0xFFFFFFFFFFFF8000ull, 0xFFFFFFFFFFFF8000ull}}};',
                      specials(header, "s16"))
        self.assertEqual(specials(header, "a4"), "")
        self.assertEqual(specials(header, "off"), "")
        self.assertIn('{"busy", 0x80ull, 0x80ull, 0x80ull}}};', specials(header, "own"))  # replaces, no merge
        self.assertNotIn("sensor_error", specials(header, "own"))
        self.assertIn('{"calibrating", 0x0ull, 0x0ull, 0x0ull}', specials(header, "b8"))
        self.assertNotIn("sensor_error", specials(header, "b8"))
        self.assertIn("sensor_error", specials(header, "v8"))
        self.assertIn("parameter_specific", specials(header, "j8"))

    def test_profile_names_are_strings(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, indicators: [j1939]}
        """), "expected a profile name, got ['j1939']")
        self.assertError(message("", top="indicators: {}\n"), "expected a profile name, got {}")
        self.assertError(message("", "proprietary_b: 1\n    indicators: 1"), "expected a profile name, got 1")

    def test_a_profile_of_a_file_or_message_must_fit_some_signal(self):
        # A mistyped `when` would otherwise leave every signal without special values.
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8}
            - {name: b, start_bit: 8, length: 16}
        """, top="indicators: v\nindicator_profiles: {v: {when: {signed: true}, values: {x: -128}}}\n"),
            "profile 'v' of the file has no rule for any of its signals (16-bit unsigned, 8-bit unsigned): check its 'when'")
        own = "indicator_profiles: {v: {when: {length: 8, signed: false}, values: {x: 0}}}\n"
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 4}
        """, "proprietary_b: 1\n    indicators: v", top=own),
            "profile 'v' of the message has no rule for any of its signals (4-bit unsigned)")
        # One signal with a rule is enough; signals with a choice of their own do not count.
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 4}
            - {name: b, start_bit: 8, length: 8}
        """, top="indicators: v\n" + own))
        self.assertEqual(code, 0, header)
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 4}
            - {name: b, start_bit: 8, length: 8, indicators: none}
        """, top="indicators: v\n" + own), "profile 'v' of the file has no rule for any of its signals (4-bit unsigned)")
        # The built-in j1939 has no `when` for the user to get wrong: a file of flags only is fine.
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 1}
            - {name: b, start_bit: 1, length: 4}
        """, "proprietary_b: 1\n    indicators: j1939", top="indicators: j1939\n"))
        self.assertEqual(code, 0, header)
        # A file's own j1939 is checked like any profile of the file.
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 4}
        """, top="indicators: j1939\nindicator_profiles: {j1939: {when: {length: 24}, values: {error: 0xFE0000}}}\n"),
            "profile 'j1939' of the file has no rule for any of its signals")

    def test_a_rule_for_several_lengths(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 8}
            - {name: b, start_bit: 8, length: 16}
            - {name: c, start_bit: 24, length: 4}
        """, top="indicators: v\nindicator_profiles: {v: {when: {length: [8, 16]}, values: {x: 0}}}\n"))
        self.assertEqual(code, 0, header)
        self.assertIn('"x"', specials(header, "a"))
        self.assertIn('"x"', specials(header, "b"))
        self.assertEqual(specials(header, "c"), "")

    def test_the_first_matching_rule_wins(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 8}
            - {name: b, start_bit: 8, length: 4}
        """, top="""\
            indicators: p
            indicator_profiles:
              p:
                - when: {length: 8}
                  values: {first: 0xFF}
                - values: {fallback: 0}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn('"first"', specials(header, "a"))
        self.assertNotIn('"fallback"', specials(header, "a"))
        self.assertIn('"fallback"', specials(header, "b"))

    def test_a_file_may_replace_the_j1939_profile(self):
        code, header = run(message("""
            - {name: a, start_bit: 0, length: 24}
        """, top="""\
            indicators: j1939
            indicator_profiles:
              j1939:
                - when: {length: 24, signed: false}
                  values: {error: [0xFE0000, 0xFEFFFF], not_available: [0xFF0000, 0xFFFFFF]}
        """))
        self.assertEqual(code, 0, header)
        self.assertIn('{"error", 0xFE0000ull, 0xFEFFFFull, 0xFE0000ull}', specials(header, "a"))
        self.assertError(message("", top="indicator_profiles: {none: {values: {}}}\n"), "'none' cannot be redefined")

    def test_profile_errors(self):
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 4}
        """, top="indicators: v\nindicator_profiles: {v: {values: {x: 0xFF}}}\n"),
            "special value 'x' (profile v of the file): 255 does not fit in a 4-bit unsigned signal")
        self.assertError(message("", top="indicator_profiles: {v: {values: {x: 1}, wen: {}}}\n"), "unknown key 'wen'")
        self.assertError(message("", top="indicator_profiles: {v: {values: {x: 1}, when: {length: 65}}}\n"),
                         "65 is outside 1 .. 64")
        self.assertError(message("", top="indicator_profiles: {v: {values: {x: 1}, when: {signed: 1}}}\n"),
                         "expected true or false")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, min: 0}
        """, top="indicators: v\nindicator_profiles: {v: {values: {x: 0x80}}}\n"),
            "special value 'x' (0x80, profile v of the file) lies inside the valid range 0x0 .. 0xFF")
        self.assertError(message("""
            - {name: a, start_bit: 0, length: 8, signed: true, special_values: {x: 5}}
        """), "special value 'x' (0x5, the signal's own) lies inside the valid range -128 (0x80) .. 0x7F")
        self.assertError(message("", top="indicator_profiles: {v: {values: {x: 1}, when: {length: []}}}\n"),
                         "give at least one length")

    def test_fill(self):
        code, header = run("namespace: n\nfill: 0x00\nmessages:\n"
                           "  - {name: A, proprietary_b: 1}\n"
                           "  - {name: B, proprietary_b: 2, fill: 0xAA}\n"
                           "  - {name: C, proprietary_b: 3, length: 3, fill: [0xFF, 0x00, 0x55]}\n")
        self.assertEqual(code, 0, header)
        self.assertIn("kFill = 0x00;", header)
        self.assertIn("kFill = 0xAA;", header)
        self.assertIn("kFillBytes{0xFF, 0x00, 0x55};", header)
        self.assertError("namespace: n\nmessages:\n  - {name: C, proprietary_b: 3, length: 3, fill: [1, 2]}\n",
                         "a list of 3 bytes (the message's length), got 2")
        self.assertError("namespace: n\nfill: [1]\nmessages: []\n", "the default fill is one byte")
        self.assertError("namespace: n\nfill: 256\nmessages: []\n", "outside 0 .. 255")

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
