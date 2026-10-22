"""SOME/IP and SOME/IP-SD (AUTOSAR PRS) as the suites drive the node, written from
the PRS, not with a SOME/IP library, so they check the server's bytes.

Every socket is bound to the host's address on the node's subnet, and multicast
goes out and is joined there: on the hardware bench the default route is
another interface.
"""

from __future__ import annotations

import socket
import struct
import time
from dataclasses import dataclass
from typing import Self

from zelos_testing import frames
from zelos_testing.doip import local_address

SD_PORT, SD_GROUP = 30490, "224.224.224.245"
SD_SERVICE, SD_METHOD = 0xFFFF, 0x8100
HEADER = struct.Struct("!HHIHHBBBB")
REQUEST, REQUEST_NO_RETURN, NOTIFICATION, RESPONSE, ERROR = 0x00, 0x01, 0x02, 0x80, 0x81
FIND, OFFER, SUBSCRIBE, SUBSCRIBE_ACK = 0x00, 0x01, 0x06, 0x07
FLAG_REBOOT, FLAG_UNICAST = 0x80, 0x40
#: The node's service (arxml/can-full.arxml).
SERVICE, INSTANCE, MAJOR, MINOR, PORT, EVENTGROUP = 0x5A00, 0x0001, 1, 0, 30509, 0x0001
STATUS_EVENT, SETPOINT_GETTER, SETPOINT_NOTIFIER = 0x8001, 0x0001, 0x8002
#: DcdcStatus: output V, output A, input V, input A, temperature, state, counter, uptime.
STATUS = struct.Struct("!5fBBI")


@dataclass
class Message:
    service: int
    method: int
    client: int
    session: int
    protocol: int
    interface: int
    kind: int
    code: int
    payload: bytes


def message(service: int, method: int, payload: bytes = b"", kind: int = REQUEST, session: int = 1, client: int = 0x0001, interface: int = MAJOR, protocol: int = 1) -> bytes:
    return HEADER.pack(service, method, 8 + len(payload), client, session, protocol, interface, kind, 0) + payload


def parse(data: bytes) -> Message:
    service, method, length, client, session, protocol, interface, kind, code = HEADER.unpack_from(data)
    assert length == len(data) - 8, data.hex()
    return Message(service, method, client, session, protocol, interface, kind, code, data[HEADER.size :])


@dataclass
class Entry:
    kind: int
    service: int
    instance: int
    major: int
    ttl: int
    minor_or_eventgroup: int
    options: list[tuple[str, int, int]]


def sd_message(entries: list[bytes], options: list[bytes], session: int = 1, flags: int = FLAG_REBOOT | FLAG_UNICAST) -> bytes:
    body = struct.pack("!B3xI", flags, 16 * len(entries)) + b"".join(entries)
    opts = b"".join(options)
    body += struct.pack("!I", len(opts)) + opts
    return message(SD_SERVICE, SD_METHOD, body, kind=NOTIFICATION, session=session, client=0, interface=1)


def endpoint_option(address: str, port: int) -> bytes:
    return struct.pack("!HBB4sBBH", 9, 0x04, 0, socket.inet_aton(address), 0, 0x11, port)


def find_entry(service: int = SERVICE) -> bytes:
    return struct.pack("!BBBBHHB3sI", FIND, 0, 0, 0, service, 0xFFFF, 0xFF, (3).to_bytes(3, "big"), 0xFFFFFFFF)


def subscribe_entry(ttl: int, eventgroup: int = EVENTGROUP, options: int = 1) -> bytes:
    return struct.pack("!BBBBHHB3sHH", SUBSCRIBE, 0, 0, options << 4, SERVICE, INSTANCE, MAJOR, ttl.to_bytes(3, "big"), 0, eventgroup)


def sd_entries(msg: Message) -> tuple[int, list[Entry]]:
    """(flags, entries with their endpoint options as (ip, protocol, port))."""
    assert (msg.service, msg.method, msg.kind) == (SD_SERVICE, SD_METHOD, NOTIFICATION)
    p = msg.payload
    flags, entries_len = struct.unpack_from("!B3xI", p)
    options_len = struct.unpack_from("!I", p, 8 + entries_len)[0]
    raw_options, at = [], 8 + entries_len + 4
    while at < 8 + entries_len + 4 + options_len:
        size = struct.unpack_from("!H", p, at)[0] + 3
        raw_options.append(p[at : at + size])
        at += size
    entries = []
    for e in range(0, entries_len, 16):
        kind, i1, i2, n, service, instance, major, ttl, last = struct.unpack_from("!BBBBHHB3sI", p, 8 + e)
        refs = [*range(i1, i1 + (n >> 4)), *range(i2, i2 + (n & 0x0F))]
        options = []
        for o in (raw_options[r] for r in refs):
            _, kind_o, _, ip, _, proto, port = struct.unpack("!HBB4sBBH", o)
            assert kind_o == 0x04, o.hex()
            options.append((socket.inet_ntoa(ip), proto, port))
        group = last & 0xFFFF if kind in (SUBSCRIBE, SUBSCRIBE_ACK) else last
        entries.append(Entry(kind, service, instance, major, int.from_bytes(ttl, "big"), group, options))
    return flags, entries


class Client:
    """An SD socket (on the group, the host's address) and an event socket."""

    def __init__(self, host: str):
        self.host, self.local = host, local_address(host)
        self.sd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sd.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sd.bind(("", SD_PORT))
        group = socket.inet_aton(SD_GROUP) + socket.inet_aton(self.local)
        self.sd.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, group)
        self.sd.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.local))
        self.events = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.events.bind((self.local, 0))
        self.event_port = self.events.getsockname()[1]
        self.session = 0

    def close(self) -> None:
        self.sd.close()
        self.events.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _next(self) -> int:
        self.session += 1
        return self.session

    def send_sd(self, entries: list[bytes], options: list[bytes], to: str | None = None) -> None:
        self.sd.sendto(sd_message(entries, options, self._next()), (to or self.host, SD_PORT))

    def subscribe(self, ttl: int = 3, eventgroup: int = EVENTGROUP) -> None:
        self.send_sd([subscribe_entry(ttl, eventgroup)], [endpoint_option(self.local, self.event_port)])

    def sd_from_node(self, timeout_s: float, want=lambda flags, entries: True) -> tuple[float, int, list[Entry], Message] | None:
        """The next SD message from the node that `want` takes, within timeout_s of node time."""
        end = time.monotonic() + frames.bound(timeout_s)
        while (left := end - time.monotonic()) > 0:
            self.sd.settimeout(left)
            try:
                data, (ip, _) = self.sd.recvfrom(4096)
            except TimeoutError:
                return None
            if ip != self.host:
                continue
            msg = parse(data)
            flags, entries = sd_entries(msg)
            if want(flags, entries):
                return time.monotonic(), flags, entries, msg
        return None

    def event(self, timeout_s: float) -> tuple[float, Message] | None:
        """The next message on the event socket, within timeout_s of node time, and its
        arrival on the wall clock (as python-can stamps frames)."""
        self.events.settimeout(frames.bound(timeout_s))
        try:
            data, (ip, _) = self.events.recvfrom(4096)
        except TimeoutError:
            return None
        assert ip == self.host
        return time.time(), parse(data)

    def call(self, method: int, payload: bytes = b"", **kwargs) -> Message | None:
        """A request to the service's port; the reply, or None."""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind((self.local, 0))
            s.settimeout(frames.bound(1.0))
            s.sendto(message(kwargs.pop("service", SERVICE), method, payload, session=self._next(), **kwargs), (self.host, PORT))
            try:
                return parse(s.recvfrom(4096)[0])
            except TimeoutError:
                return None
