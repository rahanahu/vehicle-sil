import { HttpErrorResponse } from '@angular/common/http';
import { httpExamples, webSocketExamples } from '../testing/protocol-examples';
import {
  ProtocolError,
  errorMessage,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readLauncherEvent,
  readServerMessage,
  readSessionResponse,
} from './api';

/** 要求のパスごとの、成功の応答を読む関数。 */
const READERS: Record<string, (value: unknown) => unknown> = {
  'GET api/app': readAppInfo,
  'GET api/configs': readConfigList,
  'POST api/check': readCheckResponse,
  'POST api/run': readSessionResponse,
  'POST api/stop': readSessionResponse,
  'GET api/session': readSessionResponse,
};

describe('the readers accept every example of gui/protocol', () => {
  it('reads the HTTP answers, and the message of every error', async () => {
    const examples = await httpExamples();
    expect(examples.length).toBeGreaterThan(10);
    for (const example of examples) {
      const key = `${example.request.method} ${example.request.path}`;
      if (example.status >= 400) {
        const error = new HttpErrorResponse({ error: example.response, status: example.status });
        const message = (example.response as { error: { message: string } }).error.message;
        expect(errorMessage(error), example.name).toBe(message);
        continue;
      }
      const read = READERS[key];
      expect(read, `${example.name}: no reader for ${key}`).toBeDefined();
      expect(() => read(example.response), example.name).not.toThrow();
    }
  });

  it('sends the request bodies the server expects', async () => {
    const bodies = new Map(
      (await httpExamples())
        .filter((example) => example.status === 200 && example.request.method === 'POST')
        .map((example) => [example.name, Object.keys(example.request.body ?? {})]),
    );
    expect(bodies.get('check-passed')?.sort()).toEqual(['disable', 'file']);
    expect(bodies.get('run')?.sort()).toEqual(['disable', 'file', 'speed', 'start_paused']);
    expect(bodies.get('stop-force')).toEqual(['force']);
  });

  it('reads the lines the server skipped in the standard error, and the fields it left out', async () => {
    const examples = await webSocketExamples();
    const stderr = examples
      .flatMap((example) => example.server)
      .map((message) => readServerMessage(message))
      .filter((message) => message?.type === 'error' && message.code === 'launcher-stderr');
    expect(stderr.length).toBeGreaterThan(0);
    expect(
      stderr.every((message) => message?.type === 'error' && message.skipped === undefined),
    ).toBe(true);
    // 間で落とした行の知らせ（例にはない形: 標準エラーに 50 行より多く書くランチャー）
    const gap = readServerMessage({
      type: 'error',
      session: 1,
      code: 'launcher-stderr',
      message: 'the launcher wrote 7 more lines to its standard error here',
      detail: { skipped: 7 },
    });
    expect(gap).toMatchObject({ type: 'error', code: 'launcher-stderr', skipped: 7 });
    expect(
      readServerMessage({
        type: 'error',
        session: 1,
        code: 'x',
        message: 'm',
        detail: { skipped: -1 },
      }),
    ).not.toHaveProperty('skipped');

    const running = (await httpExamples()).find((example) => example.name === 'session-running');
    const raw = (running?.response as { session: Record<string, unknown> }).session;
    const participants = raw['participants'] as Record<string, unknown>[];
    expect(participants.length).toBeGreaterThan(0);
    const summary = readSessionResponse({
      session: {
        ...raw,
        time: { sim_ms: 250, omitted_fields: ['wall_clock'] },
        participants: [{ ...participants[0], omitted_fields: ['environment'] }],
      },
    });
    expect(summary?.time_omitted_fields).toEqual(['wall_clock']);
    expect(summary?.participants[0].omitted_fields).toEqual(['environment']);
    expect(readSessionResponse(running?.response)?.participants[0].omitted_fields).toEqual([]);
  });

  it('reads the launcher state and the stop request of a summary', async () => {
    const summary = readSessionResponse(
      (await httpExamples()).find((example) => example.name === 'run')?.response,
    );
    expect(summary?.launcher.running).toBe(true);
    expect(summary?.stop).toBe('none');
  });

  it('reads every WebSocket message, passing over what it does not know', async () => {
    const examples = await webSocketExamples();
    expect(examples.map((example) => example.name)).toContain('ws-pass');
    for (const example of examples) {
      for (const [i, message] of example.server.entries()) {
        const read = (() => {
          try {
            return readServerMessage(message);
          } catch (error) {
            throw new Error(`${example.name} server[${i}]: ${String(error)}`, { cause: error });
          }
        })();
        const raw = message as { type: string; event?: { event: string } };
        if (raw.type === 'event' && raw.event?.event === 'mystery') {
          expect(read, `${example.name} server[${i}]`).toBeNull();
        } else {
          expect(read?.type, `${example.name} server[${i}]`).toBe(raw.type);
        }
      }
      for (const message of example.client) {
        // 画面が送る最初のメッセージの欄（AuthMessage）
        expect(
          Object.keys(message as object).every((key) =>
            ['type', 'token', 'session', 'from_seq'].includes(key),
          ),
        ).toBe(true);
      }
    }
  });

  it('reads the summary in the hello of a replay', async () => {
    const replay = (await webSocketExamples()).find((example) => example.name === 'ws-replay');
    const hello = readServerMessage(replay?.server[0]);
    expect(hello?.type).toBe('hello');
    if (hello?.type !== 'hello') return;
    expect(hello.session?.state).toBe('ended');
    expect(hello.session?.result?.result).toBe('passed');
    expect(hello.replay?.from_seq).toBeNull();
  });
});

describe('the readers refuse what they cannot read', () => {
  const finding = {
    event: 'finding',
    seq: 3,
    t_ms: 30,
    severity: 'error',
    code: 'x',
    file: null,
    line: null,
    participant: null,
    network: null,
    message: 'm',
    detail: null,
  };

  it('ignores unknown fields and unknown events', () => {
    expect(readLauncherEvent({ ...finding, colour: 'blue' }, 'e')).toMatchObject({
      event: 'finding',
      finding: { code: 'x', message: 'm' },
    });
    expect(readLauncherEvent({ event: 'mystery', seq: 1, t_ms: 0 }, 'e')).toBeNull();
    expect(readServerMessage({ type: 'later', anything: 1 })).toBeNull();
  });

  it('names the place of a missing or wrong field', () => {
    expect(() => readLauncherEvent({ ...finding, message: 5 }, 'e')).toThrow(
      new ProtocolError('e.message', 'a string'),
    );
    expect(() => readLauncherEvent({ event: 'log', seq: 1, t_ms: 0, source: 'A' }, 'e')).toThrow(
      /e\.line: expected a string/,
    );
  });

  it('refuses a format version it does not know', () => {
    expect(() => readServerMessage({ type: 'hello', vehicle_sil_gui: 2 })).toThrow(
      /vehicle_sil_gui: expected format version 1/,
    );
  });

  it('falls back to the status when an error is not in the server form', () => {
    const error = new HttpErrorResponse({ error: 'down', status: 502, statusText: 'Bad Gateway' });
    expect(errorMessage(error)).toBe('HTTP 502 Bad Gateway');
  });
});
