"""DoIP (ISO 13400-2) as the suites drive the node: a raw client and a UDS tester.

`Raw` and `udp` are written from the standard's tables, not with a DoIP
library, so they check the entity's bytes. `tester` puts the suites' UdsEcu on
zelos-can's DoIP client (doipclient underneath), as the UDS and OBD extensions
reach an ECU. Host sockets bind the host's address on the node's subnet: on the
hardware bench the default route is another interface.
"""

from __future__ import annotations

import contextlib
import socket
import struct
import time
from typing import Self

PORT = 13400
ECU, TESTER, FUNCTIONAL = 0x1001, 0x0E80, 0xE400
VERSION = 0x02
HEADER = struct.Struct("!BBHI")

GENERIC_NACK = 0x0000
IDENTIFY, IDENTIFY_EID, IDENTIFY_VIN, ANNOUNCEMENT = 0x0001, 0x0002, 0x0003, 0x0004
ACTIVATE, ACTIVATION = 0x0005, 0x0006
ALIVE_CHECK, ALIVE_CHECK_RESPONSE = 0x0007, 0x0008
ENTITY_STATUS, ENTITY_STATUS_RESPONSE = 0x4001, 0x4002
POWER_MODE, POWER_MODE_RESPONSE = 0x4003, 0x4004
DIAGNOSTIC, DIAGNOSTIC_ACK, DIAGNOSTIC_NACK = 0x8001, 0x8002, 0x8003
#: After a reboot: the link (about 1.7 s), then the address's conflict detection (RFC 5227).
ADDRESS_S = 10.0
#: Vehicle announcement / identification response: VIN, address, EID, GID, further action.
VAM = struct.Struct("!17sH6s6sB")


def message(kind: int, payload: bytes = b"", version: int = VERSION) -> bytes:
    return HEADER.pack(version, version ^ 0xFF, kind, len(payload)) + payload


def parse(data: bytes) -> tuple[int, bytes]:
    """(payload type, payload) of one whole datagram or message."""
    version, inverse, kind, length = HEADER.unpack_from(data)
    assert version ^ inverse == 0xFF, data.hex()
    return kind, data[HEADER.size : HEADER.size + length]


def local_address(host: str) -> str:
    """The host's own address on the route to `host`."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.connect((host, PORT))
        return s.getsockname()[0]


def udp(host: str, kind: int, payload: bytes = b"", version: int = VERSION, wait_s: float = 1.0) -> list[tuple[float, str, int, bytes]]:
    """Send one datagram to `host` (an address or the subnet's broadcast) from the
    host's address on that subnet; every answer within wait_s: (seconds after, ip, type, payload)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind((local_address(host), 0))
        sent = time.monotonic()
        s.sendto(message(kind, payload, version), (host, PORT))
        answers = []
        while (left := sent + wait_s - time.monotonic()) > 0:
            s.settimeout(left)
            try:
                data, (ip, _) = s.recvfrom(4096)
            except TimeoutError:
                break
            answers.append((time.monotonic() - sent, ip, *parse(data)))
        return answers


class Raw:
    """One TCP connection to the entity, message by message."""

    def __init__(self, host: str, timeout_s: float = 2.0):
        self.sock = socket.create_connection((host, PORT), timeout_s, source_address=(local_address(host), 0))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.rx = b""

    def close(self) -> None:
        self.sock.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def send(self, kind: int, payload: bytes = b"") -> None:
        self.sock.sendall(message(kind, payload))

    def recv(self, timeout_s: float = 2.0) -> tuple[int, bytes] | None:
        """The next message; None once the entity closed; TimeoutError if none in timeout_s."""
        deadline = time.monotonic() + timeout_s
        while True:
            if len(self.rx) >= HEADER.size:
                size = HEADER.size + HEADER.unpack_from(self.rx)[3]
                if len(self.rx) >= size:
                    data, self.rx = self.rx[:size], self.rx[size:]
                    return parse(data)
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("no DoIP message")
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(4096)
            except ConnectionResetError:
                return None
            if not chunk:
                return None
            self.rx += chunk

    def closed(self, timeout_s: float = 1.0) -> bool:
        """Whether the entity closes the connection within timeout_s, messages before it dropped."""
        with contextlib.suppress(TimeoutError):
            while self.recv(timeout_s) is not None:
                pass
            return True
        return False

    def activate(self, source: int = TESTER, kind: int = 0x00) -> int:
        """Routing activation: the response code."""
        self.send(ACTIVATE, struct.pack("!HBI", source, kind, 0))
        reply = self.recv()
        assert reply is not None and reply[0] == ACTIVATION, reply
        tester, entity, code = struct.unpack_from("!HHB", reply[1])
        assert (tester, entity) == (source, ECU), reply[1].hex()
        return code

    def request(self, data: bytes, source: int = TESTER, target: int = ECU, timeout_s: float = 2.0) -> bytes | None:
        """A UDS request as one diagnostic message: the acknowledgement checked, then the
        response's user data, or None if none comes within timeout_s."""
        self.send(DIAGNOSTIC, struct.pack("!HH", source, target) + data)
        kind, payload = self.recv()
        assert kind == DIAGNOSTIC_ACK, (hex(kind), payload.hex())
        assert payload[:5] == struct.pack("!HHB", target, source, 0), payload.hex()
        try:
            kind, payload = self.recv(timeout_s)
        except TimeoutError:
            return None
        assert kind == DIAGNOSTIC and payload[:4] == struct.pack("!HH", ECU, source), (hex(kind), payload.hex())
        return payload[4:]


def tester(host: str, timeout_s: float):
    """A zelos-can DoIP client, routing activated, that reconnects after the entity closed
    (an ECU reset reboots the node), as a tester does."""
    from zelos_can import doip

    return doip.Client(host, ECU, client_logical_address=TESTER, activation_type=0, connect_timeout=timeout_s)


def connection(client, target: int, timeout_s: float):
    """udsoncan's connection on `client` to `target`; a request that met a closed connection
    reconnects and is sent again once."""
    from zelos_can import doip

    class Reconnecting(doip.Connection):
        def empty_rxqueue(self) -> None:
            try:
                super().empty_rxqueue()
            except doip.Lost:
                self.client.reconnect(close_delay=0)

        def specific_send(self, payload: bytes) -> None:
            try:
                super().specific_send(payload)
            except doip.Lost:
                self.client.reconnect(close_delay=0)
                super().specific_send(payload)

    return Reconnecting(client, TESTER, target, ack_timeout=timeout_s)


def uds_ecu(host: str, timeout_s: float, delay_s: float):
    """The suites' UdsEcu, its requests over DoIP to the node at `host`: physical to the
    ECU's logical address, functional to the functional one."""
    from udsoncan.client import Client
    from zelos_can import doip as zdoip

    from zelos_testing import uds

    class DoipEcu(uds.UdsEcu):
        def __enter__(self):
            self.doip = tester(host, self.timeout_s)
            self.client = self._client(uds.PHYSICAL)
            return self

        def __exit__(self, *exc):
            with contextlib.suppress(Exception):
                self.client.close()
            self.doip.close()

        def wait_up(self, timeout_s: float):
            """After a reset: the address first, reconnecting until the entity takes us."""
            end = time.monotonic() + timeout_s + ADDRESS_S
            while True:
                try:
                    return super().wait_up(max(end - time.monotonic(), 0.0))
                except (OSError, AssertionError, zdoip.DoipError):
                    if time.monotonic() >= end:
                        raise
                    time.sleep(0.5)

        def _client(self, txid: int) -> Client:
            target = ECU if txid == uds.PHYSICAL else FUNCTIONAL
            client = Client(connection(self.doip, target, self.timeout_s), config=self._config())
            client.open()
            return client

    return DoipEcu("doip", host, timeout_s=timeout_s, delay_s=delay_s)
