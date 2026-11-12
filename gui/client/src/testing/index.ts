// 試験のための部品（`@vehicle-sil/gui-client/testing`）。本番のコードからは import しない。
export { FakeHttp, FakeRequest } from './fake-http';
export { FakeSocket, FakeWebSockets } from './fake-web-socket';
export { httpExamples, protocolExamples, webSocketExamples } from './protocol-examples';
export type { HttpExample, WebSocketExample } from './protocol-examples';
export { sessionViewFrom } from './session-view';
