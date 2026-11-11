"""The screens connected over WebSocket, each with a bounded queue of messages to send.

A screen that reads too slowly does not make the server keep everything for it: when its queue is full, the
queue is emptied, and the screen gets `resync` followed by a fresh hello and replay (what a reconnection gets).
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable

QUEUE_LIMIT = 2000  # messages waiting for one screen
SEND_TIMEOUT_S = 30  # one message that cannot be sent in this time closes the connection


class _Resync:
    pass


RESYNC = _Resync()
CLOSE = _Resync()


class Client:
    def __init__(self, send: Callable[[dict], Awaitable[None]], limit: int = QUEUE_LIMIT):
        self.send = send
        self.queue: asyncio.Queue = asyncio.Queue()
        self.limit = limit
        self.resyncs = 0

    def put(self, message: dict) -> None:
        if self.queue.qsize() >= self.limit:
            self._clear()
            self.queue.put_nowait(RESYNC)
            self.resyncs += 1
            return
        self.queue.put_nowait(message)

    def _clear(self) -> None:
        while not self.queue.empty():
            self.queue.get_nowait()


class Hub:
    def __init__(self, snapshot: Callable[[int | None, int | None], list[dict]], limit: int = QUEUE_LIMIT,
                 max_clients: int = 32):
        """snapshot(session, from_seq) gives hello, the replayed events and synced, in order."""
        self.snapshot = snapshot
        self.limit = limit
        self.max_clients = max_clients
        self.clients: set[Client] = set()

    def full(self) -> bool:
        return len(self.clients) >= self.max_clients

    def broadcast(self, messages: list[dict]) -> None:
        for client in list(self.clients):
            for m in messages:
                client.put(m)

    def close_all(self) -> None:
        for client in self.clients:
            client._clear()
            client.queue.put_nowait(CLOSE)

    async def serve(self, client: Client, session: int | None, from_seq: int | None) -> None:
        """Sends hello, the replay and then every broadcast message until the connection fails or close_all.
        The snapshot is taken in the same step as joining, so nothing is lost or sent twice between them."""
        self.clients.add(client)
        try:
            pending = self.snapshot(session, from_seq)
            while True:
                for message in pending:
                    async with asyncio.timeout(SEND_TIMEOUT_S):
                        await client.send(message)
                item = await client.queue.get()
                if item is CLOSE:
                    return
                if item is RESYNC:
                    client._clear()  # what came after the overflow is in the new snapshot
                    pending = [{"type": "resync", "reason": "slow-client",
                                "message": "this screen read too slowly and messages were dropped: the state "
                                           "is sent again from the start"}] + self.snapshot(None, None)
                else:
                    pending = [item]
        finally:
            self.clients.discard(client)
