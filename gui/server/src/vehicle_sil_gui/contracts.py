"""Single-source-of-truth Pydantic v2 wire contracts for the GUI server (`vehicle_sil_gui: 4`).

Zero `aiohttp` imports and zero I/O. Provides `OmitOnly` + `ContractJsonSchemaGenerator`,
strictly typed HTTP (17 endpoints), WebSocket (16 frames), CLI (`--json`), and Events v4 models,
the centralized `ErrorCode` catalog, and strict JSON / validation formatting helpers.
Shared numeric and enum constants are imported from stdlib-only `vehicle_sil_control.protocol`.
"""

from __future__ import annotations

import json
import math
from collections.abc import Mapping, Sequence
from enum import StrEnum
from typing import Annotated, Any, ClassVar, Literal, TypeVar, Union

from pydantic import (
    AfterValidator,
    BaseModel,
    BeforeValidator,
    ConfigDict,
    Field,
    GetJsonSchemaHandler,
    StrictBool,
    StrictFloat,
    StrictInt,
    StrictStr,
    TypeAdapter,
    ValidationError,
    ValidationInfo,
    field_validator,
    model_validator,
)
from pydantic.json_schema import GenerateJsonSchema, JsonSchemaValue
from pydantic_core import CoreSchema
from vehicle_sil_control.protocol import (
    EVENTS_FORMAT,
    MAX_ID,
    SPEED_MAX,
    SPEED_MIN,
    STEP_MS_MAX,
    TIME_COMMANDS,
)

PROTOCOL: int = 4
STAMP_PATTERN: str = r"^[0-9a-f]{64}$"
REQUEST_FIELDS: tuple[str, ...] = (
    "file",
    "$schema",
    "vehicle_sil_run_request",
    "scenario",
    "after_scenario",
    "variant",
    "params",
    "disable",
    "enable",
    "manual",
    "strict",
    "connect",
    "time_policy",
    "speed",
    "start_paused",
    "stop_at_ms",
    "late_tolerance_ms",
)
CHECK_FIELDS: tuple[str, ...] = (*REQUEST_FIELDS, "text")

DEFAULT_WRITES_PER_REQUEST: int = 256
DEFAULT_WATCH_VARIABLES: int = 2000
DEFAULT_PERIOD_MS_MIN: int = 20
DEFAULT_PERIOD_MS_MAX: int = 60_000

_M = TypeVar("_M", bound="ContractModel")


# ------------------------------------------------------------ Strict JSON helpers


def no_json_constant(name: str) -> Any:
    raise ValueError(f"{name} is not JSON")


def finite_json_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"{text} is not a finite number")
    return value


def parse_strict_json(raw: str | bytes) -> Any:
    """Parses strict JSON (no NaN/Infinity, no overflowing floats, catches deep recursion)."""
    try:
        return json.loads(raw, parse_constant=no_json_constant, parse_float=finite_json_float)
    except RecursionError as exc:
        raise ValueError("JSON nesting is too deep") from exc


def parse_json_object(raw: str | bytes, *, allow_non_finite: bool = False) -> dict[str, Any]:
    """Parses a JSON object, raising ValueError on malformed JSON, deep nesting, or non-object root."""
    try:
        data = json.loads(raw) if allow_non_finite else parse_strict_json(raw)
    except (ValueError, UnicodeDecodeError, RecursionError) as exc:
        raise ValueError("the body is not JSON") from exc
    if not isinstance(data, dict):
        raise ValueError("the body must be a JSON object")
    return data


# ------------------------------------------------------------ OmitOnly & JSON Schema hook


class _OmitOnlyMarker:
    """Marks an optional field as omit-only: omitted is allowed, explicit `null` is rejected at runtime and in schema."""

    def __repr__(self) -> str:
        return "OmitOnly"

    def __get_pydantic_json_schema__(
        self,
        core_schema: CoreSchema,
        handler: GetJsonSchemaHandler,
    ) -> JsonSchemaValue:
        schema = dict(handler(core_schema))
        schema["x-omit-only"] = True
        return schema


OmitOnly = _OmitOnlyMarker()


def _strip_omit_only(node: Any) -> Any:
    if isinstance(node, dict):
        if node.pop("x-omit-only", None):
            if node.get("default") is None:
                node.pop("default", None)
            if "anyOf" in node and isinstance(node["anyOf"], list):
                non_null = [s for s in node["anyOf"] if s != {"type": "null"}]
                if len(non_null) == 1:
                    rest = {k: v for k, v in node.items() if k != "anyOf"}
                    node = {**non_null[0], **rest}
                else:
                    node["anyOf"] = non_null
        return {k: _strip_omit_only(v) for k, v in node.items()}
    if isinstance(node, list):
        return [_strip_omit_only(v) for v in node]
    return node


class ContractJsonSchemaGenerator(GenerateJsonSchema):
    """Pydantic v2 JSON Schema generator keeping `OmitOnly` fields non-nullable in OpenAPI 3.1 / Draft 2020-12."""

    def generate(self, schema: CoreSchema, mode: str = "validation") -> JsonSchemaValue:
        return _strip_omit_only(super().generate(schema, mode=mode))

    def generate_definitions(self, inputs: Any) -> Any:
        remap, defs = super().generate_definitions(inputs)
        return remap, _strip_omit_only(defs)


# ------------------------------------------------------------ Base models & scalar types


class _OmitOnlyMixin(BaseModel):
    """Rejects explicit `null` for fields annotated with `OmitOnly`."""

    _omit_only_keys: ClassVar[frozenset[str]] = frozenset()

    @classmethod
    def __pydantic_init_subclass__(cls, **kwargs: Any) -> None:
        super().__pydantic_init_subclass__(**kwargs)
        cls._omit_only_keys = frozenset(
            (info.alias or name)
            for name, info in cls.model_fields.items()
            if any(m is OmitOnly for m in info.metadata)
        )

    @model_validator(mode="before")
    @classmethod
    def _reject_omit_only_nulls(cls, data: Any) -> Any:
        if isinstance(data, dict) and cls._omit_only_keys:
            nulls = sorted(k for k in cls._omit_only_keys if k in data and data[k] is None)
            if nulls:
                raise ValueError(
                    f"null is not allowed for {', '.join(repr(k) for k in nulls)}: leave it out instead"
                )
        return data


class ContractModel(_OmitOnlyMixin):
    """Strict closed wire contract model (`extra='forbid'`, `strict=True`, rejects explicit `null` on `OmitOnly`)."""

    model_config = ConfigDict(extra="forbid", strict=True)


class OpenContractModel(_OmitOnlyMixin):
    """Launcher-originated payload model: types known fields while allowing future launcher keys."""

    model_config = ConfigDict(extra="allow")


def _exact_int(value: Any) -> Any:
    if type(value) is not int:
        raise ValueError("must be an integer, not a boolean")
    return value


def _exact_bool(value: Any) -> Any:
    if type(value) is not bool:
        raise ValueError("must be true or false")
    return value


def _check_utf8_str(value: str) -> str:
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        raise ValueError(
            "text is not valid Unicode (it has a lone surrogate such as \\ud800): send the text as UTF-8"
        ) from None
    return value


def _check_unique_names(key: str, value: list[str] | None) -> list[str] | None:
    if value is not None and len(set(value)) != len(value):
        raise ValueError(f"{key} names a participant twice")
    return value


ExactLiteral1 = Annotated[Literal[1], BeforeValidator(_exact_int)]
ExactLiteral4 = Annotated[Literal[4], BeforeValidator(_exact_int)]
ExactLiteralTrue = Annotated[Literal[True], BeforeValidator(_exact_bool)]
ExactLiteralFalse = Annotated[Literal[False], BeforeValidator(_exact_bool)]

NonEmptyStr = Annotated[StrictStr, Field(min_length=1)]
Utf8Str = Annotated[StrictStr, AfterValidator(_check_utf8_str)]
NonEmptyUtf8Str = Annotated[Utf8Str, Field(min_length=1)]
NonNegInt = Annotated[StrictInt, Field(ge=0)]
PosInt = Annotated[StrictInt, Field(ge=1)]
CommandId = Annotated[StrictInt, Field(ge=0, le=MAX_ID)]
StepMs = Annotated[StrictInt, Field(ge=1, le=STEP_MS_MAX)]
AtMs = Annotated[StrictInt, Field(ge=0, le=STEP_MS_MAX)]
FiniteFloat = Annotated[StrictFloat, Field(allow_inf_nan=False)]
FiniteNumber = StrictInt | FiniteFloat
SpeedValue = Union[
    Literal["max"],
    Annotated[StrictInt, Field(ge=1, le=int(SPEED_MAX))],
    Annotated[StrictFloat, Field(ge=SPEED_MIN, le=SPEED_MAX, allow_inf_nan=False)],
]
StampStr = Annotated[StrictStr, Field(pattern=STAMP_PATTERN)]
ParamScalar = StrictStr | StrictBool | StrictInt | FiniteFloat
Utf8ParamScalar = Utf8Str | StrictBool | StrictInt | FiniteFloat
LatePolicy = Literal["reject", "apply"]
AfterScenario = Literal["stop", "pause"]

OmitStr = Annotated[StrictStr | None, OmitOnly]
OmitNonEmptyStr = Annotated[NonEmptyStr | None, OmitOnly]
OmitCommandId = Annotated[CommandId | None, OmitOnly]
OmitNonNegInt = Annotated[NonNegInt | None, OmitOnly]
OmitPosInt = Annotated[PosInt | None, OmitOnly]
OmitStepMs = Annotated[StepMs | None, OmitOnly]
OmitAtMs = Annotated[AtMs | None, OmitOnly]
OmitSpeedValue = Annotated[SpeedValue | None, OmitOnly]
OmitAfterScenario = Annotated[AfterScenario | None, OmitOnly]
OmitBool = Annotated[StrictBool, OmitOnly]


# ------------------------------------------------------------ Error catalog & envelopes


class ErrorCode(StrEnum):
    """Stable lowercase-hyphenated error codes across HTTP and WebSocket protocol v4."""

    http_status: int | None

    def __new__(cls, value: str, http_status: int | None = None) -> ErrorCode:
        obj = str.__new__(cls, value)
        obj._value_ = value
        obj.http_status = http_status
        return obj

    # HTTP 400
    INVALID_JSON = ("invalid-json", 400)
    INVALID_FIELD = ("invalid-field", 400)
    INVALID_PATH = ("invalid-path", 400)
    OUTSIDE_ROOT = ("outside-root", 400)
    NOT_A_CONFIGURATION = ("not-a-configuration", 400)
    UNKNOWN_PARTICIPANT = ("unknown-participant", 400)
    UNKNOWN_NETWORK = ("unknown-network", 400)
    TARGET_NOT_LISTED = ("target-not-listed", 400)
    SCENARIO_NOT_LISTED = ("scenario-not-listed", 400)
    UNSUPPORTED = ("unsupported", 400)
    UNKNOWN_VARIABLE = ("unknown-variable", 400)
    NOT_WRITABLE = ("not-writable", 400)
    WRITE_REFUSED = ("write-refused", 400)
    # HTTP 401
    UNAUTHORIZED = ("unauthorized", 401)
    # HTTP 403
    FORBIDDEN_HOST = ("forbidden-host", 403)
    FORBIDDEN_ORIGIN = ("forbidden-origin", 403)
    CORS_NOT_ALLOWED = ("cors-not-allowed", 403)
    SAVE_DISABLED = ("save-disabled", 403)
    COMMANDS_CHANGED = ("commands-changed", 403)
    # HTTP 404
    FILE_NOT_FOUND = ("file-not-found", 404)
    NOT_FOUND = ("not-found", 404)
    NO_TIMELINE = ("no-timeline", 404)
    # HTTP 405
    METHOD_NOT_ALLOWED = ("method-not-allowed", 405)
    # HTTP 409
    ALREADY_RUNNING = ("already-running", 409)
    STARTING = ("starting", 409)
    NOT_RUNNING = ("not-running", 409)
    LAUNCHER_UNSUPPORTED = ("launcher-unsupported", 409)
    SCENARIO_RUNNING = ("scenario-running", 409)
    SESSION_CHANGED = ("session-changed", 409)
    CONFIG_CHANGED = ("config-changed", 409)
    CONFIG_BUSY = ("config-busy", 409)
    VARIABLES_NOT_READY = ("variables-not-ready", 409)
    # HTTP 413
    TOO_LARGE = ("too-large", 413)
    TEXT_TOO_LARGE = ("text-too-large", 413)
    CONFIG_TOO_LARGE = ("config-too-large", 413)
    # HTTP 415
    UNSUPPORTED_MEDIA_TYPE = ("unsupported-media-type", 415)
    # HTTP 422
    CHECK_FAILED = ("check-failed", 422)
    CONFIG_UNREADABLE = ("config-unreadable", 422)
    # HTTP 500
    INTERNAL = ("internal", 500)
    REQUEST_FILE = ("request-file", 500)
    # HTTP 502
    LAUNCHER_START = ("launcher-start", 502)
    LAUNCHER_OUTPUT = ("launcher-output", 502)
    WRITE_FAILED = ("write-failed", 502)
    # HTTP 503
    LAUNCHER_UNUSABLE = ("launcher-unusable", 503)
    CHECK_BUSY = ("check-busy", 503)
    WRITE_BUSY = ("write-busy", 503)
    SHUTTING_DOWN = ("shutting-down", 503)
    TOO_MANY_CONNECTIONS = ("too-many-connections", 503)
    # HTTP 504
    CHECK_TIMEOUT = ("check-timeout", 504)
    WRITE_TIMEOUT = ("write-timeout", 504)
    # WebSocket / run problem codes
    INVALID_MESSAGE = "invalid-message"
    INVALID_WATCH = "invalid-watch"
    TOO_MANY_WATCHES = "too-many-watches"
    BAD_EVENT = "bad-event"
    KEPT_LIMIT = "kept-limit"
    NO_RESULT = "no-result"
    OUTPUT_LEFT_OPEN = "output-left-open"
    STOP_NOT_DELIVERED = "stop-not-delivered"
    STOP_ESCALATED = "stop-escalated"
    LAUNCHER_SILENT = "launcher-silent"
    LAUNCHER_STDERR = "launcher-stderr"
    SEQ_ORDER = "seq-order"
    SEQ_GAP = "seq-gap"
    LINE_TOO_LONG = "line-too-long"
    EVENTS_FORMAT = "events-format"


HTTP_ERROR_CODES: dict[ErrorCode, int] = {c: c.http_status for c in ErrorCode if c.http_status is not None}


class ErrorBody(ContractModel):
    code: NonEmptyStr
    message: NonEmptyStr
    detail: dict[str, Any] | list[Any] | None


class ErrorResponse(ContractModel):
    error: ErrorBody


def error_payload(code: str, message: str, detail: Any = None) -> dict[str, Any]:
    return {"error": {"code": code, "message": message, "detail": detail}}


def model_allowed_fields(model: type[BaseModel]) -> tuple[str, ...]:
    return tuple(info.alias or name for name, info in model.model_fields.items())


def format_validation_error(
    exc: ValidationError,
    *,
    endpoint: str | None = None,
    allowed_fields: Sequence[str] | None = None,
    extra_with_text: bool = False,
) -> tuple[str, str]:
    """Formats a Pydantic ValidationError into `(code, message)` preserving legacy client phrasing."""
    errors = exc.errors()
    if endpoint and allowed_fields is not None:
        extras = sorted({
            str(e["loc"][0])
            for e in errors
            if e["type"] == "extra_forbidden" and len(e["loc"]) == 1
        })
        if extras:
            names = ", ".join(repr(k) for k in extras)
            takes = ", ".join(allowed_fields)
            if extra_with_text:
                return (
                    ErrorCode.INVALID_FIELD,
                    f"{endpoint} takes no {names} together with text: a check of unsaved text checks the "
                    f"configuration only; the fields it takes with text are {takes}",
                )
            return (
                ErrorCode.INVALID_FIELD,
                f"{endpoint} takes no {names}: the fields it takes are {takes}",
            )
    parts: list[str] = []
    for err in errors:
        loc_parts = [str(p) for p in err["loc"] if p != "__root__"]
        msg = err["msg"].removeprefix("Value error, ")
        if err["type"] == "missing" and loc_parts:
            parts.append(f"{'.'.join(loc_parts)} is missing")
        elif loc_parts and not msg.startswith(loc_parts[0]):
            parts.append(f"{'.'.join(loc_parts)}: {msg}")
        else:
            parts.append(msg)
    return ErrorCode.INVALID_FIELD, "; ".join(parts) or "invalid field"


def validate_model_defer_file(
    model: type[_M],
    body: Mapping[str, Any],
) -> tuple[_M, object]:
    """Validates `body` with `model` while deferring invalid/missing `file` and non-string `scenario` to path checks."""
    raw_file = body.get("file")
    patched: dict[str, Any] | None = None
    if not (isinstance(raw_file, str) and raw_file):
        patched = {**body, "file": "__deferred__"}
    if "scenario" in model.model_fields and "scenario" in body:
        raw_scenario = body["scenario"]
        if raw_scenario is not None and not isinstance(raw_scenario, str):
            patched = {**(patched if patched is not None else body), "scenario": "__deferred__"}
    return model.model_validate(patched if patched is not None else body), raw_file


# ------------------------------------------------------------ Shared small sub-models


class RunTarget(ContractModel):
    run_id: NonEmptyStr
    instance: NonEmptyStr


class CautionInfo(ContractModel):
    code: NonEmptyStr
    message: NonEmptyStr


class AllowedItem(ContractModel):
    ok: StrictBool
    reason: StrictStr | None
    message: StrictStr | None


class SaveAllowedItem(AllowedItem):
    caution: CautionInfo | None


class ServerAllowed(ContractModel):
    start: AllowedItem
    stop: AllowedItem
    force: AllowedItem
    save: SaveAllowedItem


class RuntimeStatus(ContractModel):
    mode: Literal["hot", "cold"]
    state: Literal["idle", "preparing", "ready", "running", "resetting", "cold"]
    config: StrictStr | None
    reason: StrictStr | None
    message: StrictStr | None


class VariableRef(ContractModel):
    peer: StrictStr
    name: StrictStr


class WatchVariableRef(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True)
    peer: StrictStr
    name: StrictStr


# ------------------------------------------------------------ POST /api/time contracts


class PauseCommand(ContractModel):
    command: Literal["pause"]
    id: OmitCommandId = None


class ResumeCommand(ContractModel):
    command: Literal["resume"]
    id: OmitCommandId = None


class StepCommand(ContractModel):
    command: Literal["step"]
    ms: StepMs
    id: OmitCommandId = None


class RunUntilCommand(ContractModel):
    command: Literal["run_until"]
    at_ms: AtMs
    id: OmitCommandId = None


class SpeedCommand(ContractModel):
    command: Literal["speed"]
    value: SpeedValue
    id: OmitCommandId = None


TimeCommand = Annotated[
    Union[PauseCommand, ResumeCommand, StepCommand, RunUntilCommand, SpeedCommand],
    Field(discriminator="command"),
]


class _TimeTargetMixin(ContractModel):
    model_config = ConfigDict(
        extra="forbid",
        strict=True,
        json_schema_extra={"dependentRequired": {"run_id": ["instance"], "instance": ["run_id"]}},
    )

    run_id: OmitNonEmptyStr = None
    instance: OmitNonEmptyStr = None

    @model_validator(mode="after")
    def _check_target_pair(self) -> _TimeTargetMixin:
        if (self.run_id is None) != (self.instance is None):
            if self.run_id is None:
                raise ValueError("run_id must be a non-empty string")
            raise ValueError("instance must be the server instance from the run or session answer")
        return self

    def to_launcher_command(self) -> dict[str, Any]:
        data = self.model_dump(exclude={"run_id", "instance"}, exclude_none=True)
        if data["command"] == "speed" and data["value"] != "max":
            data["value"] = float(data["value"])
        return data


class PauseCommandRequest(_TimeTargetMixin, PauseCommand):
    pass


class ResumeCommandRequest(_TimeTargetMixin, ResumeCommand):
    pass


class StepCommandRequest(_TimeTargetMixin, StepCommand):
    pass


class RunUntilCommandRequest(_TimeTargetMixin, RunUntilCommand):
    pass


class SpeedCommandRequest(_TimeTargetMixin, SpeedCommand):
    pass


TimeCommandRequest = Annotated[
    Union[
        PauseCommandRequest,
        ResumeCommandRequest,
        StepCommandRequest,
        RunUntilCommandRequest,
        SpeedCommandRequest,
    ],
    Field(discriminator="command"),
]
TIME_COMMAND_REQUEST_ADAPTER: TypeAdapter[TimeCommandRequest] = TypeAdapter(TimeCommandRequest)


class TimeCommandAck(ContractModel):
    run_id: NonEmptyStr
    command: TimeCommand


def parse_time_command_request(body: Mapping[str, Any]) -> tuple[dict[str, Any], str | None, str | None]:
    """Validates `POST /api/time` body, returning `(normalized_command, run_id, instance)` or raising ValueError."""
    if not isinstance(body, Mapping):
        raise ValueError("the body must be a JSON object")
    if body.get("command") not in TIME_COMMANDS:
        raise ValueError(
            f"command must be one of {', '.join(TIME_COMMANDS)} (to stop, use POST api/stop-session)"
        )
    try:
        req = TIME_COMMAND_REQUEST_ADAPTER.validate_python(body)
    except ValidationError as exc:
        _, message = format_validation_error(exc)
        raise ValueError(message) from None
    return req.to_launcher_command(), req.run_id, req.instance


# ------------------------------------------------------------ HTTP Request Models (17 endpoints)


class ConfigSaveRequest(ContractModel):
    file: NonEmptyStr
    text: Utf8Str
    stamp: StampStr | None


UniqueNameList = Annotated[list[NonEmptyStr], Field(json_schema_extra={"uniqueItems": True})]


class PrepareRequest(ContractModel):
    file: NonEmptyStr
    variant: OmitNonEmptyStr = None
    disable: Annotated[UniqueNameList | None, OmitOnly] = None
    enable: Annotated[UniqueNameList | None, OmitOnly] = None
    manual: Annotated[UniqueNameList | None, OmitOnly] = None

    @field_validator("disable", "enable", "manual")
    @classmethod
    def _unique_participants(cls, value: list[str] | None, info: ValidationInfo) -> list[str] | None:
        return _check_unique_names(info.field_name or "list", value)


class RunRequest(PrepareRequest):
    schema_uri: OmitStr = Field(default=None, alias="$schema")
    vehicle_sil_run_request: Annotated[ExactLiteral1 | None, OmitOnly] = None
    scenario: OmitStr = None
    after_scenario: OmitAfterScenario = None
    params: Annotated[dict[StrictStr, ParamScalar] | None, OmitOnly] = None
    strict: OmitBool = False
    connect: Annotated[dict[NonEmptyStr, StrictStr] | None, OmitOnly] = None
    time_policy: OmitNonEmptyStr = None
    speed: OmitSpeedValue = None
    start_paused: OmitBool = False
    stop_at_ms: OmitStepMs = None
    late_tolerance_ms: OmitAtMs = None


CheckFileRequest = RunRequest


class CheckTextRequest(ContractModel):
    file: NonEmptyStr
    schema_uri: OmitStr = Field(default=None, alias="$schema")
    vehicle_sil_run_request: Annotated[ExactLiteral1 | None, OmitOnly] = None
    variant: OmitNonEmptyStr = None
    strict: OmitBool = False
    text: Utf8Str


CheckRequest = CheckTextRequest | CheckFileRequest


class DescribeRequest(ContractModel):
    file: NonEmptyStr
    variant: OmitNonEmptyStr = None


class ConnectionsRequest(ContractModel):
    file: NonEmptyStr
    network: NonEmptyStr


class StopSessionRequest(ContractModel):
    force: OmitBool = False
    run_id: NonEmptyStr
    instance: NonEmptyStr


PeerWritesMap = Annotated[dict[StrictStr, Any], Field(json_schema_extra={"minProperties": 1})]


class WritePayload(ContractModel):
    writes: Annotated[dict[StrictStr, PeerWritesMap], Field(json_schema_extra={"minProperties": 1})]
    at_ms: AtMs | None = None
    late: Annotated[LatePolicy, OmitOnly] = "reject"

    @field_validator("writes")
    @classmethod
    def _check_writes_non_empty(
        cls,
        value: dict[str, dict[str, Any]],
        info: ValidationInfo,
    ) -> dict[str, dict[str, Any]]:
        if not value or not all(v for v in value.values()):
            raise ValueError(
                'writes must be {"<peer>": {"<variable>": <value>}} with at least one variable for each peer named'
            )
        count = sum(len(v) for v in value.values())
        limits = info.context.get("limits") if isinstance(info.context, dict) else None
        max_writes = getattr(limits, "writes_per_request", DEFAULT_WRITES_PER_REQUEST)
        if count > max_writes:
            raise ValueError(
                f"one request writes at most {max_writes} variables (this one {count}): send more requests"
            )
        return value


class WriteRequest(WritePayload):
    run_id: NonEmptyStr
    instance: OmitNonEmptyStr = None


# ------------------------------------------------------------ HTTP Response Models (17 endpoints)


class LauncherProblem(OpenContractModel):
    code: StrictStr | None = None
    message: StrictStr


class LauncherVersionInfo(OpenContractModel):
    vehicle_sil_launch: StrictInt
    vehicle_sil: StrictStr
    formats: dict[StrictStr, StrictInt] = Field(default_factory=dict)
    features: list[StrictStr] = Field(default_factory=list)


class LauncherInfo(ContractModel):
    path: StrictStr
    usable: StrictBool
    problems: list[LauncherProblem]
    version: LauncherVersionInfo | None


class VariablesAvailability(ContractModel):
    available: StrictBool
    reason: StrictStr | None


class ServerLimits(ContractModel):
    log_lines_per_source: NonNegInt
    check_timeout_s: StrictInt | FiniteFloat
    speed_min: FiniteFloat
    speed_max: FiniteFloat
    step_ms_max: PosInt
    watches: PosInt
    watch_variables: PosInt
    watch_period_ms_min: PosInt
    watch_period_ms_max: PosInt
    writes_per_request: PosInt
    write_at_ms_max: PosInt
    write_timeout_s: StrictInt | FiniteFloat


class AppResponse(ContractModel):
    vehicle_sil_gui: ExactLiteral4
    version: NonEmptyStr
    instance: NonEmptyStr
    root: StrictStr
    initial_file: StrictStr | None
    launcher: LauncherInfo
    variables: VariablesAvailability
    limits: ServerLimits


class ConfigEntry(ContractModel):
    path: NonEmptyStr
    modified: NonEmptyStr
    size: NonNegInt
    simulation: StrictBool


class ConfigsResponse(ContractModel):
    root: StrictStr
    files: list[ConfigEntry]
    truncated: StrictBool


class ConfigGetResponse(ContractModel):
    file: NonEmptyStr
    text: StrictStr
    stamp: StampStr


class ConfigSaveResponse(ContractModel):
    file: NonEmptyStr
    stamp: StampStr


class FindingSummary(OpenContractModel):
    severity: StrictStr
    message: StrictStr
    code: StrictStr | None = None
    file: StrictStr | None = None
    line: StrictInt | None = None
    participant: StrictStr | None = None
    network: StrictStr | None = None
    key: StrictStr | None = None
    detail: Any = None


class CheckParticipant(OpenContractModel):
    name: StrictStr
    line: StrictInt | None = None
    command: list[StrictStr] | None = None
    describe: StrictBool | None = None
    required: StrictBool | None = None
    enabled: StrictBool | None = None
    manual: StrictBool | None = None
    role: StrictStr | None = None
    step_ms: StrictInt | None = None
    status: StrictStr | None = None


class CheckNetwork(OpenContractModel):
    name: StrictStr
    kind: StrictStr | None = None


class CheckScenarioParam(OpenContractModel):
    name: StrictStr
    value: ParamScalar
    from_: StrictStr = Field(alias="from")


class CheckScenario(OpenContractModel):
    file: StrictStr | None = None
    name: StrictStr | None = None
    case: StrictStr | None = None
    cases: list[StrictStr] | None = None
    steps: NonNegInt | None = None
    only: list[StrictStr] | None = None
    params: list[CheckScenarioParam] | None = None
    skipped_steps: NonNegInt | None = None
    unchecked_expects: NonNegInt | None = None
    end_ms: NonNegInt | None = None


class CheckObject(OpenContractModel):
    result: StrictStr | None = None
    participants: list[CheckParticipant] = Field(default_factory=list)
    networks: list[CheckNetwork] | None = None
    findings: list[FindingSummary] = Field(default_factory=list)
    errors: NonNegInt | None = None
    warnings: NonNegInt | None = None
    notes: NonNegInt | None = None
    stop_at_ms: PosInt | None = None
    commands: StrictStr | None = None
    scenario: CheckScenario | None = None


class CheckResponse(ContractModel):
    file: NonEmptyStr
    disable: list[StrictStr]
    manual: list[StrictStr]
    scenario: StrictStr | None
    exit_code: StrictInt
    check: CheckObject


class DescribeResponse(ContractModel):
    file: NonEmptyStr
    variant: StrictStr | None
    exit_code: StrictInt
    check: CheckObject


class ConnectionTarget(OpenContractModel):
    value: StrictStr
    label: StrictStr | None = None
    detail: StrictStr | None = None
    usable: StrictBool
    reason: StrictStr | None = None
    in_use: StrictBool | None = None
    in_use_by: list[StrictStr] | None = None
    in_use_count: NonNegInt | None = None


class ConnectionTargetList(OpenContractModel):
    vehicle_sil_targets: StrictInt | None = None
    tool: StrictStr | None = None
    backend: StrictStr | None = None
    source: StrictStr | None = None
    targets: list[ConnectionTarget] = Field(default_factory=list)


class ConnectionNetwork(OpenContractModel):
    name: StrictStr
    kind: StrictStr | None = None
    tool: StrictStr | None = None
    backend: StrictStr | None = None
    tool_path: StrictStr | None = None
    list: ConnectionTargetList | None = None


class ConnectionsObject(OpenContractModel):
    vehicle_sil_connections: StrictInt | None = None
    vehicle_sil: StrictStr | None = None
    file: StrictStr | None = None
    result: StrictStr | None = None
    networks: list[ConnectionNetwork] = Field(default_factory=list)
    findings: list[FindingSummary] = Field(default_factory=list)


class ConnectionsResponse(ContractModel):
    file: NonEmptyStr
    network: NonEmptyStr
    exit_code: StrictInt
    connections: ConnectionsObject


class PrepareResponse(ContractModel):
    instance: NonEmptyStr


class RunResponse(RunTarget):
    pass


class StopSessionResponse(RunTarget):
    pass


class WriteResponse(ContractModel):
    run_id: NonEmptyStr
    operation: NonNegInt


# ------------------------------------------------------------ Variables & Session Summary models


class VariableEnumItem(ContractModel):
    name: StrictStr
    value: StrictInt


class VariableSpec(ContractModel):
    name: NonEmptyStr
    type: Literal["bool", "integer", "real", "enum"]
    role: Literal["input", "output", "parameter"]
    writable: StrictBool
    builtin: StrictBool
    unit: StrictStr
    description: StrictStr
    min: FiniteNumber | None
    max: FiniteNumber | None
    step: FiniteNumber | None
    enum: list[VariableEnumItem] | None
    availability: Annotated[Literal["active", "inactive", "unknown"] | None, OmitOnly] = None


class VariablePeer(ContractModel):
    name: NonEmptyStr
    connected: StrictBool
    variables: list[VariableSpec]


class OperationPeerResult(ContractModel):
    state: StrictStr
    applied_at_ms: StrictInt | None
    late: StrictBool
    reason: StrictStr | None
    applied_writes: NonNegInt | None
    failed_write: NonNegInt | None


class OperationRecord(ContractModel):
    seq: NonNegInt
    state: StrictStr
    done: StrictBool
    peers: dict[StrictStr, OperationPeerResult]


class VariablesSummary(ContractModel):
    state: Literal["off", "connecting", "ready", "failed", "ended"]
    reason: StrictStr | None
    peers: list[VariablePeer]
    peers_omitted: NonNegInt
    variables_omitted: NonNegInt
    operations: list[OperationRecord]
    operations_from_seq: NonNegInt
    operations_open_omitted: NonNegInt


class SessionLauncherSummary(ContractModel):
    pid: StrictInt | None
    running: StrictBool
    exit_code: StrictInt | None
    signal: StrictInt | None
    silent_s: NonNegInt | None
    mode: Literal["hot", "cold"]
    process_alive: StrictBool


class LogSourceStats(ContractModel):
    kept: NonNegInt
    dropped: NonNegInt


class SessionProblem(ContractModel):
    code: NonEmptyStr
    message: StrictStr
    detail: Any


class ScenarioCallFrame(OpenContractModel):
    file: Utf8Str | None = None
    line: NonNegInt | None = None
    name: Utf8Str | None = None
    params: dict[Utf8Str, Utf8ParamScalar] = Field(default_factory=dict)


class ScenarioStepSummary(OpenContractModel):
    index: PosInt
    status: Utf8Str
    file: Utf8Str | None = None
    line: StrictInt | None = None
    kind: Utf8Str | None = None
    name: Utf8Str | None = None
    text: Utf8Str | None = None
    start_ms: FiniteNumber | None = None
    at_ms: FiniteNumber | None = None
    end_ms: FiniteNumber | None = None
    code: Utf8Str | None = None
    message: Utf8Str | None = None
    call_stack: list[ScenarioCallFrame] | None = None


class SessionScenarioSummary(ContractModel):
    file: NonEmptyUtf8Str
    name: Utf8Str | None
    case: Utf8Str | None = None
    status: Utf8Str | None
    step_count: NonNegInt | None
    over_at_ms: FiniteNumber | None
    steps: list[ScenarioStepSummary]
    steps_omitted: NonNegInt
    timeline_url: Utf8Str | None
    handed_over: StrictBool


class RegistrySummary(OpenContractModel):
    uri: StrictStr | None
    omitted_fields: list[StrictStr] | None = None


class TimeSummary(OpenContractModel):
    event: Literal["time"]
    seq: NonNegInt | None = None
    t_ms: NonNegInt | None = None
    sim_ms: StrictInt
    actual_speed: FiniteNumber | None = None
    late_steps: NonNegInt | None = None
    max_late_ms: NonNegInt | None = None
    slipped_ms: NonNegInt | None = None
    late_ms: NonNegInt | None = None
    omitted_fields: list[StrictStr] | None = None


class CommandSummary(OpenContractModel):
    seq: NonNegInt | None = None
    command: StrictStr | None = None
    source: StrictStr | None = None
    accepted: StrictBool
    message: StrictStr | None = None
    reason: StrictStr | None = None
    ms: PosInt | None = None
    at_ms: NonNegInt | None = None
    asked_run: StrictStr | None = None
    id: CommandId | None = None
    omitted_fields: list[StrictStr] | None = None


class LauncherAllowedItem(OpenContractModel):
    ok: StrictBool
    reason: StrictStr | None
    message: StrictStr | None
    caution: CautionInfo | None


class LauncherStateSummary(OpenContractModel):
    seq: NonNegInt
    rev: CommandId
    allowed: dict[StrictStr, LauncherAllowedItem]
    handed_over: StrictBool = False
    scenario_over_at_ms: NonNegInt | None = None


class ParticipantControlLink(OpenContractModel):
    state: StrictStr
    name: StrictStr | None = None
    variables: NonNegInt | None = None
    version: StrictInt | StrictStr | None = None


class ParticipantSummary(OpenContractModel):
    name: StrictStr
    state: OmitStr = None
    pid: StrictInt | None = None
    exit_code: StrictInt | None = None
    signal: StrictStr | StrictInt | None = None
    reason: StrictStr | None = None
    stop_requested: StrictBool | None = None
    control_link: ParticipantControlLink | None = None
    omitted_fields: list[StrictStr] | None = None


class ResultSummary(OpenContractModel):
    result: StrictStr
    ok: StrictBool
    end: StrictStr | None = None
    message: StrictStr | None = None


class SessionWsSummary(ContractModel):
    """Session summary in WS `session` frames (omits `variables`)."""

    id: NonEmptyStr
    run_t0_ms: NonNegInt | None
    file: NonEmptyStr
    disable: list[StrictStr]
    manual: list[StrictStr]
    speed: SpeedValue | None
    start_paused: StrictBool
    started_at: NonEmptyStr
    ended_at: StrictStr | None
    state: Literal["running", "ended"]
    launcher: SessionLauncherSummary
    stop: Literal["none", "requested", "forced"]
    phase: StrictStr | None
    last_seq: NonNegInt | None
    last_t_ms: NonNegInt | None
    time: TimeSummary | None
    check: CheckObject | None
    registry: RegistrySummary | None
    participants: list[ParticipantSummary]
    participants_omitted: NonNegInt
    findings: list[FindingSummary]
    findings_omitted: NonNegInt
    last_command: CommandSummary | None
    launcher_state: LauncherStateSummary | None
    allowed: ServerAllowed
    result: ResultSummary | None
    log: dict[StrictStr, LogSourceStats]
    problems: list[SessionProblem]
    problems_omitted: NonNegInt
    scenario: SessionScenarioSummary | None
    stop_at_ms: StepMs | None = None


class SessionSummary(SessionWsSummary):
    """Full session summary in `GET /api/session` and WS `hello` (includes `variables`)."""

    variables: VariablesSummary


class SessionResponse(ContractModel):
    instance: NonEmptyStr
    session: SessionSummary | None
    allowed: ServerAllowed
    runtime: RuntimeStatus


# ------------------------------------------------------------ Dashboard / Scenarios / Recording responses


class DashboardErrorInfo(ContractModel):
    line: NonNegInt
    column: NonNegInt
    at: StrictStr
    message: StrictStr


class DashboardPanel(ContractModel):
    module: NonEmptyStr
    url: NonEmptyStr


class DashboardVariableItem(ContractModel):
    variable: VariableRef
    widget: StrictStr | None
    label: StrictStr | None
    options: dict[StrictStr, Any]


class DashboardPanelItem(ContractModel):
    panel: NonEmptyStr
    label: StrictStr | None
    options: dict[StrictStr, Any]
    variables: list[VariableRef]


class DashboardAutoItem(ContractModel):
    auto: NonEmptyStr


DashboardItem = DashboardVariableItem | DashboardPanelItem | DashboardAutoItem


class DashboardSection(ContractModel):
    title: StrictStr | None
    collapsed: StrictBool
    items: list[DashboardItem]


class DashboardDocument(ContractModel):
    title: StrictStr | None
    panels: list[DashboardPanel]
    sections: list[DashboardSection]


class DashboardResponse(ContractModel):
    file: NonEmptyStr
    path: NonEmptyStr
    found: StrictBool
    errors: list[DashboardErrorInfo]
    dashboard: DashboardDocument | None


class ScenarioEntry(ContractModel):
    path: NonEmptyStr
    name: NonEmptyStr
    size: NonNegInt


class ScenariosResponse(ContractModel):
    file: NonEmptyStr
    directory: NonEmptyStr
    found: StrictBool
    scenarios: list[ScenarioEntry]
    truncated: StrictBool


class RecordingSummary(ContractModel):
    run_id: NonEmptyStr
    file: NonEmptyStr
    state: Literal["running", "ended"]
    available: StrictBool
    reason: StrictStr | None
    writes: NonNegInt
    omitted: NonNegInt
    steps: NonNegInt
    end_ms: AtMs | None
    yaml: StrictStr | None


class RecordingResponse(ContractModel):
    recording: RecordingSummary | None


# ------------------------------------------------------------ WebSocket Client -> Server (3 frames)


class WsAuthMessage(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True)
    type: Literal["auth"]
    token: StrictStr
    instance: NonEmptyStr | None = None
    run_id: NonEmptyStr | None = None
    after_stream_seq: NonNegInt | None = None


class WatchPayload(_OmitOnlyMixin):
    model_config = ConfigDict(extra="ignore", strict=True)
    id: CommandId
    variables: Literal["all"] | list[WatchVariableRef]
    period_ms: OmitPosInt = None

    @field_validator("period_ms")
    @classmethod
    def _check_period_bounds(cls, value: int | None, info: ValidationInfo) -> int | None:
        if value is None:
            return None
        limits = info.context.get("limits") if isinstance(info.context, dict) else None
        lo = getattr(limits, "period_ms_min", DEFAULT_PERIOD_MS_MIN)
        hi = getattr(limits, "period_ms_max", DEFAULT_PERIOD_MS_MAX)
        if not lo <= value <= hi:
            raise ValueError(f"period_ms must be a whole number of milliseconds from {lo} to {hi}")
        return value

    @field_validator("variables")
    @classmethod
    def _check_watch_limit(
        cls,
        value: Literal["all"] | list[WatchVariableRef],
        info: ValidationInfo,
    ) -> Literal["all"] | list[WatchVariableRef]:
        if isinstance(value, list):
            limits = info.context.get("limits") if isinstance(info.context, dict) else None
            max_vars = getattr(limits, "watch_variables", DEFAULT_WATCH_VARIABLES)
            if len(value) > max_vars:
                raise ValueError(
                    f'a watch names at most {max_vars} variables (this one {len(value)}): watch "all", or use more watches'
                )
        return value


class WsWatchMessage(WatchPayload):
    type: Literal["watch"]


class WsUnwatchMessage(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True)
    type: Literal["unwatch"]
    id: CommandId


WsClientMessage = Annotated[
    Union[WsAuthMessage, WsWatchMessage, WsUnwatchMessage],
    Field(discriminator="type"),
]


# ------------------------------------------------------------ WebSocket `ws-command-event` (discriminated)


CommandSource = Literal["stdin", "eof", "control_server", "api"]


class _CommandEventBase(ContractModel):
    event: Literal["command"]
    seq: NonNegInt
    t_ms: OmitNonNegInt = None
    run: OmitNonEmptyStr = None
    source: CommandSource
    message: StrictStr | None
    id: CommandId | None


class AcceptedStepCommandEvent(_CommandEventBase):
    command: Literal["step"]
    accepted: ExactLiteralTrue
    reason: Literal[None]
    ms: OmitPosInt = None
    at_ms: OmitNonNegInt = None


class AcceptedOtherCommandEvent(_CommandEventBase):
    command: NonEmptyStr
    accepted: ExactLiteralTrue
    reason: Literal[None]
    at_ms: OmitNonNegInt = None


class RefusedCommandEvent(_CommandEventBase):
    command: StrictStr | None
    accepted: ExactLiteralFalse
    reason: StrictStr | None
    asked_run: StrictStr | None = Field(default=None)


WsCommandEventPayload = AcceptedStepCommandEvent | AcceptedOtherCommandEvent | RefusedCommandEvent


class WsCommandEventMessage(ContractModel):
    type: Literal["event"]
    stream_seq: NonNegInt
    run_id: NonEmptyStr
    event: WsCommandEventPayload


# ------------------------------------------------------------ Events v4 Models (Inbound & Outbound)


class _LauncherEventBase(OpenContractModel):
    event: NonEmptyStr
    seq: CommandId
    t_ms: OmitCommandId = None
    run: OmitNonEmptyStr = None


class RunStartEvent(_LauncherEventBase):
    event: Literal["run_start"]
    run: NonEmptyStr


class PhaseEvent(_LauncherEventBase):
    event: Literal["phase"]
    phase: StrictStr


class InboundCheckParticipant(OpenContractModel):
    name: StrictStr


class InboundCheckObject(OpenContractModel):
    participants: list[InboundCheckParticipant] = Field(default_factory=list)


class CheckEvent(_LauncherEventBase):
    event: Literal["check"]
    check: InboundCheckObject


class WsCheckObject(InboundCheckObject):
    vehicle_sil_check: ExactLiteral1


class WsCheckEvent(_LauncherEventBase):
    event: Literal["check"]
    check: WsCheckObject


class RegistryEvent(_LauncherEventBase):
    event: Literal["registry"]
    uri: StrictStr | None = None


class ParticipantEvent(_LauncherEventBase):
    event: Literal["participant"]
    name: StrictStr
    state: StrictStr
    pid: StrictInt | None = None
    exit_code: StrictInt | None = None


class LogEvent(_LauncherEventBase):
    event: Literal["log"]
    source: StrictStr
    line: StrictStr


class FindingEvent(_LauncherEventBase):
    event: Literal["finding"]
    severity: StrictStr
    message: StrictStr


class TimeEvent(_LauncherEventBase):
    event: Literal["time"]
    sim_ms: StrictInt


class LogDroppedEvent(_LauncherEventBase):
    event: Literal["log_dropped"]
    count: CommandId
    sources: dict[StrictStr, CommandId]


class InboundCommandEvent(_LauncherEventBase):
    event: Literal["command"]
    command: StrictStr | None = None
    accepted: StrictBool
    id: CommandId | None = None


class StateEvent(_LauncherEventBase):
    event: Literal["state"]
    rev: CommandId
    allowed: dict[StrictStr, Any]
    handed_over: StrictBool = False
    scenario_over_at_ms: NonNegInt | None = None


class ResultEvent(_LauncherEventBase):
    event: Literal["result"]
    result: dict[StrictStr, Any]


class ControlLinkEvent(_LauncherEventBase):
    event: Literal["control_link"]
    name: StrictStr
    state: StrictStr


class ScenarioStepEvent(_LauncherEventBase):
    event: Literal["scenario_step"]
    index: PosInt
    status: StrictStr


class HeartbeatEvent(_LauncherEventBase):
    event: Literal["heartbeat"]


class HelloEvent(_LauncherEventBase):
    event: Literal["hello"]


_EVENT_MODELS: dict[str, tuple[type[OpenContractModel], str]] = {
    "run_start": (RunStartEvent, "run is not a string"),
    "phase": (PhaseEvent, "phase is not a string"),
    "check": (CheckEvent, "check is not an object or check.participants is invalid"),
    "registry": (RegistryEvent, "uri is not a string"),
    "participant": (ParticipantEvent, "participant fields have wrong types"),
    "log": (LogEvent, "source or line is not a string"),
    "finding": (FindingEvent, "severity or message is not a string"),
    "time": (TimeEvent, "sim_ms is not an integer"),
    "log_dropped": (LogDroppedEvent, "count or sources is not a count, or an object of counts"),
    "command": (InboundCommandEvent, "command, accepted, or id has wrong type"),
    "state": (StateEvent, "rev is not an integer from 0 to 2^53, or allowed is not an object"),
    "result": (ResultEvent, "result is not an object"),
    "control_link": (ControlLinkEvent, "name or state is not a string"),
    "scenario_step": (ScenarioStepEvent, "index is not a whole number from 1, or status is not a string"),
}
_KNOWN_EVENT_KINDS: tuple[str, ...] = tuple(sorted((*_EVENT_MODELS, "heartbeat", "hello")))
_KNOWN_EVENT_KIND_SET: frozenset[str] = frozenset(_KNOWN_EVENT_KINDS)


class UnknownLauncherEvent(_LauncherEventBase):
    """Forward-compatible fallback strictly for unknown future launcher event kinds."""

    event: NonEmptyStr = Field(json_schema_extra={"not": {"enum": list(_KNOWN_EVENT_KINDS)}})

    @field_validator("event")
    @classmethod
    def _forbid_known_event_kinds(cls, value: str) -> str:
        if value in _KNOWN_EVENT_KIND_SET:
            raise ValueError(f"known event kind {value!r} must match its typed schema")
        return value


class WsStateEvent(_LauncherEventBase):
    event: Literal["state"]
    rev: CommandId
    allowed: dict[StrictStr, LauncherAllowedItem]
    handed_over: StrictBool = False
    scenario_over_at_ms: NonNegInt | None = None


class WsResultEvent(_LauncherEventBase):
    event: Literal["result"]
    result: ResultSummary


LauncherEventPayload = (
    RunStartEvent
    | PhaseEvent
    | WsCheckEvent
    | RegistryEvent
    | ParticipantEvent
    | LogEvent
    | FindingEvent
    | TimeEvent
    | LogDroppedEvent
    | WsCommandEventPayload
    | WsStateEvent
    | WsResultEvent
    | ControlLinkEvent
    | ScenarioStepEvent
    | HeartbeatEvent
    | HelloEvent
    | UnknownLauncherEvent
)


def validate_launcher_event(event: Mapping[str, Any]) -> str | None:
    """Validates a known Events v4 payload; returns None for valid or unknown future events, or a concise reason."""
    kind = event.get("event")
    spec = _EVENT_MODELS.get(kind) if isinstance(kind, str) else None
    if spec is None:
        return None
    model, fallback = spec
    payload = event if "seq" in event else {**event, "seq": 0}
    try:
        model.model_validate(payload)
    except ValidationError:
        return fallback
    return None


# ------------------------------------------------------------ WebSocket Server -> Client (13 frames)


class ReplayInfo(ContractModel):
    after_stream_seq: NonNegInt | None


class WsHelloMessage(ContractModel):
    type: Literal["hello"]
    vehicle_sil_gui: ExactLiteral4
    version: NonEmptyStr
    instance: NonEmptyStr
    stream_seq: NonNegInt
    session: SessionSummary | None
    allowed: ServerAllowed
    replay: ReplayInfo | None
    runtime: RuntimeStatus


class WsSyncedMessage(ContractModel):
    type: Literal["synced"]
    stream_seq: NonNegInt
    run_id: StrictStr | None
    last_seq: NonNegInt | None


class WsSessionMessage(ContractModel):
    type: Literal["session"]
    stream_seq: NonNegInt
    session: SessionWsSummary


class WsEventMessage(ContractModel):
    type: Literal["event"]
    stream_seq: NonNegInt
    run_id: NonEmptyStr
    event: LauncherEventPayload


class WsAllowedMessage(ContractModel):
    type: Literal["allowed"]
    stream_seq: NonNegInt
    run_id: StrictStr | None
    allowed: ServerAllowed


class WsRuntimeMessage(ContractModel):
    type: Literal["runtime"]
    stream_seq: NonNegInt
    runtime: RuntimeStatus


class WsVariablesMessage(ContractModel):
    type: Literal["variables"]
    stream_seq: NonNegInt
    run_id: NonEmptyStr
    variables: VariablesSummary


class PeerValuesSnapshot(ContractModel):
    sample_ms: StrictInt
    values: dict[StrictStr, StrictBool | StrictInt | FiniteFloat | None]


class WsValuesMessage(ContractModel):
    type: Literal["values"]
    id: CommandId
    watch_id: CommandId
    run_id: NonEmptyStr
    sim_ms: StrictInt | None
    paused: StrictBool
    peers: dict[StrictStr, PeerValuesSnapshot]


class WsOperationMessage(OperationRecord):
    type: Literal["operation"]
    stream_seq: NonNegInt
    run_id: NonEmptyStr


class WsAvailabilityMessage(ContractModel):
    type: Literal["availability"]
    stream_seq: NonNegInt
    run_id: NonEmptyStr
    peers: dict[StrictStr, dict[StrictStr, Literal["active", "inactive", "unknown"]]] = Field(min_length=1)


class WsDroppedMessage(ContractModel):
    type: Literal["dropped"]
    run_id: StrictStr | None
    reason: Literal["not-kept", "slow-client"]
    log: dict[StrictStr, NonNegInt]
    at_least: list[StrictStr]
    events: NonNegInt


class WsResyncMessage(ContractModel):
    type: Literal["resync"]
    reason: NonEmptyStr
    message: NonEmptyStr


class WsErrorMessage(ContractModel):
    type: Literal["error"]
    stream_seq: OmitNonNegInt = None
    run_id: StrictStr | None
    code: NonEmptyStr
    message: StrictStr
    detail: Any


WsServerMessage = Annotated[
    Union[
        WsHelloMessage,
        WsSyncedMessage,
        WsSessionMessage,
        WsEventMessage,
        WsAllowedMessage,
        WsRuntimeMessage,
        WsVariablesMessage,
        WsValuesMessage,
        WsOperationMessage,
        WsAvailabilityMessage,
        WsDroppedMessage,
        WsResyncMessage,
        WsErrorMessage,
    ],
    Field(discriminator="type"),
]
WS_SERVER_MESSAGE_ADAPTER: TypeAdapter[WsServerMessage] = TypeAdapter(WsServerMessage)
WS_CLIENT_MESSAGE_ADAPTER: TypeAdapter[WsClientMessage] = TypeAdapter(WsClientMessage)


# ------------------------------------------------------------ CLI `--json` readiness message


class CliReadyMessage(ContractModel):
    vehicle_sil_gui: ExactLiteral4
    url: NonEmptyStr
    port: Annotated[StrictInt, Field(ge=1, le=65535)]
    dev_url: OmitNonEmptyStr = None
    auth: Annotated[ExactLiteralFalse | None, OmitOnly] = None
