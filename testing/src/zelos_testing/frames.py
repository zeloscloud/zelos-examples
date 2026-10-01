"""Receiving and sending frames on a bus under test."""

import threading
import time
from collections.abc import Callable, Iterable

import can


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
