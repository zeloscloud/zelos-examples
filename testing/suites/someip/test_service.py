"""The DC-DC's SOME/IP service (DcdcService 0x5A00, instance 1) and its SD, checked
with a client written from the PRS (zelos_testing.someip), against the node's CAN
frames and UDS server.

    cd testing && uv run pytest suites/someip --channel can0
"""

import itertools
import socket
import struct
import time

import can
import pytest

from zelos_testing import frames, someip
from zelos_testing.dcdc import STATUS
from zelos_testing.frames import bound
from zelos_testing.someip import (
    ERROR,
    FLAG_REBOOT,
    FLAG_UNICAST,
    INSTANCE,
    MAJOR,
    MINOR,
    NOTIFICATION,
    OFFER,
    PORT,
    RESPONSE,
    SD_GROUP,
    SD_PORT,
    SERVICE,
    SETPOINT_GETTER,
    SETPOINT_NOTIFIER,
    STATUS_EVENT,
    SUBSCRIBE_ACK,
    Client,
    Entry,
)
from zelos_testing.uds import EXTENDED, LEVEL_EXTENDED, SETPOINT, SETPOINT_V

pytestmark = pytest.mark.usefixtures("bus_health")

TTL_S, CYCLIC_S, EVENT_S = 3, 1.0, 0.1
# The node's clock runs about 0.8 % fast; host scheduling on top.
RATE_MARGIN, SLACK_S = 0.02, 0.05
SOAK_S = 10.0
# DCDC_Status's resolutions, for values the CAN frame carries.
RESOLUTION = {"OutputVoltage": 0.01, "OutputCurrent": 0.1, "InputCurrent": 0.01, "Temperature": 1.0}


def offers(flags_entries) -> bool:
    _, entries = flags_entries
    return any(e.kind == OFFER and e.service == SERVICE for e in entries)


@pytest.fixture(scope="module")
def group(host) -> bool:
    """Whether the SD group's traffic reaches this host. The hardware bench's switch
    forwards the node's multicast to IGMPv2 members only, and Linux joins with v3."""
    with Client(host) as c:
        return c.sd_from_node(CYCLIC_S * 2.5, lambda f, e: offers((f, e))) is not None


def find(c: Client) -> tuple[int, Entry, someip.Message] | None:
    """FindService to the group from a socket of its own: the node's unicast answer."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as finder:
        finder.bind((c.local, 0))
        finder.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(c.local))
        finder.settimeout(bound(1.0))
        finder.sendto(someip.sd_message([someip.find_entry()], []), (SD_GROUP, SD_PORT))
        try:
            data, (ip, _) = finder.recvfrom(4096)
        except TimeoutError:
            return None
    assert ip == c.host
    msg = someip.parse(data)
    flags, [entry] = someip.sd_entries(msg)
    return flags, entry, msg


def test_offers(host, group):
    if not group:
        pytest.skip("the SD group does not reach this host (the bench switch); Renode covers it")
    with Client(host) as c:
        seen = []
        while len(seen) < 3:
            got = c.sd_from_node(CYCLIC_S * 2, lambda f, e: offers((f, e)))
            assert got is not None, "no offer"
            seen.append(got)
    for at, flags, entries, msg in seen:
        [e] = entries
        assert (e.service, e.instance, e.major, e.minor_or_eventgroup, e.ttl) == (SERVICE, INSTANCE, MAJOR, MINOR, TTL_S)
        assert e.options == [(host, 0x11, PORT)]
        assert flags & FLAG_UNICAST
    sessions = [m.session for *_, m in seen]
    assert all(b == a + 1 for a, b in itertools.pairwise(sessions)), sessions
    if not frames.SIMULATED:
        gaps = [b[0] - a[0] for a, b in itertools.pairwise(seen)]
        assert all(abs(g - CYCLIC_S) <= CYCLIC_S * RATE_MARGIN + SLACK_S for g in gaps), gaps


def test_find_service_is_answered_unicast(host):
    with Client(host) as c:
        sent = time.monotonic()
        got = find(c)
        took = time.monotonic() - sent
    assert got is not None, "no answer"
    flags, entry, _ = got
    assert (entry.kind, entry.service, entry.ttl) == (OFFER, SERVICE, TTL_S)
    assert entry.options == [(host, 0x11, PORT)]
    assert flags & FLAG_UNICAST
    if not frames.SIMULATED:
        assert took <= 0.1 + SLACK_S, took


def test_events_equal_the_can_frames(pytestconfig, host, can_ecu):
    """DcdcStatus every 100 ms, none missing, each equal to DCDC_Status of its Counter."""
    with can.Bus(interface=pytestconfig.getoption("interface"), channel=pytestconfig.getoption("channel")) as bus, Client(host) as c:
        bus.set_filters([{"can_id": STATUS.frame_id, "can_mask": 0x7FF}])
        with frames.reading(bus) as seen:
            c.subscribe(ttl=0xFFFFFF)
            ack = c.sd_from_node(1.0, lambda f, e: any(x.kind == SUBSCRIBE_ACK for x in e))
            assert ack is not None and ack[2][0].ttl == 0xFFFFFF
            events, setpoint = [], None
            end = time.monotonic() + bound(SOAK_S)
            while time.monotonic() < end:
                got = c.event(1.0)
                assert got is not None, "events stopped"
                at, msg = got
                assert (msg.service, msg.kind, msg.code, msg.interface) == (SERVICE, NOTIFICATION, 0, MAJOR)
                if msg.method == SETPOINT_NOTIFIER:
                    setpoint = msg.payload
                    continue
                assert msg.method == STATUS_EVENT and len(msg.payload) == someip.STATUS.size
                events.append((at, someip.STATUS.unpack(msg.payload)))
            c.subscribe(ttl=0)  # stop
    # The setpoint field's initial value, first, as DID 0x0200 has it.
    assert setpoint == struct.pack("!H", round(can_ecu.read(SETPOINT) * 100))

    counters = [e[1][6] for e in events]
    steps = [(b - a) % 128 for a, b in itertools.pairwise(counters)]
    # Two 50 ms model cycles per event; one or three where the two clocks' phases cross.
    assert set(steps) <= {1, 2, 3} and abs(sum(steps) - 2 * len(steps)) <= 1, steps
    if not frames.SIMULATED:
        period = (events[-1][0] - events[0][0]) / (len(events) - 1)
        assert abs(period - EVENT_S) <= EVENT_S * RATE_MARGIN, period

    can_frames = [(m.timestamp, STATUS.decode(m.data)) for m in seen if m.arbitration_id == STATUS.frame_id]
    matched = 0
    for at, (out_v, out_a, _in_v, in_a, temp, state, counter, _uptime) in events:
        # The Counter wraps every 6.4 s: its frame is the one nearest in time.
        same = sorted((abs(t - at), i) for i, (t, f) in enumerate(can_frames) if f["Counter"] == counter)
        if not same or same[0][0] > bound(1.0):
            continue
        frame = can_frames[same[0][1]][1]
        matched += 1
        for name, value in (("OutputVoltage", out_v), ("OutputCurrent", out_a), ("InputCurrent", in_a), ("Temperature", temp)):
            assert abs(frame[name] - value) <= RESOLUTION[name], (counter, name, frame[name], value)
        assert (frame["DerateActive"] == 1) == (state == 2), (counter, frame["DerateActive"], state)
    assert matched >= len(events) - 2, f"{matched} of {len(events)} events had their CAN frame"


def test_setpoint_getter_and_notifier(host, can_ecu):
    with Client(host) as c:
        reply = c.call(SETPOINT_GETTER)
        assert (reply.kind, reply.code, reply.payload) == (RESPONSE, 0, struct.pack("!H", round(SETPOINT_V * 100)))
        c.subscribe(ttl=0xFFFFFF)
        assert c.sd_from_node(1.0, lambda f, e: any(x.kind == SUBSCRIBE_ACK for x in e)) is not None
        can_ecu.enter(EXTENDED)
        can_ecu.unlock(LEVEL_EXTENDED)
        try:
            written = time.time()
            can_ecu.client.write_data_by_identifier(SETPOINT, 12.0)
            while True:
                got = c.event(1.0)
                assert got is not None, "no notification"
                at, msg = got
                if msg.method == SETPOINT_NOTIFIER and msg.payload == struct.pack("!H", 1200):
                    break
            if not frames.SIMULATED:
                assert at - written <= 0.15, at - written
            assert c.call(SETPOINT_GETTER).payload == struct.pack("!H", 1200)
        finally:
            can_ecu.client.write_data_by_identifier(SETPOINT, SETPOINT_V)
            c.subscribe(ttl=0)


def test_communication_control_stops_events_only(host, can_ecu):
    with Client(host) as c:
        c.subscribe(ttl=0xFFFFFF)
        assert c.sd_from_node(1.0, lambda f, e: any(x.kind == SUBSCRIBE_ACK for x in e)) is not None
        assert c.event(1.0) is not None
        can_ecu.enter(EXTENDED)
        can_ecu.unlock(LEVEL_EXTENDED)
        try:
            can_ecu.client.communication_control(0x01, 0x01)  # enableRxAndDisableTx, normal
            # Drain what was already on its way, then none.
            while c.event(0.3) is not None:
                pass
            assert c.event(1.0) is None
            assert find(c) is not None
            assert c.call(SETPOINT_GETTER).kind == RESPONSE
        finally:
            can_ecu.client.communication_control(0x00, 0x01)
        assert c.event(1.0) is not None
        c.subscribe(ttl=0)


def test_errors_and_refusals(host):
    with Client(host) as c:
        unknown = c.call(0x0042)
        assert (unknown.kind, unknown.code) == (ERROR, 0x03)  # unknown method
        assert c.call(SETPOINT_GETTER, interface=2).code == 0x08  # wrong interface version
        assert c.call(SETPOINT_GETTER, service=0x1234).code == 0x02  # unknown service
        assert c.call(SETPOINT_GETTER, protocol=2).code == 0x07  # wrong protocol version
        assert c.call(SETPOINT_GETTER, kind=someip.REQUEST_NO_RETURN) is None
        # An eventgroup it does not have: a negative acknowledgement (TTL 0).
        c.subscribe(eventgroup=0x0009)
        got = c.sd_from_node(1.0, lambda f, e: any(x.kind == SUBSCRIBE_ACK for x in e))
        assert got is not None
        [nack] = got[2]
        assert (nack.minor_or_eventgroup, nack.ttl) == (0x0009, 0)
        assert c.event(0.5) is None


def test_reboot_stops_the_offer_and_sets_the_reboot_flag(host, group, can_ecu):
    with Client(host) as c:
        assert find(c) is not None
        assert can_ecu.raw(b"\x11\x01") == b"\x51\x01"
        if group:
            stop = c.sd_from_node(1.0, lambda f, e: offers((f, e)))
            assert stop is not None and stop[2][0].ttl == 0, "no StopOffer"
            first = c.sd_from_node(15.0, lambda f, e: offers((f, e)))
            assert first is not None, "no offer after the reboot"
            _, flags, entries, msg = first
            assert flags & FLAG_REBOOT and msg.session == 1 and entries[0].ttl == TTL_S
        # Unicast too: the first answer after the reboot is session 1, Reboot set. The
        # node answers for a moment after the reset's response: wait for its reboot.
        can_ecu.wait_up(bound(5.0))
        end = time.monotonic() + bound(15.0)
        while (got := find(c)) is None:
            assert time.monotonic() < end, "no answer after the reboot"
        flags, _, msg = got
        assert flags & FLAG_REBOOT and msg.session == 1, (hex(flags), msg.session)
