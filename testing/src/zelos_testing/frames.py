"""Receiving and sending frames on a bus under test."""

import threading
import time
from collections.abc import Callable, Iterable

import can
import pytest

# Host seconds a wait may take per second of the node's time. The plugin sets it
# from --time-scale: 1 on hardware, more under Renode, whose clock runs slow.
TIME_SCALE = 1.0


def bound(node_s: float) -> float:
    """Host seconds to allow for node_s of the node's time. A safety bound only."""
    return node_s * TIME_SCALE


def until(bus: can.BusABC, done: Callable[[list[can.Message]], bool], node_s: float, ids: Iterable[int] | None = None) -> list[can.Message]:
    """Data frames, oldest first, optionally only these IDs, until done(frames) holds.

    done should decide on frames the node sends on its own clock, so the wait is
    in the node's time; node_s only bounds it.
    """
    wanted = None if ids is None else set(ids)
    frames = []
    end = time.monotonic() + bound(node_s)
    while (left := end - time.monotonic()) > 0:
        msg = bus.recv(left)
        if msg is None or msg.is_error_frame or (wanted is not None and msg.arbitration_id not in wanted):
            continue
        frames.append(msg)
        if done(frames):
            return frames
    pytest.fail(f"not done within {node_s} s of node time ({bound(node_s):.0f} s here), after {len(frames)} frames")


def count(frames: list[can.Message], frame_id: int) -> int:
    """How many of frames are on frame_id."""
    return sum(m.arbitration_id == frame_id for m in frames)


def collect(bus: can.BusABC, seconds: float, ids: Iterable[int] | None = None) -> list[can.Message]:
    """Every data frame received for `seconds`, oldest first, optionally only these IDs."""
    wanted = None if ids is None else set(ids)
    frames = []
    end = time.monotonic() + seconds
    while (left := end - time.monotonic()) > 0:
        msg = bus.recv(left)
        if msg is None or msg.is_error_frame:
            continue
        if wanted is None or msg.arbitration_id in wanted:
            frames.append(msg)
    return frames


class Periodic:
    """Send make_msg() every period_s while the block runs, as the node's peer would.

    make_msg is called for each frame, so it can advance a counter or pick up a
    changed value. A send failure is raised when the block exits.
    """

    def __init__(self, bus: can.BusABC, make_msg: Callable[[], can.Message], period_s: float):
        self.bus, self.make_msg, self.period_s = bus, make_msg, period_s
        self.sent = 0
        self._stop = threading.Event()
        self._error: Exception | None = None
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join()
        if self._error is not None and exc[0] is None:
            raise self._error

    def _run(self):
        try:
            while not self._stop.is_set():
                self.bus.send(self.make_msg(), timeout=self.period_s)
                self.sent += 1
                self._stop.wait(self.period_s)
        except can.CanError as err:
            self._error = err
