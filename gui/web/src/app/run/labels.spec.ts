import { simulationLabel } from './labels';

describe('simulationLabel', () => {
  it('says the scenario ended only when it was over (passed or failed)', () => {
    expect(simulationLabel('ended', 'passed')).toBe('シナリオが終わった');
    expect(simulationLabel('ended', 'failed')).toBe('シナリオが終わった');
    // ended before the scenario was over: a participant stopped it, as in a run without a scenario
    expect(simulationLabel('ended', 'incomplete')).toBe(simulationLabel('ended'));
    expect(simulationLabel('ended', null)).toBe('participant が止めた');
    expect(simulationLabel('reached_stop_at', 'failed')).toBe('stop_at_ms まで動いた');
  });
});
