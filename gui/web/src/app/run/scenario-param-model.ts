import {
  CheckResult,
  CheckScenario,
  CheckScenarioCaseDetail,
  CheckScenarioParam,
  CheckScenarioParamMeta,
  ScenarioParamType,
  ScenarioParamValue,
} from '@vehicle-sil/gui-client';

/** Why a `usage` check result only needs a user selection rather than showing an error count. */
export type SelectionGuidance = 'variant' | 'case' | null;

/** One selectable case option with variant-restriction (`only`) evaluation. */
export interface CaseOption {
  readonly name: string;
  readonly only: readonly string[] | null;
  readonly disabled: boolean;
}

/** Normalized provenance badge category for a scenario parameter. */
export type ParamProvenance =
  | 'override'
  | 'missing'
  | 'scenario'
  | 'configuration'
  | 'variant'
  | 'case'
  | 'default'
  | 'template'
  | 'other';

/** Input widget kind for editing a parameter. */
export type ParamControlKind = 'select' | 'boolean' | 'integer' | 'number' | 'string';

/** View model of one parameter row in the parameter confirmation/override table. */
export interface ParamRow {
  readonly name: string;
  readonly description: string | null;
  readonly type: ScenarioParamType | null;
  readonly values: readonly string[] | null;
  readonly control: ParamControlKind;
  readonly resolvedValue: ScenarioParamValue | undefined;
  readonly effectiveValue: ScenarioParamValue | undefined;
  readonly overridden: boolean;
  readonly missing: boolean;
  readonly provenance: ParamProvenance;
  readonly rawFrom: string | null;
  /** Text shown inside text/number/select controls (reflects in-progress draft when present). */
  readonly displayText: string;
  readonly invalid: boolean;
}

/**
 * Returns whether a `usage` check result is simply asking the user to pick a variant or a scenario case,
 * so the UI can show calm guidance instead of a red error count.
 */
export function checkSelectionGuidance(check: CheckResult | null | undefined): SelectionGuidance {
  if (!check || check.result !== 'usage') return null;
  if ((check.variants?.length ?? 0) > 0 && !check.variant) return 'variant';
  if ((check.scenario?.cases?.length ?? 0) > 0 && !check.scenario?.case) return 'case';
  return null;
}

/** Builds case select options with variant restriction (`case_details[].only`) disabled state. */
export function caseOptionsFor(
  scenario: CheckScenario | null | undefined,
  variant: string | null | undefined,
): CaseOption[] {
  if (!scenario) return [];
  const details: readonly CheckScenarioCaseDetail[] =
    scenario.case_details && scenario.case_details.length > 0
      ? scenario.case_details
      : (scenario.cases ?? []).map((name) => ({ name, only: null }));
  return details.map((detail) => {
    const only = detail.only && detail.only.length > 0 ? detail.only : null;
    const disabled = Boolean(only && variant && !only.includes(variant));
    return { name: detail.name, only, disabled };
  });
}

/** Normalizes a launcher parameter `from` string into a provenance category. */
export function normalizeProvenance(
  from: string | null | undefined,
  overridden: boolean,
  missing: boolean,
): ParamProvenance {
  if (overridden) return 'override';
  if (from === 'request_file' || from === 'command_line' || from === 'request') return 'override';
  if (missing) return 'missing';
  switch (from) {
    case 'scenario':
      return 'scenario';
    case 'configuration':
      return 'configuration';
    case 'variant':
      return 'variant';
    case 'case':
      return 'case';
    case 'template_default':
    case 'default':
      return 'default';
    case 'template_call':
      return 'template';
    default:
      return 'other';
  }
}

/** Picks the input widget kind from template metadata and the resolved value's JS type. */
export function controlKindFor(
  type: ScenarioParamType | null,
  values: readonly string[] | null,
  value: ScenarioParamValue | undefined,
): ParamControlKind {
  if (values && values.length > 0) return 'select';
  if (type === 'boolean') return 'boolean';
  if (type === 'integer') return 'integer';
  if (type === 'number') return 'number';
  if (type === 'string') return 'string';
  if (typeof value === 'boolean') return 'boolean';
  if (typeof value === 'number') return 'number';
  return 'string';
}

/**
 * Coerces a dropdown (`values`) option string into the JSON type expected by the launcher.
 * Never coerces when `type === 'string'`.
 */
export function coerceSelectParamValue(
  raw: string,
  type: ScenarioParamType | null,
  resolvedValue: ScenarioParamValue | undefined,
): ScenarioParamValue {
  if (type === 'string') return raw;
  if (type === 'boolean') {
    if (raw === 'true') return true;
    if (raw === 'false') return false;
    return raw;
  }
  if (type === 'integer' || type === 'number') {
    const trimmed = raw.trim();
    if (trimmed !== '') {
      const n = Number(trimmed);
      if (type === 'integer' ? Number.isSafeInteger(n) : Number.isFinite(n)) return n;
    }
    return raw;
  }
  if (typeof resolvedValue === 'boolean') {
    if (raw === 'true') return true;
    if (raw === 'false') return false;
  }
  if (typeof resolvedValue === 'number' && raw.trim() !== '') {
    const n = Number(raw.trim());
    if (Number.isFinite(n)) return n;
  }
  return raw;
}

const DECIMAL_INT = /^-?\d+$/;
const HEX_INT = /^0[xX][0-9a-fA-F]+$/;
const UNTYPED_NUMBER = /^-?(?:0|[1-9]\d*)(?:\.\d+)?$/;

/**
 * Parses text typed into a parameter input according to its control kind and declared type.
 * - `type: 'string'` always preserves the exact string (e.g. `"0123"`, `"50"`, `"true"`).
 * - `integer` requires a safe integer (decimal or `0x...` hex).
 * - `number` requires a finite number.
 * - Untyped missing parameters (`type === null && resolvedValue === undefined`) parse bare
 *   non-leading-zero numbers and `true`/`false` booleans, keeping leading-zero strings like `"0123"` as string.
 */
export function parseParamText(
  raw: string,
  control: ParamControlKind,
  type: ScenarioParamType | null,
  resolvedValue: ScenarioParamValue | undefined,
): { ok: true; value: ScenarioParamValue } | { ok: false } {
  if (control === 'integer') {
    const trimmed = raw.trim();
    if (!trimmed) return { ok: false };
    if (DECIMAL_INT.test(trimmed)) {
      const n = Number(trimmed);
      return Number.isSafeInteger(n) ? { ok: true, value: n } : { ok: false };
    }
    if (HEX_INT.test(trimmed)) {
      const n = Number.parseInt(trimmed.slice(2), 16);
      return Number.isSafeInteger(n) ? { ok: true, value: n } : { ok: false };
    }
    return { ok: false };
  }
  if (control === 'number') {
    const trimmed = raw.trim();
    if (!trimmed) return { ok: false };
    const n = Number(trimmed);
    return Number.isFinite(n) ? { ok: true, value: n } : { ok: false };
  }
  if (control === 'boolean') {
    if (raw === 'true') return { ok: true, value: true };
    if (raw === 'false') return { ok: true, value: false };
    return { ok: false };
  }
  if (control === 'select') {
    return { ok: true, value: coerceSelectParamValue(raw, type, resolvedValue) };
  }
  // String control: declared string or resolved string NEVER coerces.
  if (type === 'string' || typeof resolvedValue === 'string') {
    return { ok: true, value: raw };
  }
  // Untyped missing parameter: allow convenient numeric/boolean entry while keeping leading-zero codes as strings.
  const trimmed = raw.trim();
  if (trimmed === 'true') return { ok: true, value: true };
  if (trimmed === 'false') return { ok: true, value: false };
  if (UNTYPED_NUMBER.test(trimmed)) {
    const n = Number(trimmed);
    if (Number.isFinite(n)) return { ok: true, value: n };
  }
  return { ok: true, value: raw };
}

/** Formats a resolved parameter scalar for display. */
export function formatParamValue(value: ScenarioParamValue | undefined): string {
  if (value === undefined) return '';
  return typeof value === 'string' ? value : String(value);
}

/**
 * Merges `check.scenario.params`, `param_meta`, `missing_params`, user `overrides`, and in-progress `drafts`
 * into ordered parameter rows. Missing required parameters are listed first for visibility.
 */
export function buildParamRows(
  scenario: CheckScenario | null | undefined,
  overrides: Readonly<Record<string, ScenarioParamValue>>,
  drafts: Readonly<Record<string, string>>,
): ParamRow[] {
  if (!scenario) return [];
  const resolvedByName = new Map<string, CheckScenarioParam>(
    (scenario.params ?? []).map((p) => [p.name, p]),
  );
  const metaByName = new Map<string, CheckScenarioParamMeta>(
    (scenario.param_meta ?? []).map((m) => [m.name, m]),
  );
  const missingSet = new Set(scenario.missing_params ?? []);

  const names: string[] = [];
  const seen = new Set<string>();
  const add = (name: string) => {
    if (!seen.has(name)) {
      seen.add(name);
      names.push(name);
    }
  };
  for (const name of scenario.missing_params ?? []) add(name);
  for (const p of scenario.params ?? []) add(p.name);
  for (const m of scenario.param_meta ?? []) add(m.name);
  for (const name of Object.keys(overrides)) add(name);

  return names.map((name) => {
    const resolved = resolvedByName.get(name);
    const meta = metaByName.get(name);
    const overridden = Object.hasOwn(overrides, name);
    const effectiveValue = overridden ? overrides[name] : resolved?.value;
    const missing = !overridden && (missingSet.has(name) || resolved === undefined);
    const type = (meta?.type ?? resolved?.type ?? null) as ScenarioParamType | null;
    const rawValues = meta?.values ?? resolved?.values ?? null;
    const values = rawValues && rawValues.length > 0 ? rawValues : null;
    const description = meta?.description ?? resolved?.description ?? null;
    const control = controlKindFor(type, values, effectiveValue);
    const hasDraft = Object.hasOwn(drafts, name);
    let displayText: string;
    if (hasDraft) {
      displayText = drafts[name];
    } else if (control === 'select' && values && effectiveValue !== undefined) {
      const formatted = formatParamValue(effectiveValue);
      displayText = values.includes(formatted)
        ? formatted
        : (values.find(
            (opt) => coerceSelectParamValue(opt, type, resolved?.value) === effectiveValue,
          ) ?? formatted);
    } else {
      displayText = formatParamValue(effectiveValue);
    }
    const invalid = hasDraft && !parseParamText(drafts[name], control, type, resolved?.value).ok;
    const provenance = normalizeProvenance(resolved?.from, overridden, missing);
    return {
      name,
      description,
      type,
      values,
      control,
      resolvedValue: resolved?.value,
      effectiveValue,
      overridden,
      missing,
      provenance,
      rawFrom: resolved?.from ?? null,
      displayText,
      invalid,
    };
  });
}

/** Validates a plain JSON object of parameter overrides carried in sessionStorage. */
export function isParamRecord(value: unknown): value is Record<string, ScenarioParamValue> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  return Object.values(value as Record<string, unknown>).every(
    (entry) =>
      typeof entry === 'string' ||
      typeof entry === 'boolean' ||
      (typeof entry === 'number' && Number.isFinite(entry)),
  );
}
