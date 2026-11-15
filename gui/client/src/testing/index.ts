// Helpers for tests (`@vehicle-sil/gui-client/testing`). Do not import them from production code.
export { FakeHttp, FakeRequest } from './fake-http';
export { FakeSocket, FakeWebSockets } from './fake-web-socket';
export { httpExamples, protocolExamples, webSocketExamples } from './protocol-examples';
export type { HttpExample, WebSocketExample } from './protocol-examples';
export { sessionViewFrom } from './session-view';
