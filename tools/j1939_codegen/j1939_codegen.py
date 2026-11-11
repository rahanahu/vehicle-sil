#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates typed J1939 message types (a C++ header) from a YAML definition.

Usage: j1939_codegen.py <definition.yaml> -o <header.hpp>

The format is described in docs/j1939-codec.md. The header includes only
vehicle_sil/j1939/codec.hpp. Needs Python 3.11+ and PyYAML.
"""

import argparse
import math
import os
import re
import sys
from fractions import Fraction

try:
    import yaml
except ImportError:  # pragma: no cover - reported to the build
    sys.stderr.write("j1939_codegen: PyYAML is required (pip install pyyaml)\n")
    sys.exit(2)

MAX_LENGTH = 1785  # the longest message of the transport protocol

CPP_KEYWORDS = set("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
char32_t class compl concept const consteval constexpr constinit const_cast continue co_await
co_return co_yield decltype default delete do double dynamic_cast else enum explicit export
extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq
nullptr operator or or_eq private protected public register reinterpret_cast requires return
short signed sizeof static static_assert static_cast struct switch template this thread_local
throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while
xor xor_eq final override import module
""".split())

# The members of a signal's description (Signals::<signal name>): a signal of the same name would
# be a member with the name of its class.
SPEC_MEMBERS = {"layout", "name", "unit", "spn", "enumerators", "special_values"}

# Names the generated message type uses itself (members and the parameters of its functions).
MESSAGE_MEMBERS = {"kPgn", "kPriority", "kLength", "kFill", "kFillBytes", "Selector", "Signals", "encode", "decode",
                   "bytes", "for_each_signal", "output_bytes", "input_bytes", "visitor", "Visitor"}

# Names of the generated namespaces and types. Generated code spells std and vehicle_sil from the
# global namespace, so the user's names do not hide them.
CODEC = "::vehicle_sil::j1939::codec"
J1939 = "::vehicle_sil::j1939"


class DefinitionError(Exception):
    pass


# ---------------------------------------------------------------------------
# YAML: "on", "off", "yes" and "no" stay strings (YAML 1.1 would make them booleans, which breaks
# enumerator names); integers are decimal (no leading 0: YAML 1.1 reads 010 as octal 8),
# hexadecimal (0x) or binary (0b), and 1:10 or 1:30.0 (base 60 in YAML 1.1) is not a number; a key may appear
# only once in a mapping (a key merged with << may be overridden).
# ---------------------------------------------------------------------------
class _Loader(yaml.SafeLoader):
    pass


_Loader.yaml_implicit_resolvers = {
    first: [(tag, regexp) for tag, regexp in resolvers
            if tag not in ("tag:yaml.org,2002:bool", "tag:yaml.org,2002:int", "tag:yaml.org,2002:float")]
    for first, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
_Loader.add_implicit_resolver("tag:yaml.org,2002:bool", re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"),
                              list("tTfF"))
_INTEGER = re.compile(r"^(?:[-+]?(?:0|[1-9][0-9_]*)|0x[0-9a-fA-F][0-9a-fA-F_]*|0b[01][01_]*)$")
_Loader.add_implicit_resolver("tag:yaml.org,2002:int", _INTEGER, list("-+0123456789"))
# The floats of YAML 1.1 without the base 60 form.
_FLOAT = re.compile(r"""^(?:[-+]?(?:[0-9][0-9_]*)\.[0-9_]*(?:[eE][-+][0-9]+)?
                    |\.[0-9][0-9_]*(?:[eE][-+][0-9]+)?
                    |[-+]?\.(?:inf|Inf|INF)
                    |\.(?:nan|NaN|NAN))$""", re.X)
_Loader.add_implicit_resolver("tag:yaml.org,2002:float", _FLOAT, list("-+0123456789."))


def _mapping(loader, node):
    own = {id(key_node) for key_node, _ in node.value if key_node.tag != "tag:yaml.org,2002:merge"}
    loader.flatten_mapping(node)
    result = {}
    written = set()  # keys written in this mapping itself (not merged)
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        mine = id(key_node) in own
        if key in result and (key in written or not mine):
            raise DefinitionError("line %d: duplicate key '%s'" % (key_node.start_mark.line + 1, key))
        result[key] = loader.construct_object(value_node, deep=True)
        if mine:
            written.add(key)
    return result


_Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def fail(where, text):
    raise DefinitionError("%s: %s" % (where, text))


def check_keys(where, mapping, required, optional):
    if not isinstance(mapping, dict):
        fail(where, "expected a mapping")
    for key in required:
        if key not in mapping:
            fail(where, "'%s' is missing" % key)
    for key in mapping:
        if key not in required and key not in optional:
            fail(where, "unknown key '%s'" % key)


def identifier(where, value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        fail(where, "'%s' is not a C++ identifier" % (value,))
    if value in CPP_KEYWORDS:
        fail(where, "'%s' is a C++ keyword" % value)
    if value.startswith("__") or re.match(r"^_[A-Z]", value) or "__" in value:
        fail(where, "'%s' is reserved in C++" % value)
    return value


def integer(where, value, low, high):
    if isinstance(value, bool) or not isinstance(value, int):
        fail(where, "expected an integer, got %r" % (value,))
    if not low <= value <= high:
        fail(where, "%d is outside %d .. %d" % (value, low, high))
    return value


def fits_double(where, value, text_):
    try:
        as_double = float(value)
    except OverflowError:
        as_double = math.inf
    if not math.isfinite(as_double) or (value != 0 and as_double == 0):
        fail(where, "%s does not fit in a double" % text_)
    return value


def number(where, value):
    """An exact number: an int, a float, or a string such as '1/8' or '1e-3'."""
    if isinstance(value, bool):
        fail(where, "expected a number, got %r" % (value,))
    if isinstance(value, int):
        return fits_double(where, Fraction(value), value)
    if isinstance(value, float):
        if not math.isfinite(value):
            fail(where, "expected a finite number")
        return Fraction(repr(value))
    if isinstance(value, str):
        try:
            result = Fraction(value.strip())
        except (ValueError, ZeroDivisionError):
            result = None
        if result is not None:
            return fits_double(where, result, value)
    fail(where, "expected a number, got %r" % (value,))


def text(where, value):
    if not isinstance(value, str):
        fail(where, "expected a string, got %r" % (value,))
    return value


def positions(layout):
    """The absolute bit positions of a signal (start_bit, length, big_endian)."""
    start, length, big = layout
    if not big:
        return [start + i for i in range(length)]
    result = []
    position = start
    for _ in range(length):
        result.append(position)
        position = position - 1 if position % 8 else position + 15
    return result


# ---------------------------------------------------------------------------
# Special values and their profiles
#
# A profile is a list of rules; a rule has an optional `when` (length: a number or a list, signed:
# true/false) and `values` (name: value, [first, last] or {range|value, send}). The first rule
# that matches the signal gives its special values. Values are raw values: a negative number for a
# signed signal, or its bit pattern (0xFF is -1 in 8 signed bits).
# ---------------------------------------------------------------------------
SPECIAL_NAME = re.compile(r"[a-z_][a-z0-9_]*")
BUILTIN_NONE = "none"


def _j1939_bytes_rule(length):
    rest = length - 8
    low = (1 << rest) - 1
    return {"when": {"length": length, "signed": False},
            "values": {"parameter_specific": [0xFB << rest, (0xFB << rest) | low],
                       "reserved": [0xFC << rest, (0xFD << rest) | low],
                       "error": [0xFE << rest, (0xFE << rest) | low],
                       "not_available": {"range": [0xFF << rest, (1 << length) - 1], "send": (1 << length) - 1}}}


# The built-in j1939 profile: the widely published values only, not checked against the standard
# (docs/j1939-codec.md). A definition file may replace it with its own `j1939` profile.
BUILTIN_PROFILES = {
    "j1939": [_j1939_bytes_rule(8), _j1939_bytes_rule(16), _j1939_bytes_rule(32),
              {"when": {"length": 2, "signed": False}, "values": {"error": 2, "not_available": 3}}],
}


class Rule:
    pass


def parse_special_values(where, values):
    """name -> (first, last, send or None) as written (not yet fitted to a signal)."""
    if not isinstance(values, dict):
        fail(where, "expected a mapping of names to values")
    result = {}
    for name, value in values.items():
        if not isinstance(name, str) or not SPECIAL_NAME.fullmatch(name):
            fail(where, "'%s' is not a special value name (lowercase letters, digits, _)" % (name,))
        w = "%s.%s" % (where, name)
        send = None
        if isinstance(value, dict):
            check_keys(w, value, [], ["value", "range", "send"])
            if ("value" in value) == ("range" in value):
                fail(w, "give either 'value' or 'range'")
            if "send" in value:
                send = integer(w + ".send", value["send"], -(1 << 64), (1 << 64) - 1)
            value = value.get("value", value.get("range"))
        if isinstance(value, list):
            if len(value) != 2:
                fail(w, "expected a value or [first, last]")
            first = integer(w, value[0], -(1 << 64), (1 << 64) - 1)
            last = integer(w, value[1], -(1 << 64), (1 << 64) - 1)
        else:
            first = last = integer(w, value, -(1 << 64), (1 << 64) - 1)
        result[name] = (first, last, send)
    return result


def parse_profile(where, profile):
    rules_in = profile if isinstance(profile, list) else [profile]
    rules = []
    for i, r in enumerate(rules_in):
        w = "%s[%d]" % (where, i) if isinstance(profile, list) else where
        check_keys(w, r, ["values"], ["when"])
        rule = Rule()
        rule.lengths = None
        rule.signed = None
        when = r.get("when") or {}
        check_keys(w + ".when", when, [], ["length", "signed"])
        if "length" in when:
            lengths = when["length"] if isinstance(when["length"], list) else [when["length"]]
            if not lengths:
                fail(w + ".when.length", "give at least one length")
            rule.lengths = {integer(w + ".when.length", n, 1, 64) for n in lengths}
        if "signed" in when:
            if not isinstance(when["signed"], bool):
                fail(w + ".when.signed", "expected true or false")
            rule.signed = when["signed"]
        rule.values = parse_special_values(w + ".values", r["values"] or {})
        rules.append(rule)
    return rules


def parse_profiles(where, document):
    """The profiles (built-in and the file's) and the names the file defines itself."""
    profiles = {name: parse_profile("built-in profile %s" % name, rules) for name, rules in BUILTIN_PROFILES.items()}
    defined = document.get("indicator_profiles") or {}
    if not isinstance(defined, dict):
        fail(where + ".indicator_profiles", "expected a mapping of names to profiles")
    for name, profile in defined.items():
        w = "%s.indicator_profiles.%s" % (where, name)
        identifier(w, name)
        if name == BUILTIN_NONE:
            fail(w, "'none' cannot be redefined")
        profiles[name] = parse_profile(w, profile)  # a file's own j1939 replaces the built-in one
    return profiles, set(defined)


def profile_name(where, value, profiles):
    if not isinstance(value, str):
        fail(where, "expected a profile name, got %r" % (value,))
    if value != BUILTIN_NONE and value not in profiles:
        known = ", ".join(sorted(list(profiles) + [BUILTIN_NONE]))
        fail(where, "unknown profile '%s' (known: %s)" % (value, known))
    return value


def raw_value(where, value, length, signed):
    """A raw value or a bit pattern as the signal's raw value."""
    if signed:
        low, high = -(1 << (length - 1)), (1 << (length - 1)) - 1
        if low <= value <= high:
            return value
        if high < value <= (1 << length) - 1:
            return value - (1 << length)  # the bit pattern of a negative value
        fail(where, "%d does not fit in a %d-bit signed signal" % (value, length))
    if not 0 <= value <= (1 << length) - 1:
        fail(where, "%d does not fit in a %d-bit unsigned signal" % (value, length))
    return value


def fit_special_values(where, values, length, signed, source):
    """[(name, first, last, send)] as raw values of the signal, checked. `source` tells where the
    values come from (for the messages)."""
    result = []
    for name, (first, last, send) in values.items():
        w = "%s: special value '%s' (%s)" % (where, name, source)
        first_raw = raw_value(w, first, length, signed)
        last_raw = raw_value(w, last, length, signed)
        if first_raw > last_raw:
            fail(w, "the range is reversed as raw values (%d > %d; a signed range across the sign is two "
                    "special values)" % (first_raw, last_raw))
        if send is None:
            send_raw = first_raw
        else:
            send_raw = raw_value(w + " send", send, length, signed)
            if not first_raw <= send_raw <= last_raw:
                fail(w, "send %d is outside the range" % send)
        result.append((name, first_raw, last_raw, send_raw))
    ordered = sorted(result, key=lambda v: v[1])
    for before, after in zip(ordered, ordered[1:]):
        if after[1] <= before[2]:
            fail(where, "special values '%s' and '%s' overlap (%s)" % (before[0], after[0], source))
    return result


def matching_rule(rules, length, signed):
    for rule in rules:
        if (rule.lengths is None or length in rule.lengths) and (rule.signed is None or rule.signed == signed):
            return rule
    return None


def hex_raw(value, length):
    """A raw value for a message: hexadecimal, and a negative one also in decimal (its bit pattern
    in parentheses), so that a signed range does not look reversed."""
    pattern = "0x%X" % (value & ((1 << length) - 1))
    return "%d (%s)" % (value, pattern) if value < 0 else pattern


def without_special_values(where, low, high, specials, source, length):
    """Narrows the valid range low .. high so that it holds no special value: the special values may
    sit at its ends, not inside it."""
    changed = True
    while changed:  # trims from the ends while one of them is special
        changed = False
        for _, first, last, _ in specials:
            if low > high or last < low or first > high:
                continue
            if last >= high:
                high, changed = first - 1, True
            elif first <= low:
                low, changed = last + 1, True
    if low > high:
        fail(where, "the special values (%s) leave no valid value (min, max)" % source)
    for name, first, last, _ in specials:
        if first <= high and last >= low:
            shown = hex_raw(first, length) if first == last else "%s .. %s" % (hex_raw(first, length), hex_raw(last, length))
            fail(where, "special value '%s' (%s, %s) lies inside the valid range %s .. %s: set 'min' and 'max' "
                 "around it" % (name, shown, source, hex_raw(low, length), hex_raw(high, length)))
    return low, high


# ---------------------------------------------------------------------------
# The definition
# ---------------------------------------------------------------------------
class Signal:
    pass


class Message:
    pass


def pascal(name):
    return "".join(part[:1].upper() + part[1:] for part in name.split("_") if part)


def parse_layout(where, s, message_bits):
    start = integer(where + ".start_bit", s["start_bit"], 0, message_bits - 1)
    length = integer(where + ".length", s["length"], 1, 64)
    order = s.get("byte_order", "little")
    if order not in ("little", "big"):
        fail(where + ".byte_order", "expected 'little' or 'big'")
    layout = (start, length, order == "big")
    bits = positions(layout)
    if max(bits) >= message_bits:
        fail(where, "does not fit in the message (%d bytes)" % (message_bits // 8))
    return layout, bits


def special_values_of(where, s, inherited, profiles, length, signed):
    """(special values, where they come from, whether an inherited profile had a rule for the
    signal): the signal's own (special_values, not_available, error), or the profile it names, or
    the inherited profile (message, then file). No merging: the nearest choice replaces the others.
    `inherited` is (profile name, where it was chosen)."""
    own = {key: s[key] for key in ("special_values", "not_available", "error") if key in s}
    if own:
        if "indicators" in s:
            fail(where, "give either 'indicators' or the signal's own special values")
        values = parse_special_values(where + ".special_values", own.get("special_values") or {})
        for key in ("not_available", "error"):
            if key in own:
                if key in values:
                    fail(where, "'%s' is given twice" % key)
                values.update(parse_special_values(where, {key: own[key]}))
        source = "the signal's own"
        return fit_special_values(where, values, length, signed, source), source, False
    if "indicators" in s:
        name = profile_name(where + ".indicators", s["indicators"], profiles)
        explicit, source = True, "profile %s of the signal" % name
    else:
        name, chosen_at = inherited
        explicit, source = False, "profile %s of %s" % (name, chosen_at)
    if name == BUILTIN_NONE:
        return [], source, False
    rule = matching_rule(profiles[name], length, signed)
    if rule is None:
        if explicit:
            fail(where, "profile '%s' has no rule for a %d-bit %s signal" % (name, length,
                                                                          "signed" if signed else "unsigned"))
        return [], source, False  # an inherited profile leaves the signals it has no rule for alone
    return fit_special_values(where, rule.values, length, signed, source), source, not explicit


def parse_signal(where, s, inherited, profiles, message_bits):
    check_keys(where, s, ["name", "start_bit", "length"],
               ["byte_order", "signed", "scale", "offset", "min", "max", "unit", "indicators",
                "special_values", "not_available", "error", "spn", "values", "comment"])
    sig = Signal()
    sig.name = identifier(where + ".name", s["name"])
    where = "%s (%s)" % (where, sig.name)
    sig.layout, sig.bits = parse_layout(where, s, message_bits)
    length = sig.layout[1]
    sig.signed = s.get("signed", False)
    if not isinstance(sig.signed, bool):
        fail(where + ".signed", "expected true or false")
    sig.scale = number(where + ".scale", s.get("scale", 1))
    sig.offset = number(where + ".offset", s.get("offset", 0))
    if sig.scale <= 0:
        fail(where + ".scale", "must be above 0")
    sig.unit = text(where + ".unit", s.get("unit", ""))
    sig.comment = text(where + ".comment", s.get("comment", ""))
    sig.spn = integer(where + ".spn", s["spn"], 0, (1 << 32) - 1) if "spn" in s else None

    sig.specials, source, sig.inherited_match = special_values_of(where, s, inherited, profiles, length, sig.signed)
    sig.inherits = not any(key in s for key in ("indicators", "special_values", "not_available", "error"))

    # The valid range of the raw value.
    top = (1 << length) - 1
    if sig.signed:
        low, high = -(1 << (length - 1)), (1 << (length - 1)) - 1
    else:
        low, high = 0, top
    sig.enumerators = None
    if "values" in s:
        values = s["values"]
        if not isinstance(values, dict) or not values:
            fail(where + ".values", "expected a mapping of names to raw values")
        if sig.signed or "scale" in s or "offset" in s or "min" in s or "max" in s or "unit" in s:
            fail(where, "an enumerated signal ('values') is unsigned, without scale, offset, min, max or unit")
        sig.enumerators = []
        seen = set()
        for key, raw in values.items():
            name = identifier(where + ".values", key)
            raw = integer("%s.values.%s" % (where, name), raw, 0, top)
            if raw in seen:
                fail(where + ".values", "raw value %d appears twice" % raw)
            for special, first, last, _ in sig.specials:
                if first <= raw <= last:
                    fail("%s.values.%s" % (where, name), "raw value %d is the special value '%s'" % (raw, special))
            seen.add(raw)
            sig.enumerators.append((name, raw))
        low, high = min(seen), max(seen)
    else:
        if "min" in s:
            low = max(low, math.ceil((number(where + ".min", s["min"]) - sig.offset) / sig.scale))
        if "max" in s:
            high = min(high, math.floor((number(where + ".max", s["max"]) - sig.offset) / sig.scale))
        if low > high:
            fail(where, "the valid range is empty (min, max, special values)")
        low, high = without_special_values(where, low, high, sig.specials, source, length)
    sig.raw_min, sig.raw_max = low, high
    return sig


def parse_pgn(where, m):
    kinds = [k for k in ("pgn", "proprietary_a", "proprietary_b") if k in m]
    if len(kinds) != 1:
        fail(where, "give one of 'pgn', 'proprietary_a', 'proprietary_b'")
    kind = kinds[0]
    if kind == "pgn":
        pgn = integer(where + ".pgn", m["pgn"], 0, 0x3FFFF)
        if (pgn >> 8) & 0xFF < 0xF0 and pgn & 0xFF:
            fail(where + ".pgn", "0x%X is a PDU1 group: its low byte must be 0" % pgn)
        return pgn
    value = m[kind]
    if kind == "proprietary_a":
        # proprietary_a: {} or {data_page: 1}
        if value is None:
            value = {}
        check_keys(where + ".proprietary_a", value, [], ["data_page"])
        page = integer(where + ".proprietary_a.data_page", value.get("data_page", 0), 0, 1)
        return (page << 16) | 0xEF00
    # proprietary_b: <group extension> or {group_extension: n, data_page: 1}
    if isinstance(value, dict):
        check_keys(where + ".proprietary_b", value, ["group_extension"], ["data_page"])
        extension = integer(where + ".proprietary_b.group_extension", value["group_extension"], 0, 255)
        page = integer(where + ".proprietary_b.data_page", value.get("data_page", 0), 0, 1)
    else:
        extension = integer(where + ".proprietary_b", value, 0, 255)
        page = 0
    return (page << 16) | 0xFF00 | extension


def parse_fill(where, value, length):
    """One byte for every byte, or the message's bytes: a tuple of `length` bytes."""
    if isinstance(value, list):
        if len(value) != length:
            fail(where, "a list of %d bytes (the message's length), got %d" % (length, len(value)))
        return tuple(integer(where, b, 0, 255) for b in value)
    return (integer(where, value, 0, 255),) * length


def parse_message(where, m, defaults, profiles, defined):
    check_keys(where, m, ["name"], ["pgn", "proprietary_a", "proprietary_b", "priority", "length", "fill",
                                    "indicators", "selector", "signals", "comment"])
    msg = Message()
    msg.name = identifier(where + ".name", m["name"])
    if msg.name in MESSAGE_MEMBERS:
        fail(where + ".name", "'%s' is used by the generated type" % msg.name)
    where = "%s (%s)" % (where, msg.name)
    msg.pgn = parse_pgn(where, m)
    msg.priority = integer(where + ".priority", m.get("priority", 6), 0, 7)
    msg.length = integer(where + ".length", m.get("length", 8), 1, MAX_LENGTH)
    msg.fill = parse_fill(where + ".fill", m["fill"] if "fill" in m else defaults["fill"], msg.length)
    if "indicators" in m:
        indicators = (profile_name(where + ".indicators", m["indicators"], profiles), "the message")
    else:
        indicators = defaults["indicators"]
    msg.own_indicators = "indicators" in m
    msg.comment = text(where + ".comment", m.get("comment", ""))
    bits = msg.length * 8
    used = {}

    def claim(owner, positions_):
        for p in positions_:
            if p in used:
                fail(where, "%s overlaps %s (bit %d)" % (owner, used[p], p))
            used[p] = owner

    msg.selector = None
    if "selector" in m:
        sw = where + ".selector"
        check_keys(sw, m["selector"], ["start_bit", "length", "value"], ["byte_order"])
        layout, positions_ = parse_layout(sw, m["selector"], bits)
        value = integer(sw + ".value", m["selector"]["value"], 0, (1 << layout[1]) - 1)
        msg.selector = (layout, value)
        claim("the selector", positions_)

    signals = m.get("signals") or []
    if not isinstance(signals, list):
        fail(where + ".signals", "expected a list")
    msg.signals = []
    names = {}
    for i, s in enumerate(signals):
        sig = parse_signal("%s.signals[%d]" % (where, i), s, indicators, profiles, bits)
        if sig.name in MESSAGE_MEMBERS or sig.name in SPEC_MEMBERS or sig.name == msg.name:
            fail(where, "signal name '%s' is used by the generated type" % sig.name)
        claim("signal '%s'" % sig.name, sig.bits)
        names_here = [(sig.name, "signal")]
        if sig.enumerators:
            sig.enum_type = pascal(sig.name)
            if sig.enum_type in MESSAGE_MEMBERS or sig.enum_type == msg.name:
                fail(where, "the enum type '%s' of signal '%s' clashes with a generated name"
                     % (sig.enum_type, sig.name))
            names_here.append((sig.enum_type, "enum type"))
        for name, kind in names_here:
            if name in names:
                fail(where, "the %s '%s' clashes with the %s of the same name" % (kind, name, names[name]))
            names[name] = kind
        msg.signals.append(sig)
    if msg.own_indicators:
        check_profile_used(where, indicators, "message", [sig for sig in msg.signals if sig.inherits], defined)
    return msg


def check_profile_used(where, chosen, scope, inheriting, defined):
    """A profile of the file's own, chosen for a whole file or message, that has a rule for none of
    the signals that inherit it is most likely a mistake in its `when`: say so instead of leaving
    them all without special values. (The built-in j1939 leaves the lengths it has no rule for
    alone: a file of flags only is no mistake.)"""
    name = chosen[0]
    if name not in defined or not inheriting or any(sig.inherited_match for sig in inheriting):
        return
    kinds = sorted({"%d-bit %s" % (sig.layout[1], "signed" if sig.signed else "unsigned") for sig in inheriting})
    fail(where, "profile '%s' of the %s has no rule for any of its signals (%s): check its 'when'"
         % (name, scope, ", ".join(kinds)))


def parse(document, where):
    if not isinstance(document, dict):
        fail(where, "expected a mapping with 'namespace' and 'messages'")
    check_keys(where, document, ["namespace", "messages"], ["indicators", "indicator_profiles", "fill"])
    namespace = text(where + ".namespace", document["namespace"])
    for part in namespace.split("::"):
        identifier(where + ".namespace", part)
    profiles, defined = parse_profiles(where, document)
    # Without a choice, no special values: the J1939 ones are chosen, never assumed.
    defaults = {"indicators": (profile_name(where + ".indicators", document.get("indicators", BUILTIN_NONE), profiles),
                               "the file"),
                "fill": document.get("fill", 0xFF)}
    if isinstance(defaults["fill"], list):
        fail(where + ".fill", "the default fill is one byte (a list of bytes belongs to a message)")
    integer(where + ".fill", defaults["fill"], 0, 255)
    messages = document["messages"]
    if not isinstance(messages, list):
        fail(where + ".messages", "expected a list")
    result = []
    seen = set()
    for i, m in enumerate(messages):
        msg = parse_message("%s.messages[%d]" % (where, i), m, defaults, profiles, defined)
        if msg.name in seen:
            fail(where, "message '%s' appears twice" % msg.name)
        seen.add(msg.name)
        result.append(msg)
    check_profile_used(where, defaults["indicators"], "file",
                       [sig for msg in result if not msg.own_indicators for sig in msg.signals if sig.inherits], defined)
    check_same_pgn(where, result)
    return namespace, result


def check_same_pgn(where, messages):
    """Messages of one PGN must be told apart: all with a selector in the same place, with
    different values. Otherwise one frame would decode as several types."""
    by_pgn = {}
    for msg in messages:
        by_pgn.setdefault(msg.pgn, []).append(msg)
    for pgn, group in by_pgn.items():
        if len(group) < 2:
            continue
        names = ", ".join(msg.name for msg in group)
        if any(msg.selector is None for msg in group):
            fail(where, "messages %s share PGN 0x%X: each needs a selector" % (names, pgn))
        if len({msg.selector[0] for msg in group}) != 1:
            fail(where, "messages %s share PGN 0x%X: their selectors must be in the same place" % (names, pgn))
        values = [msg.selector[1] for msg in group]
        if len(set(values)) != len(values):
            fail(where, "messages %s share PGN 0x%X: their selector values must differ" % (names, pgn))


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def cpp_string(value):
    out = []
    for byte in value.encode("utf-8"):
        c = chr(byte)
        if c in "\\\"":
            out.append("\\" + c)
        elif 0x20 <= byte < 0x7F and c != "?":
            out.append(c)
        else:
            out.append("\\%03o" % byte)  # octal: never swallows the next character
    return '"' + "".join(out) + '"'


def cpp_double(value):
    text_ = repr(float(value))
    if not math.isfinite(float(value)):
        raise DefinitionError("a value is too large for a double: %s" % value)
    return text_


def cpp_u64(value):
    return "0x%Xull" % (value & 0xFFFFFFFFFFFFFFFF)


def comment_lines(value, indent):
    lines = []
    for line in value.strip().splitlines():
        line = line.rstrip().rstrip("\\").rstrip()  # a trailing backslash would continue the comment
        lines.append("%s/// %s" % (indent, line) if line else "%s///" % indent)
    return lines


def layout_initializer(layout, signed=False, scale=Fraction(1), offset=Fraction(0), raw_min=0, raw_max=0):
    start, length, big = layout
    parts = [".start_bit = %d" % start, ".length = %d" % length,
             ".byte_order = %s::ByteOrder::%s" % (CODEC, "BigEndian" if big else "LittleEndian"),
             ".is_signed = %s" % ("true" if signed else "false"),
             ".scale = %s" % cpp_double(scale), ".offset = %s" % cpp_double(offset),
             ".raw_min = %s" % cpp_u64(raw_min), ".raw_max = %s" % cpp_u64(raw_max)]
    lines = [", ".join(parts[:3]), ", ".join(parts[3:])]
    return "{\n" + ",\n".join(" " * 16 + line for line in lines) + "}"


def enum_underlying(enumerators):
    top = max(raw for _, raw in enumerators)
    for bits in (8, 16, 32):
        if top < (1 << bits):
            return "::std::uint%d_t" % bits
    return "::std::uint64_t"


def cpp_unsigned(value):
    return "%dull" % value if value >= 1 << 63 else "%d" % value


def emit_message(msg):
    out = []
    out.extend(comment_lines(msg.comment, ""))
    out.append("struct %s" % msg.name)
    out.append("{")
    out.append("    static constexpr %s::Pgn kPgn{0x%Xu};" % (J1939, msg.pgn))
    out.append("    static constexpr %s::Priority kPriority = %d;" % (J1939, msg.priority))
    out.append("    static constexpr ::std::size_t kLength = %d;" % msg.length)
    if len(set(msg.fill)) == 1:
        out.append("    static constexpr ::std::uint8_t kFill = 0x%02X;" % msg.fill[0])
    else:
        out.append("    static constexpr ::std::array<::std::uint8_t, %d> kFillBytes{%s};"
                   % (msg.length, ", ".join("0x%02X" % b for b in msg.fill)))
    for sig in msg.signals:
        if sig.enumerators:
            out.append("")
            out.extend(comment_lines(sig.comment, "    "))
            items = ", ".join("%s = %s" % (name, cpp_unsigned(raw)) for name, raw in sig.enumerators)
            out.append("    enum class %s : %s { %s };" % (sig.enum_type, enum_underlying(sig.enumerators), items))
    if msg.selector:
        layout, value = msg.selector
        out.append("")
        out.append("    /// The constant field that tells this message from others of the same PGN.")
        out.append("    struct Selector")
        out.append("    {")
        out.append("        static constexpr %s::SignalLayout layout%s;" % (CODEC, layout_initializer(layout)))
        out.append("        static constexpr ::std::uint64_t value = %s;" % cpp_u64(value))
        out.append("    };")
    out.append("")
    out.append("    struct Signals")
    out.append("    {")
    for sig in msg.signals:
        out.append("        struct %s" % sig.name)
        out.append("        {")
        out.append("            static constexpr %s::SignalLayout layout%s;" % (
            CODEC, layout_initializer(sig.layout, sig.signed, sig.scale, sig.offset, sig.raw_min, sig.raw_max)))
        out.append("            static constexpr ::std::string_view name{%s};" % cpp_string(sig.name))
        out.append("            static constexpr ::std::string_view unit{%s};" % cpp_string(sig.unit))
        if sig.spn is not None:
            out.append("            static constexpr ::std::uint32_t spn = %d;" % sig.spn)
        if sig.specials:
            out.append("            static constexpr ::std::array<%s::SpecialValue, %d> special_values{{"
                       % (CODEC, len(sig.specials)))
            items = ["{%s, %s, %s, %s}" % (cpp_string(name), cpp_u64(first), cpp_u64(last), cpp_u64(send))
                     for name, first, last, send in sig.specials]
            out.append(",\n".join(" " * 16 + item for item in items) + "}};")
        if sig.enumerators:
            out.append("            static constexpr ::std::array<::std::uint64_t, %d> enumerators{%s};" % (
                len(sig.enumerators), ", ".join(cpp_unsigned(raw) for _, raw in sig.enumerators)))
        out.append("        };")
    out.append("    };")
    out.append("")
    for sig in msg.signals:
        out.extend(comment_lines(sig.comment, "    ") if not sig.enumerators else [])
        if sig.enumerators:
            out.append("    %s::EnumSignal<Signals::%s, %s> %s;" % (CODEC, sig.name, sig.enum_type, sig.name))
        else:
            out.append("    %s::Signal<Signals::%s> %s;" % (CODEC, sig.name, sig.name))
    if msg.signals:
        out.append("")
    for qualifier in ("", " const"):
        if msg.signals:
            calls = " ".join("visitor(this->%s);" % sig.name for sig in msg.signals)
            out.append("    template <typename Visitor> constexpr void for_each_signal(Visitor&& visitor)%s { %s }"
                       % (qualifier, calls))
        else:
            out.append("    template <typename Visitor> constexpr void for_each_signal(Visitor&&)%s {}" % qualifier)
    out.append("")
    out.append("    ::std::size_t encode(::std::span<::std::uint8_t> output_bytes) const")
    out.append("    {")
    out.append("        return %s::encode_message(*this, output_bytes);" % CODEC)
    out.append("    }")
    out.append("    static ::std::optional<%s> decode(::std::span<const ::std::uint8_t> input_bytes)" % msg.name)
    out.append("    {")
    out.append("        return %s::decode_message<%s>(input_bytes);" % (CODEC, msg.name))
    out.append("    }")
    out.append("    ::std::array<::std::uint8_t, kLength> bytes() const { return %s::message_bytes(*this); }" % CODEC)
    out.append("    friend bool operator==(const %s&, const %s&) = default;" % (msg.name, msg.name))
    out.append("};")
    out.append("static_assert(%s::MessageType<%s>);" % (CODEC, msg.name))
    return out


def generate(source_name, namespace, messages):
    out = ["// Generated by vehicle-sil tools/j1939_codegen from %s. Do not edit." % source_name,
           "#pragma once",
           "",
           '#include "vehicle_sil/j1939/codec.hpp"',
           "",
           "#include <array>",
           "#include <cstddef>",
           "#include <cstdint>",
           "#include <optional>",
           "#include <span>",
           "#include <string_view>",
           "",
           "namespace %s {" % namespace]
    for msg in messages:
        out.append("")
        out.extend(emit_message(msg))
    out.append("")
    out.append("} // namespace %s" % namespace)
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Generates J1939 message types from YAML.")
    parser.add_argument("definition")
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args(argv)
    name = os.path.basename(args.definition)
    try:
        with open(args.definition, encoding="utf-8") as f:
            document = yaml.load(f, Loader=_Loader)
        namespace, messages = parse(document, name)
        header = generate(name, namespace, messages)
    except (DefinitionError, yaml.YAMLError) as error:
        sys.stderr.write("%s: error: %s\n" % (args.definition, error))
        return 1
    directory = os.path.dirname(args.output)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
