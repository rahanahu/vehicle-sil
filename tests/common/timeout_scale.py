# SPDX-License-Identifier: MIT
"""Factor on the tests' upper bounds of waiting (tests/unit/test_timeout_scale.hpp).

Standard library only. Kept under tests/common/ (not tests/) so adding its directory to sys.path
never shadows standard library modules such as signal or trace via tests/signal/ or tests/trace/.
"""

from __future__ import annotations

import math
import os
import re

TIMEOUT_SCALE_VARIABLE = "VEHICLE_SIL_TEST_TIMEOUT_SCALE"
TIMEOUT_SCALE_DEFAULT_VARIABLE = "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT"

_DIGITS_AND_POINT = re.compile(r"[0-9]*\.?[0-9]*")
_HAS_DIGIT = re.compile(r"[0-9]")
_FIX_HINT = (
    " is not a number greater than 0 in digits (such as 3 or 2.5): set it to a factor such as 3 "
    "(every upper bound of waiting in the tests 3 times as long)"
)


def parse_timeout_scale(text: str) -> float | None:
    """Return the scale factor when `text` is ASCII digits with at most one point and finite > 0."""
    if not isinstance(text, str) or not _DIGITS_AND_POINT.fullmatch(text) or not _HAS_DIGIT.search(text):
        return None
    value = float(text)
    if not math.isfinite(value) or value <= 0.0:
        return None
    return value


def read_timeout_scale(variable: str | None = None, build_default: str | None = None) -> float:
    """Read and validate the timeout scale from `variable` or else `build_default`."""
    if variable:
        parsed = parse_timeout_scale(variable)
        if parsed is None:
            raise ValueError(f'{TIMEOUT_SCALE_VARIABLE}="{variable}"{_FIX_HINT}, or unset it')
        return parsed
    default = build_default or "1"
    parsed = parse_timeout_scale(default)
    if parsed is None:
        raise ValueError(
            f'{TIMEOUT_SCALE_DEFAULT_VARIABLE}="{default}"{_FIX_HINT}, and configure the build again'
        )
    return parsed


def timeout_scale() -> float:
    """Read and validate the timeout scale for the current process environment."""
    return read_timeout_scale(
        os.environ.get(TIMEOUT_SCALE_VARIABLE),
        os.environ.get(TIMEOUT_SCALE_DEFAULT_VARIABLE),
    )


def scaled(seconds: float) -> float:
    """Multiply an upper bound of waiting by the active timeout scale."""
    return seconds * timeout_scale()
