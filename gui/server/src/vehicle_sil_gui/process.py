"""Child processes: started in a group of their own, read line by line, stopped with everything they started.

POSIX: the child leads a new session (process group); stopping it signals the whole group.
Windows: the child gets a new process group (CREATE_NEW_PROCESS_GROUP) so that CTRL_BREAK_EVENT reaches it
and not this server, and the last resort is `taskkill /T /F` (the tree). The Windows branch is written for
the ProactorEventLoop (asyncio's default there) but is not tested yet.
"""

from __future__ import annotations

import asyncio
import os
import signal
import subprocess
import sys
from collections.abc import AsyncIterator, Sequence

WINDOWS = sys.platform == "win32"
READ_CHUNK = 64 * 1024
OUTPUT_GRACE_S = 2.0  # after a child has exited, how long the rest of its output is read
KILL_WAIT_S = 2.0  # after SIGKILL / taskkill, how long the exit is waited for


async def start(argv: Sequence[str], cwd: str | os.PathLike | None, stdin: bool = True) -> asyncio.subprocess.Process:
    kwargs: dict = {}
    if WINDOWS:
        # No CREATE_NO_WINDOW: CTRL_BREAK_EVENT reaches only a child that shares this server's console.
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    return await asyncio.create_subprocess_exec(
        *argv, cwd=cwd, stdin=asyncio.subprocess.PIPE if stdin else asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, **kwargs)


async def run_to_end(argv: Sequence[str], cwd: str | os.PathLike | None, timeout_s: float,
                     max_output: int = 64 * 1024 * 1024,
                     track: set[asyncio.subprocess.Process] | None = None) -> tuple[int, bytes, bytes]:
    """Runs a command with an empty standard input; (exit code, stdout, stderr). On timeout the tree is
    killed and TimeoutError raised. Output beyond max_output bytes is dropped (the end of stderr is kept).
    The end is the process's exit, not the end of its output: what it started may keep the output open, so
    the rest is read for at most OUTPUT_GRACE_S. The process is in `track` while it runs."""
    proc = await start(argv, cwd, stdin=False)
    if track is not None:
        track.add(proc)
    out, err = bytearray(), bytearray()

    async def collect(stream: asyncio.StreamReader, data: bytearray, keep_end: bool) -> None:
        try:
            while chunk := await stream.read(READ_CHUNK):
                data += chunk
                if len(data) > max_output:
                    if keep_end:
                        del data[:len(data) - max_output]
                    else:
                        del data[max_output:]
        except (OSError, ValueError):
            pass

    readers = asyncio.gather(collect(proc.stdout, out, False), collect(proc.stderr, err, True))
    try:
        try:
            async with asyncio.timeout(timeout_s):
                code = await proc.wait()
        except TimeoutError:
            kill_tree(proc)
            await wait_bounded(proc, KILL_WAIT_S)
            raise
        kill_tree(proc)  # what it left in its group (POSIX)
        try:
            async with asyncio.timeout(OUTPUT_GRACE_S):
                await asyncio.shield(readers)
        except TimeoutError:
            pass
        return code, bytes(out), bytes(err)
    except BaseException:
        kill_tree(proc)
        raise
    finally:
        readers.cancel()
        await asyncio.gather(readers, return_exceptions=True)
        close_pipes(proc)
        if track is not None:
            track.discard(proc)


async def wait_bounded(proc: asyncio.subprocess.Process, seconds: float) -> bool:
    """Waits for the process to exit, at most `seconds`; whether it has."""
    try:
        async with asyncio.timeout(seconds):
            await proc.wait()
        return True
    except TimeoutError:
        return False


def close_pipes(proc: asyncio.subprocess.Process) -> None:
    """Closes this side of the child's pipes (a descendant may still hold the other side open)."""
    transport = getattr(proc, "_transport", None)
    if transport is not None and proc.returncode is not None:
        transport.close()


async def lines(stream: asyncio.StreamReader, max_line: int) -> AsyncIterator[str | None]:
    """The lines of a stream, decoded as UTF-8 (a bad byte becomes U+FFFD), without the line end.
    A line longer than max_line bytes is skipped, and None stands in its place."""
    buffer = bytearray()
    skipping = False
    while True:
        chunk = await stream.read(READ_CHUNK)
        if not chunk:
            break
        buffer += chunk
        while True:
            end = buffer.find(b"\n")
            if end < 0:
                if len(buffer) > max_line:
                    if not skipping:
                        yield None
                    skipping = True
                    buffer.clear()
                break
            line = bytes(buffer[:end])
            del buffer[:end + 1]
            if skipping or len(line) > max_line:
                if not skipping:
                    yield None
                skipping = False
                continue
            yield line.rstrip(b"\r").decode("utf-8", "replace")
    if buffer and not skipping:
        yield bytes(buffer).rstrip(b"\r").decode("utf-8", "replace")


def ask_to_stop(proc: asyncio.subprocess.Process) -> None:
    """The polite signal to the whole group: SIGTERM, or CTRL_BREAK_EVENT on Windows."""
    if proc.returncode is not None and WINDOWS:
        return
    try:
        if WINDOWS:
            os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
        else:
            os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def kill_tree(proc: asyncio.subprocess.Process) -> None:
    """Kills the child and what it started: SIGKILL to its process group, or taskkill /T /F on Windows.
    On POSIX it also works after the child has exited, as long as others of its group are left."""
    if WINDOWS:
        if proc.returncode is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            try:
                proc.kill()
            except ProcessLookupError:
                pass
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        pass
