import { readServerMessage } from '@vehicle-sil/gui-client';
import { webSocketExamples } from '@vehicle-sil/gui-client/testing';
import { launcherStderr } from './launcher-stderr';

const line = (text: string) => ({
  code: 'launcher-stderr',
  message: `the launcher wrote: ${text}`,
});

describe('launcherStderr', () => {
  it('takes the lines of the launcher that died from the example, without the prefix', async () => {
    const example = (await webSocketExamples()).find((e) => e.name === 'ws-launcher-crash');
    const problems = (example?.server ?? [])
      .map((message) => readServerMessage(message))
      .flatMap((message) => (message?.type === 'error' ? [message] : []));
    const stderr = launcherStderr(problems);
    expect(stderr).toEqual({
      first: [],
      skipped: 0,
      last: ['fake_launcher: dying on purpose (scenario launcher-crash)'],
    });
  });

  it('has nothing when the launcher wrote nothing to its standard error', () => {
    expect(launcherStderr([{ code: 'no-result', message: 'ended' }])).toBeNull();
  });

  it('puts the first lines before the gap and the last lines after it', () => {
    const problems = [
      line('first 1'),
      { code: 'seq-gap', message: 'a gap' },
      line('first 2'),
      { code: 'launcher-stderr', message: 'the launcher wrote 120 more lines', skipped: 120 },
      line('last 1'),
      line('last 2'),
    ];
    expect(launcherStderr(problems)).toEqual({
      first: ['first 1', 'first 2'],
      skipped: 120,
      last: ['last 1', 'last 2'],
    });
  });
});
