"""The DC-DC as a DoIP entity (ISO 13400-2), checked with a client written from the
standard (zelos_testing.doip.Raw, udp) and with doipclient, through zelos-can.

Identity cross-checked over CAN: the VIN is DID 0xF190, the EID the MAC in DID
0x0111.

    cd testing && uv run pytest suites/doip --channel can0
"""

import itertools
import socket
import struct
import time

import pytest

from zelos_testing import doip, frames
from zelos_testing.doip import (
    ALIVE_CHECK,
    ALIVE_CHECK_RESPONSE,
    ANNOUNCEMENT,
    ECU,
    ENTITY_STATUS,
    ENTITY_STATUS_RESPONSE,
    GENERIC_NACK,
    IDENTIFY,
    IDENTIFY_EID,
    IDENTIFY_VIN,
    POWER_MODE,
    POWER_MODE_RESPONSE,
    TESTER,
    VAM,
    Raw,
)
from zelos_testing.uds import ETH_STATUS, VIN

pytestmark = pytest.mark.usefixtures("bus_health")

# A_DoIP_Announce_Wait, A_DoIP_Announce_Interval, A_DoIP_Announce_Num.
ANNOUNCE_WAIT_S, ANNOUNCE_INTERVAL_S, ANNOUNCE_NUM = 0.5, 0.5, 3
# Host scheduling and the LAN, on top of the entity's timers.
SLACK_S = 0.05
MDS = 4 + 1026


@pytest.fixture(scope="module")
def identity(host, can_ecu_module):
    """(VIN, EID) as the node reports them over CAN."""
    return can_ecu_module.read(VIN).encode(), bytes.fromhex(can_ecu_module.read(ETH_STATUS)["mac"].replace(":", ""))


@pytest.fixture(scope="module")
def can_ecu_module(pytestconfig, uds_server):
    from zelos_testing.uds import UdsEcu

    interface = pytestconfig.getoption("interface")
    interface = "zelos-socketcan" if interface == "socketcan" else interface
    with UdsEcu(interface, pytestconfig.getoption("channel"), timeout_s=frames.bound(2.0)) as e:
        yield e


def vam(payload: bytes) -> tuple:
    return VAM.unpack(payload)


def test_identification(host, identity):
    vin, eid = identity
    for kind, payload in ((IDENTIFY, b""), (IDENTIFY_EID, eid), (IDENTIFY_VIN, vin)):
        answers = doip.udp(host, kind, payload)
        assert [(ip, k) for _, ip, k, _ in answers] == [(host, ANNOUNCEMENT)], answers
        delay, _, _, data = answers[0]
        assert vam(data) == (vin, ECU, eid, eid, 0x00)
        if not frames.SIMULATED:
            assert delay <= ANNOUNCE_WAIT_S + SLACK_S, delay
    # Version 0xFF is for identification requests only.
    assert doip.udp(host, IDENTIFY, version=0xFF)[0][2] == ANNOUNCEMENT
    # Another vehicle's EID or VIN: no answer.
    assert doip.udp(host, IDENTIFY_EID, bytes(6)) == []
    assert doip.udp(host, IDENTIFY_VIN, b"X" * 17) == []


def test_broadcast_identification_has_one_responder(host, identity):
    subnet = host.rsplit(".", 1)[0] + ".255"
    answers = doip.udp(subnet, IDENTIFY, wait_s=2.0)
    assert [(ip, k) for _, ip, k, _ in answers] == [(host, ANNOUNCEMENT)], answers


def test_status_power_mode_and_nacks(host):
    [(_, _, kind, payload)] = doip.udp(host, ENTITY_STATUS)
    # A node, 2 tester sockets, none open, MDS.
    assert (kind, payload) == (ENTITY_STATUS_RESPONSE, struct.pack("!BBBI", 0x01, 2, 0, MDS))
    assert doip.udp(host, POWER_MODE)[0][2:] == (POWER_MODE_RESPONSE, b"\x01")
    assert doip.udp(host, 0x0042)[0][2:] == (GENERIC_NACK, b"\x01")  # unknown type
    assert doip.udp(host, IDENTIFY, b"\x00")[0][2:] == (GENERIC_NACK, b"\x04")  # length
    assert doip.udp(host, IDENTIFY, version=0x07)[0][2:] == (GENERIC_NACK, b"\x00")  # pattern


def test_routing_activation(host):
    with Raw(host) as a:
        assert a.activate(0x1234) == 0x00
        assert a.closed()
    with Raw(host) as a:
        assert a.activate(TESTER, kind=0x01) == 0x06
        assert a.closed()
    with Raw(host) as a, Raw(host) as b:
        assert a.activate() == 0x10
        assert a.activate() == 0x10  # again, the same address
        # The same address on another socket: the entity checks on the first.
        b.send(doip.ACTIVATE, struct.pack("!HBI", TESTER, 0, 0))
        assert a.recv() == (ALIVE_CHECK, b"")
        a.send(ALIVE_CHECK_RESPONSE, struct.pack("!H", TESTER))
        assert b.recv()[1][4] == 0x03
        assert b.closed()
        # Another address on an activated socket.
        assert a.activate(0x0E81) == 0x02
        assert a.closed()


def test_a_silent_socket_loses_its_activation(host):
    with Raw(host) as a, Raw(host) as b:
        assert a.activate() == 0x10
        started = time.monotonic()
        assert b.activate() == 0x10  # a never answered the alive check
        if not frames.SIMULATED:
            assert 0.5 - SLACK_S <= time.monotonic() - started <= 0.5 + 2 * SLACK_S
        assert a.recv() == (ALIVE_CHECK, b"")
        assert a.closed()
        assert b.request(b"\x3e\x00") == b"\x7e\x00"


def test_a_third_tester_is_refused(host):
    with Raw(host) as a, Raw(host) as b, Raw(host) as c:
        assert a.activate(0x0E80) == 0x10
        assert b.activate(0x0E81) == 0x10
        c.send(doip.ACTIVATE, struct.pack("!HBI", 0x0E82, 0, 0))
        for raw, address in ((a, 0x0E80), (b, 0x0E81)):
            assert raw.recv() == (ALIVE_CHECK, b"")
            raw.send(ALIVE_CHECK_RESPONSE, struct.pack("!H", address))
        assert c.recv()[1][4] == 0x01
        assert c.closed()


def test_initial_inactivity(host):
    with Raw(host) as a:
        started = time.monotonic()
        assert a.closed(timeout_s=frames.bound(3.0))
        if not frames.SIMULATED:
            assert 2.0 - SLACK_S <= time.monotonic() - started <= 2.0 + 2 * SLACK_S


def test_diagnostic_messages(host):
    with Raw(host) as a:
        # Not activated: NACK 0x02, closed.
        a.send(doip.DIAGNOSTIC, struct.pack("!HH", TESTER, ECU) + b"\x3e\x00")
        assert a.recv() == (doip.DIAGNOSTIC_NACK, struct.pack("!HHB", ECU, TESTER, 0x02))
        assert a.closed()
    with Raw(host) as a:
        assert a.activate() == 0x10
        assert a.request(b"\x22\xf1\x90") == b"\x62\xf1\x90" + b"1ZLSDCDC0BENCH001"
        # Functional: acknowledged from the functional address, answered from the ECU's.
        assert a.request(b"\x3e\x00", target=doip.FUNCTIONAL) == b"\x7e\x00"
        # Unknown target: NACK 0x03, kept.
        a.send(doip.DIAGNOSTIC, struct.pack("!HH", TESTER, 0x2000) + b"\x3e\x00")
        assert a.recv() == (doip.DIAGNOSTIC_NACK, struct.pack("!HHB", 0x2000, TESTER, 0x03))
        # Above MDS: NACK 0x04, the payload dropped, the stream still in step.
        big = struct.pack("!HH", TESTER, ECU) + bytes(MDS)
        a.send(doip.DIAGNOSTIC, big)
        assert a.recv() == (doip.DIAGNOSTIC_NACK, struct.pack("!HHB", ECU, TESTER, 0x04))
        assert a.request(b"\x3e\x00") == b"\x7e\x00"
        # A broken header: NACK 0x00, closed.
        a.sock.sendall(bytes([0x02, 0x02, 0x80, 0x01, 0, 0, 0, 6]))
        assert a.recv() == (GENERIC_NACK, b"\x00")
        assert a.closed()


def test_announcements_after_a_reset(host, can_ecu_module):
    """Three, 500 ms apart, once the address is back."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("", doip.PORT))
    with listener:
        assert can_ecu_module.raw(b"\x11\x01") == b"\x51\x01"
        seen = []
        end = time.monotonic() + frames.bound(15.0)
        while len(seen) < ANNOUNCE_NUM and (left := end - time.monotonic()) > 0:
            listener.settimeout(left)
            data, (ip, _) = listener.recvfrom(4096)
            if ip == host:
                seen.append((time.monotonic(), doip.parse(data)))
        assert [kind for _, (kind, _) in seen] == [ANNOUNCEMENT] * ANNOUNCE_NUM
        listener.settimeout(frames.bound(1.5))
        with pytest.raises(TimeoutError):
            while listener.recvfrom(4096)[1][0] != host:
                pass
    if not frames.SIMULATED:
        gaps = [b - a for (a, _), (b, _) in itertools.pairwise(seen)]
        assert all(abs(g - ANNOUNCE_INTERVAL_S) <= SLACK_S for g in gaps), gaps
    can_ecu_module.wait_up(frames.bound(5.0))


def test_zelos_can_identifies_and_connects(host, identity):
    """The UDS and OBD extensions' path: zelos-can's identify, connect and Connection."""
    from zelos_can import doip as zdoip

    vin, eid = identity
    vehicle = zdoip.identify(host)
    assert (vehicle.ip, vehicle.logical_address, vehicle.vin, vehicle.eid) == (host, ECU, vin.decode(), eid)
    cfg = zdoip.config({"doip": {"host": host, "ecu_address": "0x1001"}})
    client, ecu = zdoip.connect(cfg, vehicle)
    try:
        conn = zdoip.Connection(client, TESTER, ecu, ack_timeout=2.0)
        conn.open()
        conn.send(b"\x22\xf1\x90")
        assert conn.wait_frame(timeout=2.0) == b"\x62\xf1\x90" + vin
        conn.send(b"\x01\x00")  # OBD over DoIP: PIDs supported
        assert conn.wait_frame(timeout=2.0)[:2] == b"\x41\x00"
    finally:
        client.close()
