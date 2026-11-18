import { reasonText, simulationLabel } from './labels';

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

describe('reasonText', () => {
  it('says the reason of an operation in the words of the screen, by the code the owner gave', () => {
    const reason = (code: string) => ({ code, message: `english ${code}` });
    expect(reasonText('start', reason('already-running'))).toBe('実行中は開始できません。');
    expect(reasonText('stop', reason('not-running'))).toBe('止める実行はありません。');
    expect(reasonText('step', reason('not-running'))).toContain('時刻の操作');
    expect(reasonText('step', reason('not-paused'))).toContain('ステップ送り');
    expect(reasonText('resume', reason('not-paused'))).toBe('一時停止していません。');
  });

  it('passes on the English sentence of a code that it has no words for', () => {
    expect(reasonText('speed', { code: 'outside-real-time', message: 'keeps real time' })).toBe(
      'keeps real time',
    );
  });
});
