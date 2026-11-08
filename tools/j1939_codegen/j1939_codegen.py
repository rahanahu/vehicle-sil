#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates typed J1939 message types (a C++ header) from a YAML definition.

Usage: j1939_codegen.py <definition.yaml> -o <header.hpp>

The format is described in docs/j1939-codec.md. The header includes only
vehicle_sil/j1939/codec.hpp. Needs Python 3.8+ and PyYAML.
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
SPEC_MEMBERS = {"layout", "name", "unit", "spn", "enumerators"}

# Names the generated message type uses itself (members and the parameters of its functions).
MESSAGE_MEMBERS = {"kPgn", "kPriority", "kLength", "kFill", "Selector", "Signals", "encode", "decode",
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


def raw_range(where, value, length):
    top = (1 << length) - 1
    if isinstance(value, list):
        if len(value) != 2:
            fail(where, "expected a value or [first, last]")
        first = integer(where, value[0], 0, top)
        last = integer(where, value[1], 0, top)
        if first > last:
            fail(where, "first > last")
        return (first, last)
    v = integer(where, value, 0, top)
    return (v, v)


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


def j1939_indicators(length):
    """(valid max, error, not available) of the j1939 indicators for an unsigned signal."""
    top = (1 << length) - 1
    if length == 1:
        return top, None, None
    if length % 8 == 0:
        rest = length - 8
        low = (1 << rest) - 1
        return (0xFA << rest) | low, (0xFE << rest, (0xFE << rest) | low), (0xFF << rest, top)
    return top - 2, (top - 1, top - 1), (top, top)


def as_raw_intervals(bits_range, signed, length):
    """A range of bit patterns as intervals of raw values (two for a signed range across the sign)."""
    first, last = bits_range
    if not signed:
        return [(first, last)]
    half = 1 << (length - 1)
    value = lambda bits: bits - (1 << length) if bits >= half else bits  # noqa: E731
    if first < half <= last:
        return [(value(first), half - 1), (-half, value(last))]
    return [(value(first), value(last))]


def without_special_values(where, low, high, ranges, signed, length):
    """Narrows the valid range low .. high so that it holds no error or "not available" value: the
    special values may sit at its ends, not inside it."""
    intervals = [i for r in ranges if r for i in as_raw_intervals(r, signed, length)]
    changed = True
    while changed:  # trims from the ends while one of them is special
        changed = False
        for first, last in intervals:
            if low > high or last < low or first > high:
                continue
            if last >= high:
                high, changed = first - 1, True
            elif first <= low:
                low, changed = last + 1, True
    if low > high:
        fail(where, "the error and 'not available' values leave no valid value (min, max)")
    for first, last in intervals:
        if first <= high and last >= low:
            fail(where, "error or 'not available' values %d .. %d lie inside the valid range: "
                 "set 'min' and 'max' around them" % (first, last))
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


def parse_signal(where, s, default_indicators, message_bits):
    check_keys(where, s, ["name", "start_bit", "length"],
               ["byte_order", "signed", "scale", "offset", "min", "max", "unit", "indicators",
                "not_available", "error", "spn", "values", "comment"])
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

    # Indicators and the valid range of the raw value.
    top = (1 << length) - 1
    if sig.signed:
        low, high = -(1 << (length - 1)), (1 << (length - 1)) - 1
    else:
        low, high = 0, top
    sig.error = sig.not_available = None
    if "not_available" in s or "error" in s:
        if "indicators" in s:
            fail(where, "give either 'indicators' or 'not_available'/'error'")
        if "not_available" in s:
            sig.not_available = raw_range(where + ".not_available", s["not_available"], length)
        if "error" in s:
            sig.error = raw_range(where + ".error", s["error"], length)
        if sig.error and sig.not_available and \
                sig.error[0] <= sig.not_available[1] and sig.not_available[0] <= sig.error[1]:
            fail(where, "the 'error' and 'not_available' ranges overlap")
    else:
        indicators = s.get("indicators", default_indicators)
        if indicators not in ("j1939", "none"):
            fail(where + ".indicators", "expected 'j1939' or 'none'")
        if indicators == "j1939" and not sig.signed:
            high, sig.error, sig.not_available = j1939_indicators(length)

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
            for kind, rng in (("error", sig.error), ("not available", sig.not_available)):
                if rng and rng[0] <= raw <= rng[1]:
                    fail("%s.values.%s" % (where, name), "raw value %d is in the %s range" % (raw, kind))
            seen.add(raw)
            sig.enumerators.append((name, raw))
        low, high = min(seen), max(seen)
    else:
        if "min" in s:
            low = max(low, math.ceil((number(where + ".min", s["min"]) - sig.offset) / sig.scale))
        if "max" in s:
            high = min(high, math.floor((number(where + ".max", s["max"]) - sig.offset) / sig.scale))
        if low > high:
            fail(where, "the valid range is empty (min, max, indicators)")
        low, high = without_special_values(where, low, high, (sig.error, sig.not_available), sig.signed, length)
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


def parse_message(where, m, default_indicators):
    check_keys(where, m, ["name"], ["pgn", "proprietary_a", "proprietary_b", "priority", "length", "fill",
                                    "selector", "signals", "comment"])
    msg = Message()
    msg.name = identifier(where + ".name", m["name"])
    if msg.name in MESSAGE_MEMBERS:
        fail(where + ".name", "'%s' is used by the generated type" % msg.name)
    where = "%s (%s)" % (where, msg.name)
    msg.pgn = parse_pgn(where, m)
    msg.priority = integer(where + ".priority", m.get("priority", 6), 0, 7)
    msg.length = integer(where + ".length", m.get("length", 8), 1, MAX_LENGTH)
    msg.fill = integer(where + ".fill", m.get("fill", 0xFF), 0, 255)
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
        sig = parse_signal("%s.signals[%d]" % (where, i), s, default_indicators, bits)
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
    return msg


def parse(document, where):
    if not isinstance(document, dict):
        fail(where, "expected a mapping with 'namespace' and 'messages'")
    check_keys(where, document, ["namespace", "messages"], ["indicators"])
    namespace = text(where + ".namespace", document["namespace"])
    for part in namespace.split("::"):
        identifier(where + ".namespace", part)
    indicators = document.get("indicators", "j1939")
    if indicators not in ("j1939", "none"):
        fail(where + ".indicators", "expected 'j1939' or 'none'")
    messages = document["messages"]
    if not isinstance(messages, list):
        fail(where + ".messages", "expected a list")
    result = []
    seen = set()
    for i, m in enumerate(messages):
        msg = parse_message("%s.messages[%d]" % (where, i), m, indicators)
        if msg.name in seen:
            fail(where, "message '%s' appears twice" % msg.name)
        seen.add(msg.name)
        result.append(msg)
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


def layout_initializer(layout, signed=False, scale=Fraction(1), offset=Fraction(0), raw_min=0, raw_max=0,
                       error=None, not_available=None):
    start, length, big = layout
    parts = [".start_bit = %d" % start, ".length = %d" % length,
             ".byte_order = %s::ByteOrder::%s" % (CODEC, "BigEndian" if big else "LittleEndian"),
             ".is_signed = %s" % ("true" if signed else "false"),
             ".scale = %s" % cpp_double(scale), ".offset = %s" % cpp_double(offset),
             ".raw_min = %s" % cpp_u64(raw_min), ".raw_max = %s" % cpp_u64(raw_max)]
    if error:
        parts.append(".error = {%s, %s}" % (cpp_u64(error[0]), cpp_u64(error[1])))
    if not_available:
        parts.append(".not_available = {%s, %s}" % (cpp_u64(not_available[0]), cpp_u64(not_available[1])))
    # Three lines, the error and "not available" ranges on the last one.
    head, tail = parts[:3], parts[3:8]
    rest = parts[8:]
    lines = [", ".join(head), ", ".join(tail)] + ([", ".join(rest)] if rest else [])
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
    out.append("    static constexpr ::std::uint8_t kFill = 0x%02X;" % msg.fill)
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
            CODEC, layout_initializer(sig.layout, sig.signed, sig.scale, sig.offset, sig.raw_min, sig.raw_max,
                                      sig.error, sig.not_available)))
        out.append("            static constexpr ::std::string_view name{%s};" % cpp_string(sig.name))
        out.append("            static constexpr ::std::string_view unit{%s};" % cpp_string(sig.unit))
        if sig.spn is not None:
            out.append("            static constexpr ::std::uint32_t spn = %d;" % sig.spn)
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
