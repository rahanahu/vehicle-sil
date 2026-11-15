/**
 * Shapes of the data exchanged with the server, and their parsers (gui/protocol/ is the authority).
 *
 * The types of the server API are written only in protocol/; users of the library (the screens too) import them from
 * the entry point (index.ts). JSON that arrives (HTTP answers, WebSocket notifications) is used only after a read*
 * function there has checked its type.
 * - protocol/launcher.ts: the shapes the launcher decides (check --json, run --json, run --events).
 * - protocol/server.ts: the shapes the GUI server decides (api/*, WebSocket messages, dashboard files).
 * - protocol/read.ts: building blocks of the parsers, and the rules they follow.
 * The unit tests (protocol.spec.ts) feed every example in gui/protocol/examples/ through these parsers.
 */

export { ProtocolError, Unreadable } from './protocol/read';
export type { Open, Reader } from './protocol/read';
export * from './protocol/launcher';
export * from './protocol/server';
