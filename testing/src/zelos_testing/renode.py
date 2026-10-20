"""The Renode bench's monitor, for what the bus alone cannot do: take a node off it.

The bench serves the monitor on port 1234 in the network namespace the tester
shares (bench/renode-run.sh). None on hardware.
"""

import contextlib
import re
import socket
import time

PORT = 1234


class Monitor:
    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._read()

    def run(self, *commands: str) -> str:
        for command in commands:
            self._sock.sendall(command.encode() + b"\n")
        return self._read()

    def _read(self, idle_s: float = 0.3) -> str:
        """What the monitor printed, until it falls quiet."""
        out = b""
        self._sock.settimeout(idle_s)
        with contextlib.suppress(TimeoutError):
            while chunk := self._sock.recv(4096):
                out += chunk
        return re.sub(r"\x1b\[[0-9;]*m", "", out.decode(errors="replace"))

    def machines(self) -> list[str]:
        return re.findall(r"^\s*\d+: (\S+)", self.run("mach"), re.MULTILINE)

    def virtual_s(self) -> float:
        """The emulation's own clock, the nodes' time."""
        h, m, s = re.search(r"Elapsed Virtual Time: (\d+):(\d+):([\d.]+)", self.run("emulation GetTimeSourceInfo")).groups()
        return int(h) * 3600 + int(m) * 60 + float(s)

    def sleep_virtual(self, node_s: float, step_s: float = 0.25) -> None:
        end = self.virtual_s() + node_s
        while self.virtual_s() < end:
            time.sleep(step_s)

    @contextlib.contextmanager
    def off_the_bus(self, machine: str):
        """machine's controller off the CAN hub: nothing acknowledges its frames, none reach it."""
        self.run(f'mach set "{machine}"', "connector Disconnect sysbus.fdcan1 canHub")
        try:
            yield
        finally:
            # A controller retransmits a pending frame until acknowledged; Renode's
            # tries one again only on a request, so request the pending (TXBRP) again (TXBAR).
            self.run(f'mach set "{machine}"', "connector Connect sysbus.fdcan1 canHub")
            pending = int(re.findall(r"0x[0-9A-Fa-f]+", self.run("sysbus.fdcan1 ReadDoubleWord 0xCC"))[-1], 16)
            if pending:
                self.run(f"sysbus.fdcan1 WriteDoubleWord 0xD0 {pending:#x}")


def monitor() -> Monitor | None:
    try:
        return Monitor(socket.create_connection(("127.0.0.1", PORT), timeout=2))
    except OSError:
        return None
