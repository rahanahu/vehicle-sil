import { OperationName } from '@vehicle-sil/gui-client';
import { reasonText } from '../widgets/control-labels';
import { failureLine, simulationLabel } from './labels';

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

describe('failureLine', () => {
  const failure = (op: OperationName, code: string) => ({
    op,
    problem: { code, message: `english ${code}` },
  });

  it("says that the launcher did not answer a time command in the words of the screen, and passes on the owner's sentence otherwise", () => {
    expect(failureLine(failure('step', 'no-answer'), 5000)).toEqual({
      text: 'ランチャーが命令（step）に 5 秒たっても答えません。入力を読んでいないかもしれません。もう一度送れます。',
      english: false,
    });
    // 開始と停止の no-answer はサーバーが答えなかったこと: 持ち主の文
    expect(failureLine(failure('stop', 'no-answer'), 5000)).toEqual({
      text: 'english no-answer',
      english: true,
    });
    expect(failureLine(failure('resume', 'not-paused'), 5000)).toEqual({
      text: 'english not-paused',
      english: true,
    });
  });

  it("leaves out what is not the failure of an operation, and the time failures that are not the user's mistake", () => {
    expect(failureLine(null)).toBeNull();
    for (const code of ['offline', 'rejected', 'incompatible', 'launcher-crashed', 'run-failed'])
      expect(failureLine(failure('start', code))).toBeNull();
    for (const code of ['interrupted', 'not-running'])
      expect(failureLine(failure('step', code), 5000)).toBeNull();
    // 停止の not-running（止める前に実行が終わった）は、いまどおり帯の下に出す
    expect(failureLine(failure('stop', 'not-running'))?.text).toBe('english not-running');
  });
});
