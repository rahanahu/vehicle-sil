// The parts of the scenario: choose the one of the next run, show the result of the run's scenario.
import { watch } from '../index';
import type { Front } from '../index';
import { pageIntent } from './page-intent';
import { Part, h, setData } from './part';

/** `<vsil-scenario-select>`: chooses the scenario of the next run, among the scenarios next to the run's configuration file. */
export class VsilScenarioSelectElement extends Part {
  protected draw(client: Front, root: HTMLElement, alive: () => boolean): void {
    const select = h('select', {}, new Option('(no scenario)', ''));
    root.append(select);
    select.addEventListener('change', () => pageIntent.update({ scenario: select.value }));
    this.own(pageIntent.subscribe((intent) => (select.value = intent.scenario ?? '')));
    let file: string | null | undefined;
    this.own(
      watch(client.status, (status) => {
        if (status.file === file) return;
        file = status.file;
        const asked = file;
        const options =
          asked === null
            ? Promise.resolve([])
            : client.low.scenarios(asked).then((list) => list.scenarios);
        options.then(
          (scenarios) => {
            if (!alive() || file !== asked) return;
            select.replaceChildren(new Option('(no scenario)', ''));
            for (const scenario of scenarios) select.add(new Option(scenario.name, scenario.path));
            select.value = pageIntent.get().scenario ?? '';
            if (select.value !== (pageIntent.get().scenario ?? ''))
              pageIntent.update({ scenario: '' });
          },
          () => undefined, // no list: the select keeps what it has
        );
      }),
    );
  }
}

/** `<vsil-scenario-result>`: the result and the steps of the run's scenario. */
export class VsilScenarioResultElement extends Part {
  protected draw(client: Front, root: HTMLElement): void {
    const result = h('output');
    const steps = h('ol');
    root.append(result, steps);
    this.own(
      watch(client.scenario, (scenario) => {
        result.textContent = scenario?.status ?? '-';
        setData(this, 'status', scenario?.status ?? null);
        steps.replaceChildren(
          ...(scenario?.steps ?? []).map((step) => {
            const item = h(
              'li',
              {},
              `${step.text} — ${step.status}${step.at_ms !== null ? ` at ${step.at_ms} ms` : ''}${step.message ? `: ${step.message}` : ''}`,
            );
            setData(item, 'status', step.status);
            return item;
          }),
        );
      }),
    );
  }
}
