"""The refusal of a request with an HTTP error, shared by the server and the modules it relays for."""

from __future__ import annotations


class Refused(Exception):
    """A request that is refused with an HTTP error (status, code, message, detail)."""

    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail
