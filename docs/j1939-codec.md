# J1939 typed messages (Codec)

[日本語](ja/j1939-codec.md)

This page shows how to define the signals of a PG in YAML, generate C++ types from the definition, and read and write signals as values. It also lists the keys of a definition and the API of the generated types. The API may still change.

When you define messages in YAML, the build generates C++ types from them. The application reads and writes signals as values, without writing bit positions or scaling. Nodes and protocol parts (`PeriodicSender`, `Monitor` and others) still send and receive the messages; the Codec only converts between the types and bytes.

| Goal | Use | Section |
|---|---|---|
| Generate types from definitions and link them | `vehicle_sil_add_j1939_codec()` | [Add to CMake](#add-to-cmake) |
| Define messages and signals | YAML keys | [Writing definitions](#writing-definitions) |
| Work out the position of a signal | `start_bit`, `byte_order` | [Bit positions](#bit-positions) |
| Define values such as Error or Not Available | `indicators`, `special_values` | [Special values (`indicators`)](#special-values-indicators) |
| Set the value of the bits that carry no signal | `fill` | [Fill value for unused bits (`fill`)](#fill-value-for-unused-bits-fill) |
| Read and write signals | `Signal`, `EnumSignal` | [Signal API (`vehicle_sil/j1939/codec.hpp`)](#signal-api-vehicle_silj1939codechpp) |
| Send and receive messages as types | `codec::attach_sender`, `codec::on_message` and others | [Use with a node (`vehicle_sil/j1939/codec_node.hpp`)](#use-with-a-node-vehicle_silj1939codec_nodehpp) |
| Test boundary values and special values | `set_raw()`, `for_each_signal` | [Use in tests](#use-in-tests) |
| Write types without the generator | `codec::MessageType` | [Hand-written types](#hand-written-types) |

Here is part of `install_engine()` in [examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp) (`node` is the node, `engine` the model). The messages are defined in `ecu_pair_messages.yaml` in the same directory.

```cpp
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
```

[examples/typed_messages](../examples/typed_messages) is a standalone example that defines messages in YAML and uses them.

## Add to CMake

Replace `engine_messages` and `my_ecu` with the names of your own targets. For how the repository uses it, see [examples/typed_messages/CMakeLists.txt](../examples/typed_messages/CMakeLists.txt).

```cmake
vehicle_sil_add_j1939_codec(engine_messages engine_messages.yaml)   # generates engine_messages.hpp
target_link_libraries(my_ecu PRIVATE engine_messages)
```

`vehicle_sil_add_j1939_codec(<target> <definition.yaml>...)` does the following:

| Item | Details |
|---|---|
| What it generates | One header per definition, at build time. |
| Header name | `<file name without its last extension>.hpp` (`engine.v2.hpp` for `engine.v2.yaml`). |
| `<target>` | An INTERFACE library that puts the generated headers on the include path and links `vehicle_sil::j1939_codec`. |
| Regeneration | Changing a definition or the generator regenerates the header. |
| Includes of the generated header | `vehicle_sil/j1939/codec.hpp` only. |

If you call this function, generating needs Python 3.11 or later and PyYAML. The Python interpreter is chosen in this order (see [Dependencies](dependencies.md)):

1. When you build vehicle-sil as the top-level project, the repository's `.venv` that `uv sync` created
2. If there is none, the Python that `find_package(Python3)` finds

You can also run the generator directly:

```sh
tools/j1939_codegen/j1939_codegen.py <definition.yaml> -o <header>
```

## Writing definitions

A complete definition file that you can pass to `vehicle_sil_add_j1939_codec()`:

```yaml
namespace: engine::messages        # namespace of the generated types
indicators: j1939                  # special value profile (default for the whole file; none if omitted)
messages:
  - name: EngineStatus
    comment: Engine -> all, every 20 ms.
    proprietary_b: 0x10            # PGN 0xFF10
    priority: 6
    signals:
      - name: speed
        start_bit: 0
        length: 16
        scale: 0.125               # or write 1/8
        unit: rpm
      - name: state
        start_bit: 16
        length: 2
        values: {stopped: 0, running: 1}
```

### Top level of the file

| Key | Meaning |
|---|---|
| `namespace` | Namespace of the generated types |
| `messages` | List of messages |
| `indicators` | Special value profile, the default for the whole file |
| `indicator_profiles` | Profile definitions |
| `fill` | Fill value, the default for the whole file (1 byte) |

### Messages

| Key | Meaning |
|---|---|
| `name` | Name of the type (a C++ identifier) |
| `pgn` | PGN (18 bits; for PDU1, the low 8 bits are 0) |
| `proprietary_a` | Proprietary A (`{}` for PGN 0xEF00, `{data_page: 1}` for 0x1EF00). It is PDU1, so you give a destination when you send it |
| `proprietary_b` | Group extension of Proprietary B (`0x10` for PGN 0xFF10, `{group_extension: 0x10, data_page: 1}` for 0x1FF10) |
| `priority` | Priority 0–7 (default 6). `codec::send()` and `codec::attach_sender()` send with this priority |
| `length` | Length in bytes, 1–1785 (default 8). A message of 9 bytes or more is sent with the transport protocol |
| `fill` | Value of the bits that carry no signal: a 1-byte value (used for every byte) or a list of bytes as long as the message (default: the file's `fill`, or 0xFF if the file has none) |
| `indicators` | Special value profile for the signals of this message (default: the file's `indicators`). See "Special values" below |
| `selector` | Constant field that tells this message apart from other messages with the same PGN (`{start_bit, length, value}`; `byte_order` is allowed too) |
| `signals` | List of signals (optional) |
| `comment` | Becomes the comment of the type |

Write exactly one of `pgn`, `proprietary_a` and `proprietary_b`.

### Signals

| Key | Meaning |
|---|---|
| `name` | Name of the member (a C++ identifier) |
| `start_bit`, `length` | Position and length (1–64 bits; see "Bit positions" below) |
| `byte_order` | `little` (default) or `big` |
| `signed` | `true` for two's complement (default `false`) |
| `scale`, `offset` | Physical value = raw × `scale` + `offset` (defaults 1 and 0; `scale` is positive). Besides numbers, you can write strings such as `"1/8"` and `"1e-3"` |
| `min`, `max` | Valid range of the physical value. The valid raw range narrows to the raw values inside this range (the raw end for `min` is rounded up, the raw end for `max` is rounded down) |
| `unit` | Unit (a string) |
| `indicators` | Special value profile for this signal (default: the message's `indicators`). See "Special values" below |
| `special_values` | Special values of this signal only (names and values, written like `values` in a profile). Replaces the inherited profile |
| `not_available`, `error` | Short forms of `not_available` and `error` in `special_values` |
| `values` | Enumeration of names and raw values; generates a scoped enum type (unsigned only; cannot be combined with `scale`, `offset`, `min`, `max` or `unit`) |
| `spn` | SPN number (for the reader; the Codec does not use it) |
| `comment` | Becomes the comment of the member |

### What the generator checks

The generator checks the following. On a violation, it fails and shows the line number or the location.

| What | Checked |
|---|---|
| Signal positions | Each signal fits in the message, and the signals and the selector do not overlap. |
| Names | They are C++ identifiers (not keywords or reserved names) and do not clash with each other. |
| Messages with the same PGN | If one definition file has several, all of them have a selector, at the same position, with different values (so that one frame is never decoded as several types). |
| Keys | There are no unknown keys and no duplicate keys. |
| Special values | They fit in the signal's bits, do not overlap each other and are not inside the valid range (see "Special values" below). |

Keys brought in with a merge key (`<<: *base`) can be overridden.

### Reserved names

Signals and messages cannot use the names that the generated types use.

| Name | Signal name | Message name |
|---|---|---|
| `encode`, `decode`, `bytes`, `for_each_signal`, `kPgn`, `kPriority`, `kLength`, `kFill`, `Signals`, `Selector`, `output_bytes`, `input_bytes`, `visitor`, `Visitor`, `kFillBytes` | Not allowed | Not allowed |
| Members of the signal definition: `layout`, `name`, `unit`, `spn`, `enumerators`, `special_values`, and `enumerator_names` (for enum signals with `values:` only) | Not allowed | Allowed |
| The message's own name | Not allowed | — |

The generated code fully qualifies `std` and `vehicle_sil` (with a leading `::`), so you can use any other name, including `std`, `data` and `size_t`.

A message that `vehicle_sil::spy::Spy::watch<M>()` watches also cannot have a signal named `count`, `last_ms`, `period_ms`, `size`, `invalid`, `stale` or `state` (they name its other variables; `watch()` does not build with such a signal), nor `stale_count` when `WatchOptions::stale_after` is set (throws `std::invalid_argument` at setup; allowed when `stale_after` is unset). See "Listen-only participants" in [j1939-guide.md](j1939-guide.md#listen-only-participants).

### How YAML values are read

| You write | Read as |
|---|---|
| `on`, `off`, `yes`, `no` | Strings (you can use them as enumerator names). |
| Integers | Decimal (no leading 0), hexadecimal (`0x`) and binary (`0b`) only. |
| `010`, `1:10`, `1:30.0` | Errors (YAML 1.1 reads them as octal and base 60). |
| A number that does not fit in a `double` | An error. |

### Bit positions

Positions count from the start of the message. Bit b of byte n (b = 0 is the least significant bit) is at position n × 8 + b.

| `byte_order` | What `start_bit` points to | Order of the other bits |
|---|---|---|
| `little` | The position of the least significant bit | Each higher bit is one position higher (up through the byte, then into the next byte) |
| `big` | The position of the most significant bit | Each lower bit is one position lower within the byte; bit 0 of a byte is followed by bit 7 of the next byte (the same counting as Motorola in DBC files) |

If you count "byte n, bit m" from 1, `start_bit` = (n − 1) × 8 + (m − 1).

### Special values (`indicators`)

A special value gives some raw values a meaning, such as "Error", "Not Available" or "calibrating". Special values belong to each signal; the Codec reserves no values across signals. In a signal without special values, every bit pattern is a value.

A special value is a name with a raw value (one value or a range).

| Item | Rule |
|---|---|
| Name | Lower-case letters, digits and `_`. |
| The names `error` and `not_available` | Give the states `Error` and `NotAvailable`. |
| Other names | Give the state `Special`. `special()` returns the name. |
| Value | Written as a raw value. |
| Values of a signed signal | Written as a negative number or as a bit pattern (in an 8-bit signed signal, `-1` and `0xFF` are the same value). |
| Range `[first, last]` | Raw values with `first <= last` (in a signed signal, split a range that crosses the sign into 2 special values). |
| Enumerator values | Cannot overlap special values. |

The value sent is the value that `set_special(name)` writes. For `not_available`, it is also the initial value in a new message.

| Written as | Value sent |
|---|---|
| One value | That value |
| A range | The first value of the range |
| `{range: [first, last], send: v}` | `v` |

The built-in `j1939` profile's `not_available` uses `send` to send all bits set to 1. To send all bits set to 1 for a `not_available` that you write as a range, write `send` as well.

The valid range (`raw_min`–`raw_max`) is narrowed to exclude the special values at its ends. So setting a physical value never produces a special value. A special value inside the valid range is an error; exclude it with `min` and `max`.

In an enum signal, every value that is neither a special value nor an enumerator is `Undefined` (an enumeration has no range, so it is never `OutOfRange`).

#### Profiles

A profile is a named set of special values. Define it once in `indicator_profiles` and select it by name. This part goes at the top level of the definition file:

```yaml
indicator_profiles:
  vendor:                                  # any name
    - when: {length: 8, signed: false}     # this rule is for 8-bit unsigned signals
      values: {sensor_error: 0xFE, not_available: 0xFF}
    - when: {length: 16, signed: false}
      values:
        sensor_error: [0xFE00, 0xFEFF]     # a range
        not_available: {range: [0xFF00, 0xFFFF], send: 0xFFFF}   # sends all bits 1 (default 0xFF00)
    - when: {length: 16, signed: true}
      values: {sensor_error: -32768}       # signed values can be negative (same as 0x8000)
  calibration:                             # a single rule need not be a list
    when: {length: 8, signed: false}
    values: {calibrating: 0, not_available: 0xFF}
```

A profile is a list of rules.

| Item | Rule |
|---|---|
| What a rule has | `values` (the special values) and an optional `when`. |
| `when` | Limits the signals the rule applies to, by `length` (a length or a list of lengths) and `signed` (true or false). |
| Rule used for a signal | The first rule whose `when` matches. |
| Rule without `when` | Matches every signal (an error if a value does not fit in a signal's bits, or falls inside its valid range). |
| Built-in profiles | `none` (no special values) and `j1939` (table below). |
| Defining `j1939` in the file | Replaces the built-in one. |
| `none` | Cannot be defined. |

In a profile that applies to a whole file or message, it is safest to write `length` and `signed` in every rule: in a signed signal, `0xFF` is −1, which falls inside the valid range.

#### Inheritance and overrides

A signal's special values come from the first of these that exists. Inherited special values and the signal's own are never merged: the one found replaces the others entirely. Which signal has which special values is decided at generation time.

1. The signal's `special_values` (and `not_available`, `error`)
2. The signal's `indicators`
3. The message's `indicators`
4. The file's `indicators`
5. If none of these exists, `none`

The default is `none`. The J1939 special values apply only when you write `indicators: j1939` (so that a Proprietary B message never gets the J1939 interpretation by accident). `indicators: none` cancels a setting at a higher level.

If the selected profile has no rule that matches the signal's length and signedness, the result depends on where the profile was selected:

| Where the profile is selected | No matching rule |
|---|---|
| The signal's `indicators` | An error. |
| A message or the file (the signal inherits it) | The signal has no special values. |
| A message or the file, and the profile is defined in the file and matches none of the signals that inherit it | An error (so that a mistake in `when` does not silently remove the special values from every signal). |

- Even with `j1939` for the whole file, 1-bit flags and 12-bit values stay plain values.
- The built-in `j1939` is exempt from the error for matching no signal (you can write `indicators: j1939` even in a file with only lamps and flags).
- The generator's error messages show the name of the special value, its value (in hexadecimal) and where the profile was selected (for example `profile vendor of the file`).

#### The built-in `j1939` profile

| Signal | Valid | `parameter_specific` | `reserved` | `error` | `not_available` (value sent) |
|---|---|---|---|---|---|
| 8-bit unsigned | 0x00–0xFA | 0xFB | 0xFC–0xFD | 0xFE | 0xFF |
| 16-bit unsigned | 0x0000–0xFAFF | 0xFB00–0xFBFF | 0xFC00–0xFDFF | 0xFE00–0xFEFF (0xFE00) | 0xFF00–0xFFFF (0xFFFF) |
| 32-bit unsigned | 0x00000000–0xFAFFFFFF | 0xFB000000–0xFBFFFFFF | 0xFC000000–0xFDFFFFFF | 0xFE000000–0xFEFFFFFF (0xFE000000) | 0xFF000000–0xFFFFFFFF (0xFFFFFFFF) |
| 2-bit unsigned | 0, 1 | — | — | 2 | 3 |

Other lengths (1 bit, 3–7 bits, 24 bits, 64 bits and so on) and signed signals have no rule. The profile applies no guessed rules to lengths whose treatment in the standard has not been verified. If you need one, redefine `j1939` in the file, or write `special_values` on the signal.

The values in the table follow widely used practice and have not been checked against the text of the standard (see "Conformance to the standard" at the end of this page).

#### Standard J1939 versus Proprietary B

| Contents of the file | How to write it |
|---|---|
| Standard PGs | Write one line, `indicators: j1939`, at the top level. For SPNs with a defined range or meaning, override with the signal's `min`, `max` and `special_values`. |
| Proprietary B only, with no custom special values | Write nothing (every bit pattern is a value). |
| Proprietary B only, with manufacturer-specific special values | Define one profile and select it with the `indicators` of the file or the message. Write `indicators` or `special_values` only on the signals that are exceptions. |
| Both in one file | Set the file's `indicators` to the kind you use more, and write `indicators` on the messages of the other kind. |

A signal's `special_values` replaces the inherited profile entirely (it does not add to it). Even to change one value of a standard signal, write the values you want to keep along with it.

For 2-bit signals, the built-in `j1939` profile keeps `2` as `error` and `3` as `not_available` by default so that standard 2-bit status signals never treat error or not-available patterns as ordinary values. For 2-bit command signals (such as TSC1 or XBR), override that rule on the signal itself: write `indicators: none` when all values `0..3` are valid command states, or write `not_available: 3` on the signal when `3` means "don't care / take no action" (this replaces the inherited 2-bit rule so `0..2` stay ordinary values).

Here is one entry of a message's `signals`:

```yaml
# a standard 8-bit signal that gives 0xFB (parameter_specific) a meaning of its own
- name: coolant_level
  start_bit: 0
  length: 8
  special_values: {level_unknown: 0xFB, reserved: [0xFC, 0xFD], error: 0xFE, not_available: 0xFF}
```

A complete definition file:

```yaml
# a Proprietary B file with hundreds of signals
namespace: vendor::messages
indicators: vendor                 # for every signal
fill: 0x00                         # unused bits of every message
indicator_profiles:
  vendor:
    - when: {length: 8, signed: false}
      values: {sensor_error: 0xFE, not_available: 0xFF}
    - when: {length: 16, signed: false}
      values: {sensor_error: [0xFE00, 0xFEFF], not_available: {range: [0xFF00, 0xFFFF], send: 0xFFFF}}
messages:
  - name: Pressures
    proprietary_b: 0x40
    signals:
      - {name: p1, start_bit: 0, length: 8, scale: 0.5, unit: kPa}
      - {name: p2, start_bit: 8, length: 8, scale: 0.5, unit: kPa}
      - {name: raw_count, start_bit: 16, length: 16, indicators: none}            # uses every value
      - {name: level, start_bit: 32, length: 8, special_values: {calibrating: 0}} # this signal differs
  - name: Debug
    proprietary_b: 0x41
    indicators: none                                                               # no special values in this message
    signals:
      - {name: word, start_bit: 0, length: 32}
```

### Fill value for unused bits (`fill`)

`fill` is the value of the bits that carry neither a signal nor a selector. It is separate from special values and does not change the value of any signal.

| Where | What you write |
|---|---|
| `fill` of the file | A 1-byte value. It is the default for every message. |
| `fill` of a message | Overrides the file's default. |
| `fill` of a message (list of bytes) | A list of bytes as long as the message (`[0xFF, 0x00, ...]`), to set a bit pattern. |
| None of these | 0xFF. |

### Name clashes with Windows macros

If you include `<windows.h>` without `NOMINMAX`, the `min` and `max` macros break calls to `Signal::min()` (write `(S::min)()`, or define `NOMINMAX`). For the same reason, avoid names that match Windows macros, such as `ERROR`, `IN` and `OUT`.

## Generated types

The first definition in "Writing definitions" generates a type of the form below. It shows only the main points and does not compile as is (`...` marks omitted parts; the real code fully qualifies names with `::std::` and `::vehicle_sil::`).

```cpp
struct EngineStatus
{
    static constexpr j1939::Pgn kPgn{0xFF10u};
    static constexpr j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 8;
    static constexpr std::uint8_t kFill = 0xFF;
    enum class State : std::uint8_t { stopped = 0, running = 1 };
    struct Signals { struct speed { static constexpr codec::SignalLayout layout{...}; ... }; ... };

    codec::Signal<Signals::speed> speed;
    codec::EnumSignal<Signals::state, State> state;

    std::size_t encode(std::span<std::uint8_t> output_bytes) const;     // writes kLength bytes
    static std::optional<EngineStatus> decode(std::span<const std::uint8_t> input_bytes);
    std::array<std::uint8_t, kLength> bytes() const;
    template <typename Visitor> void for_each_signal(Visitor&& visitor); // visitor(signal) for each signal (a const version exists too)
    friend bool operator==(const EngineStatus&, const EngineStatus&) = default;
};
```

| Item | Behavior |
|---|---|
| Signals of a new message | Not Available (raw 0 for signals without `not_available`). |
| `encode(out)` | Sets the bits that carry no signal to `kFill` (`kFillBytes` for a list-of-bytes fill value), writes `kLength` bytes and returns `kLength`. |
| `encode(out)` with a short `out` | Throws `std::length_error`. |
| `decode(data)` | Returns empty if `data` is shorter than `kLength` or the selector value differs (extra bytes are ignored). |
| State and memory | The type holds no state, and `encode` and `decode` allocate no memory. |
| `Signal` and `EnumSignal` | Do not convert implicitly to numbers or enums, and do not accept numbers by assignment. |

### Signal API (`vehicle_sil/j1939/codec.hpp`)

| | Meaning |
|---|---|
| `state()` | State of the signal (table below). Special values are checked first, then the range or the enumerators |
| `special()` | Name of the special value it holds (`std::optional<std::string_view>`); empty if it holds no special value |
| `value()` | The value, only when `Valid` (the physical value as `std::optional<double>` for `Signal`, the enumerator for `EnumSignal`); empty otherwise |
| `value_or(x)` | `x` if `value()` is empty |
| `set(physical)` | Sets the raw value nearest to the physical value (table below) |
| `set(enumerator)` | Sets the value of an `EnumSignal` |
| `raw()` | Raw value (signed for a signed signal). The received raw value is kept as is, even for special, out-of-range and undefined values |
| `set_raw(raw)` | Sets any value that fits in the signal's bits, without the restrictions of special values; `std::out_of_range` if it does not fit |
| `bits()` | The signal's bits in the message (unsigned) |
| `set_special(name)` | Sets the special value with that name (writes the value sent for it); `std::invalid_argument` if there is no special value with that name |
| `set_not_available()`, `set_error()` | `set_special("not_available")`, `set_special("error")` (write the values sent for them). A compile error for a signal without that special value |
| `has_special(name)`, `specials` | Whether there is a special value with that name (constexpr); the array of special value definitions (`SpecialValue`: `name`, `first`, `last`, `send`) |
| `enumerator_names` | Constexpr array of `codec::Enumerator` (`name`, `value`) in definition order (`EnumSignal` and `Signals::<signal>::enumerator_names`; empty for a hand-written `Spec` without `enumerator_names`) |
| `min()`, `max()`, `resolution()`, `to_physical(raw)` | Valid range of the physical value, physical value per raw step of 1, raw to physical (`Signal` only; `resolution()` is constexpr) |
| `raw_min`, `raw_max`, `lowest_raw()`, `highest_raw()` | Valid range of raw values (for `EnumSignal`, the lowest and highest enumerators), and the range of raw values that fit in the bits |
| `layout`, `name`, `unit`, `spn` | The definition (constexpr) |

| `state()` | When |
|---|---|
| `Valid` | The value can be read as a value |
| `Error`, `NotAvailable` | It is the special value with that name |
| `Special` | It is a special value with another name |
| `OutOfRange` | For a `Signal`: it is not a special value and is outside the valid range |
| `Undefined` | For an `EnumSignal`: it is neither a special value nor an enumerator |

| Value passed to `set(physical)` | Value set | Returns |
|---|---|---|
| A physical value in the valid range | The nearest raw value (exact halves round away from 0) | `true` |
| A value that rounds to outside the valid range | The nearer end | `false` |
| For a 64-bit signal, a value that equals an end once converted to `double` | That end | `true` |
| NaN | Not Available (a signal without Not Available is unchanged) | `false` |

Read a received value according to its state. For special and out-of-range values too, `raw()` returns the received value as is (for a special value defined as a range, it tells you which value in the range arrived).

A fragment (`m` is a received typed message; `use`, `log` and `detail` are yours). For a working example that reads the names of special values, see [examples/typed_messages/main.cpp](../examples/typed_messages/main.cpp).

```cpp
if (const auto v = m.speed.value()) {
    use(*v);                                        // physical value
} else if (const auto name = m.speed.special()) {
    log(*name, m.speed.raw());                      // e.g. "sensor_error", 0xFE42
    // offset from the start of the range (e.g. a custom special value with details in the low byte)
    for (const auto& s : decltype(m.speed)::specials) {
        if (s.name == *name) { detail = m.speed.raw() - s.first; }
    }
} else {
    log("out of range", m.speed.raw());             // out of range (Undefined for an enum signal)
}
```

Physical values are computed in `double`. The product raw × scale is rounded to `double` before the offset is added, so the compiler and the optimizer never fuse the multiplication and the addition into one rounding. As a result, the value is the same on every platform that computes in IEEE double precision (SSE on x86-64, AArch64 and others). 32-bit x86, which computes in x87 extended precision, is not covered. Physical values of raw values wider than 53 bits are rounded (the raw values stay exact).

## Use with a node (`vehicle_sil/j1939/codec_node.hpp`)

| Function | Meaning |
|---|---|
| `codec::attach_sender<M>(node, period, fill, options)` | Attaches a `PeriodicSender`. For each transmission, it creates a new `M`, sets the signals with `fill(M&)` and sends it |
| `codec::encoder<M>(fill)` | A `PeriodicSender::Encoder` that does the same (for when you create the `PeriodicSender` yourself; set `max_length` to `M::kLength` or more) |
| `codec::on_message<M>(node, handler)` | Receives the PGN of `M` and passes only the messages that decode to `handler(const M&)` or `handler(const M&, const Message&)` (remove it with the returned `HandlerId`) |
| `codec::send(node, message, destination, when_busy)` | `node.send(M::kPgn, message.bytes(), destination, M::kPriority, when_busy)` (`when_busy` defaults to `WhenBusy::Fail`) |
| `codec::decode<M>(message)` | `M::decode(message.data)` only when `message.pgn` is `M::kPgn` (empty otherwise). Use it where many PGNs arrive (such as the `match` of an `Exchange`) |

`codec::attach_sender` follows these rules:

| Item | Rule |
|---|---|
| Signals of a new `M` | Not Available for signals that have Not Available, raw 0 for the others. |
| Priority | Sends with `M::kPriority`. |
| `options.priority` | Leave it at the default (6) or set it to `M::kPriority` (any other value throws `std::invalid_argument`). |
| Buffer | `M::kLength` bytes or more. |

Other uses:

| Situation | How |
|---|---|
| See what `on_message<M>` does not pass (short messages, other selector values) | Use `node.on_message()` and `M::decode()`. |
| Read the latest message a `Monitor` received | Read it with `M::decode(monitor.data())` (empty if nothing has been received yet). |
| Another type arrives on the PGN of a `Monitor` | Use `MonitorOptions::accept` to accept only the messages for which `codec::decode<M>(message)` returns a value. |
| Read the response of a `Requester` | Read it with `M::decode(result.data)`. |
| Several types on the same PGN (Proprietary A told apart by selectors) | Register one `on_message<M>` per type. |

`fill` and the handlers are called within a step, like any other handler. An exception thrown from them puts the `Network` in the failed state.

## Use in tests

| To check | How |
|---|---|
| Boundary values and special values | Create them with `set_raw()` and `set_special()`, and check them with `state()`, `special()`, `value()` and `raw()`. |
| Definitions of ranges and special values | Look up the ranges with `raw_min`, `raw_max`, `min()` and `max()`, and the special values with `specials`. |
| Every signal of a message | `for_each_signal` runs the same check on every signal. |
| Broken messages (short, wrong selector) | `M::decode()` returns empty. |

`tests/j1939/test_j1939_codec.cpp` uses `for_each_signal` to check the encode and decode round trip for every raw value of every signal of 16 bits or less.

## Hand-written types

You can write a type of the same form in C++ without the generator. The type must satisfy `codec::MessageType`. Leave `encode`, `decode` and `bytes` to `codec::encode_message`, `codec::decode_message` and `codec::message_bytes`.

| What you write | Requirement |
|---|---|
| Message type | Has `kPgn`, `kPriority`, `kLength`, `kFill` or `kFillBytes`, `for_each_signal`, `encode` and `decode`. |
| `kFill` and `kFillBytes` | Has exactly one of them (`kFillBytes` is a `std::array<std::uint8_t, kLength>`). |
| Signal `Spec` | A type with `layout`, `name` and `unit` (and optionally `spn` and `special_values`, plus `enumerators` and optionally `enumerator_names` for an enum). |

| When | What is checked |
|---|---|
| At compile time | That the valid range, the special value ranges and values sent, and the enumerator values fit in the signal's bits, and that the signals fit in the message length |
| Not checked (follow the generator's rules yourself) | Overlap between special values and the valid range, and telling apart types with the same PGN |

## Limitations

- Multiplexing (signals whose layout changes with the selector value), variable-length data (strings, fields delimited by `*`), and arrays and repetitions (such as the DTCs of DM1) are not supported.
- The SAE J1939 PGN and SPN definitions (J1939DA) are not included. To use standard PGs, write the YAML from definitions that you have obtained yourself.

## Conformance to the standard

Nothing has been verified against the text of the standard yet. The built-in `j1939` profile follows widely used practice. These points are not verified:

- The ranges for 8, 16 and 32 bits, and the distinction between `parameter_specific` (0xFB…) and `reserved` (0xFC–0xFD…).
- The 2-bit ranges (2 is Error, 3 is Not Available), kept as the built-in `j1939` default for 2-bit status signal safety; 2-bit command signals (such as TSC1 or XBR) can override them per signal with `indicators: none` or `not_available: 3` (see "[Standard J1939 versus Proprietary B](#standard-j1939-versus-proprietary-b)").
- Special values for the lengths that have no rule (1 bit, 3–7 bits, 24 bits, 64 bits) and for signed signals.
- The values sent: the first value of the range for Error (0xFE00 and so on), all bits set to 1 for Not Available.
- The default fill value for unused bits and bytes (0xFF).

If the verified values turn out to differ, redefine the `j1939` profile in your file; you do not have to wait for an update of the Codec. Remember to write a `send` that sends all bits set to 1 for a `not_available` written as a range.

## See also

| Page | Contents |
|---|---|
| [j1939-guide.md](j1939-guide.md) | Nodes, sending and receiving, and protocol parts. Read it before this page. |
| [ecu-executable.md](ecu-executable.md) | How to build an ECU as an executable |
