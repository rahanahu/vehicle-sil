import { webSocketUrl } from './urls';

describe('webSocketUrl', () => {
  it('resolves against the base href and switches the scheme', () => {
    expect(webSocketUrl('http://127.0.0.1:8765/', 'ws')).toBe('ws://127.0.0.1:8765/ws');
    expect(webSocketUrl('https://example.test/gui/', 'ws')).toBe('wss://example.test/gui/ws');
    expect(webSocketUrl('http://127.0.0.1:8765/gui/smoke', 'ws')).toBe(
      'ws://127.0.0.1:8765/gui/ws',
    );
  });
});
