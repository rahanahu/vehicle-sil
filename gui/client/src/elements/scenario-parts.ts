// The parts of the scenario: choose the one of the next run, choose its case, show the result of the run's scenario.
import { watch } from '../index';
import type { Front, ScenarioParamValue } from '../index';
import { pageIntent } from './page-intent';
import { Part, h, setData } from './part';

function formatParamValue(value: ScenarioParamValue): string {
  return typeof value === 'string' ? value : String(value);
}

/** `<vsil-scenario-select>`: chooses the scenario of the next run, among the scenarios next to the run's configuration file. */
export class VsilScenarioSelectElement extends Part {
  protected draw(client: Front, root: HTMLElement, alive: () => boolean): void {
    const select = h('select', {}, new Option('(no scenario)', ''));
    root.append(select);
    select.addEventListener('change', () =>
      pageIntent.update({ scenario: select.value, case: '' }),
    );
    this.own(pageIntent.subscribe((intent) => (select.value = intent.scenario ?? '')));
    let file: string | null | undefined;
    this.own(
      watch(client.status, (status) => {
        if (status.file === file) return;
        file = status.file;
        const asked = file;
        const options = asked === null ? Promise.resolve([]) : client.scenarios({ file: asked });
        options.then(
          (scenarios) => {
            if (!alive() || file !== asked) return;
            select.replaceChildren(new Option('(no scenario)', ''));
            for (const scenario of scenarios) select.add(new Option(scenario.name, scenario.path));
            select.value = pageIntent.get().scenario ?? '';
            if (select.value !== (pageIntent.get().scenario ?? ''))
              pageIntent.update({ scenario: '', case: '' });
          },
          () => undefined, // no list: the select keeps what it has
        );
      }),
    );
  }
}

/** `<vsil-case-select>`: chooses the named case of the selected scenario for the next run. */
export class VsilCaseSelectElement extends Part {
  protected draw(client: Front, root: HTMLElement, alive: () => boolean): void {
    const select = h('select', { disabled: '' }, new Option('(no cases)', ''));
    root.append(select);
    setData(this, 'cases', 'none');
    select.addEventListener('change', () => pageIntent.update({ case: select.value }));

    let key: string | null = null;
    let controller: AbortController | null = null;
    const abort = () => {
      controller?.abort();
      controller = null;
    };
    const resetDom = () => {
      select.replaceChildren(new Option('(no cases)', ''));
      select.disabled = true;
      setData(this, 'cases', 'none');
    };
    const clear = () => {
      resetDom();
      if (pageIntent.get().case) pageIntent.update({ case: '' });
    };
    const refresh = () => {
      const file = client.status.get().file;
      const intent = pageIntent.get();
      const scenario = intent.scenario ?? '';
      const variant = intent.variant ?? '';
      const nextKey = `${file ?? ''}\u0000${scenario}\u0000${variant}`;
      if (nextKey === key) {
        const wanted = intent.case ?? '';
        if ([...select.options].some((o) => o.value === wanted && !o.disabled)) {
          select.value = wanted;
        }
        return;
      }
      key = nextKey;
      abort();
      if (!file || !scenario) {
        clear();
        return;
      }
      resetDom();
      const nextController = new AbortController();
      controller = nextController;
      const checkIntent = variant ? { scenario, variant } : { scenario };
      client.check(checkIntent, { file, signal: nextController.signal }).then(
        (checked) => {
          if (!alive() || key !== nextKey || nextController.signal.aborted) return;
          const cases = checked.scenario?.cases ?? [];
          if (cases.length === 0) {
            clear();
            return;
          }
          const details = new Map(
            (checked.scenario?.case_details ?? []).map((detail) => [detail.name, detail]),
          );
          const activeVariant = variant || (checked.variant ?? '');
          select.replaceChildren(new Option('(choose a case)', ''));
          for (const name of cases) {
            const option = new Option(name, name);
            const only = details.get(name)?.only ?? null;
            if (activeVariant && only && !only.includes(activeVariant)) {
              option.disabled = true;
              option.title = `Only for variants: ${only.join(', ')}`;
            }
            select.add(option);
          }
          select.disabled = false;
          setData(this, 'cases', 'available');
          const wanted = pageIntent.get().case ?? '';
          const valid = [...select.options].some((o) => o.value === wanted && !o.disabled);
          select.value = valid ? wanted : '';
          if (!valid && wanted !== '') pageIntent.update({ case: '' });
        },
        () => {
          if (!alive() || key !== nextKey || nextController.signal.aborted) return;
          clear();
          // Clear key after clear() so clearing pageIntent.case does not immediately re-trigger a retry loop,
          // while allowing a subsequent status or intent transition to recover cleanly.
          key = null;
        },
      );
    };

    this.own(pageIntent.subscribe(refresh));
    this.own(watch(client.status, refresh));
    this.own(() => {
      abort();
      setData(this, 'cases', null);
    });
  }
}

/** `<vsil-scenario-result>`: the result, metadata, and steps (with failure source and call stack) of the run's scenario. */
export class VsilScenarioResultElement extends Part {
  protected draw(client: Front, root: HTMLElement): void {
    const result = h('output');
    const meta = h('div', { class: 'vsil-scenario-meta' });
    meta.hidden = true;
    const steps = h('ol');
    root.append(result, meta, steps);
    this.own(
      watch(client.scenario, (scenario) => {
        result.textContent = scenario?.status ?? '-';
        setData(this, 'status', scenario?.status ?? null);
        setData(this, 'case', scenario?.case ?? null);
        const metaNodes: HTMLElement[] = [];
        if (scenario?.case) {
          metaNodes.push(h('span', { 'data-part': 'case' }, `case: ${scenario.case}`));
        }
        if (scenario?.params && scenario.params.length > 0) {
          const text = scenario.params
            .map((param) => `${param.name}=${formatParamValue(param.value)}`)
            .join(', ');
          metaNodes.push(h('span', { 'data-part': 'params' }, `params: ${text}`));
        }
        meta.replaceChildren(...metaNodes);
        meta.hidden = metaNodes.length === 0;
        steps.replaceChildren(
          ...(scenario?.steps ?? []).map((step) => {
            const label = `${step.name ? `${step.name}: ` : ''}${step.text} — ${step.status}${step.at_ms !== null ? ` at ${step.at_ms} ms` : ''}${step.message ? `: ${step.message}` : ''}`;
            const item = h('li', {}, label);
            if (step.status === 'failed') {
              const hasLine = typeof step.line === 'number' && step.line > 0;
              if (step.file || hasLine) {
                const src = step.file ?? scenario?.file ?? '';
                const loc = hasLine ? (src ? `${src}:${step.line}` : `line ${step.line}`) : src;
                item.append(h('small', { 'data-part': 'source' }, ` (${loc})`));
              }
              if (step.call_stack && step.call_stack.length > 0) {
                item.append(
                  h(
                    'ul',
                    { 'data-part': 'call-stack' },
                    ...step.call_stack.map((frame) => {
                      const where =
                        frame.file && frame.line !== null
                          ? `${frame.file}:${frame.line}`
                          : (frame.file ?? (frame.line !== null ? `line ${frame.line}` : ''));
                      const namePart = frame.name ? (where ? ` (${frame.name})` : frame.name) : '';
                      const entries = Object.entries(frame.params);
                      const paramPart =
                        entries.length > 0
                          ? ` [${entries.map(([k, v]) => `${k}=${formatParamValue(v)}`).join(', ')}]`
                          : '';
                      return h('li', {}, `${where}${namePart}${paramPart}`);
                    }),
                  ),
                );
              }
            }
            setData(item, 'status', step.status);
            return item;
          }),
        );
      }),
    );
    this.own(() => setData(this, 'case', null));
  }
}

/**
 * `<vsil-timeline>`: ties the `<vehicle-sil-timeline>` inside it to the run's recorded timeline (both scenario runs
 * and manual runs with recorded signals). The page's own `<vehicle-sil-timeline>` (with its attributes) is used; when
 * there is none, the part adds a default one. The part changes only `src` and `hidden` of it, and does not load the
 * element's script: the page does.
 */
export class VsilTimelineElement extends Part {
  protected draw(client: Front): void {
    let timeline = this.querySelector<HTMLElement>('vehicle-sil-timeline');
    if (!timeline) {
      const own = (timeline = document.createElement('vehicle-sil-timeline'));
      this.append(own);
      this.own(() => own.remove()); // only what the part made
    }
    const shown = timeline;
    const sessionReadable = client.low?.session?.session;
    const apply = (url: string | null) => {
      shown.hidden = url === null;
      setData(this, 'timeline', url === null ? 'none' : 'shown');
      // The attribute, not the property: it also reaches an element that is defined later. The same file is not set again.
      if (url !== null && shown.getAttribute('src') !== `/${url}`)
        shown.setAttribute('src', `/${url}`);
    };
    this.own(
      sessionReadable
        ? watch([sessionReadable, client.scenario] as const, (session, scenario) =>
            apply(session?.timelineUrl ?? scenario?.timeline_url ?? null),
          )
        : watch(client.scenario, (scenario) => apply(scenario?.timeline_url ?? null)),
    );
    this.own(() => setData(this, 'timeline', null));
  }
}
