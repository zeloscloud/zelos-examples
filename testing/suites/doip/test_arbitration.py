"""One UDS server, two transports: a tester on CAN and one on DoIP at once.

The rule (zelos_uds_core_serve(), AUTOSAR Dcm): the tester that leaves the
default session owns it until it is default again; the other gets 0x21 and
cannot keep it alive, but its OBD requests are served. A DoIP owner whose
connection closes gives the session up at once, outputs returned.
"""

import time

import can
import pytest

from zelos_testing import frames
from zelos_testing.frames import bound
from zelos_testing.uds import (
    ACTIVE_SESSION,
    ADJUST,
    DEFAULT,
    EXTENDED,
    FREEZE,
    LEVEL_EXTENDED,
    OUTPUT,
    RESPONSE,
    S3_S,
    SELF_TEST,
    SETPOINT_V,
)

pytestmark = pytest.mark.usefixtures("bus_health")

BUSY = 0x21


def busy(ecu, request: bytes) -> bool:
    return ecu.raw(request) == bytes([0x7F, request[0], BUSY])


def test_a_can_session_holds_doip_off(ecu, can_ecu):
    can_ecu.enter(EXTENDED)
    assert busy(ecu, b"\x10\x03")
    assert busy(ecu, b"\x22\xf1\x86")
    assert ecu.raw(b"\x3e\x80", timeout_s=0.5) is None
    # OBD is served beside it, legacy and OBDonUDS.
    assert ecu.raw(b"\x01\x42")[:2] == b"\x41\x42"
    assert ecu.raw(b"\x22\xf4\x42")[:3] == b"\x62\xf4\x42"
    assert can_ecu.session() == EXTENDED

    # None of it keeps the CAN tester's session: S3 ends it on time.
    last = time.monotonic()
    while time.monotonic() - last < bound(S3_S + 1.0):
        busy(ecu, b"\x3e\x00")
        time.sleep(0.5)
    assert can_ecu.session() == DEFAULT

    # Then DoIP may take it, and CAN waits.
    ecu.enter(EXTENDED)
    assert busy(can_ecu, b"\x22\xf1\x86")
    assert ecu.read(ACTIVE_SESSION) == EXTENDED


def test_closing_the_owner_returns_its_output(ecu, can_ecu):
    ecu.enter(EXTENDED)
    ecu.unlock(LEVEL_EXTENDED)
    ecu.client.io_control(OUTPUT, control_param=ADJUST, values=[12.0])
    assert busy(can_ecu, b"\x22\xf1\x86")

    ecu.doip.close()
    closed = time.monotonic()
    while busy(can_ecu, b"\x22\xf1\x86"):
        assert time.monotonic() - closed < bound(1.0), "the session outlived its tester"
    if not frames.SIMULATED:
        assert time.monotonic() - closed < 0.2
    assert can_ecu.session() == DEFAULT
    # The ECU's own setpoint again, not the closed tester's 12.0 V.
    can_ecu.enter(EXTENDED)
    can_ecu.unlock(LEVEL_EXTENDED)
    assert can_ecu.client.io_control(OUTPUT, control_param=FREEZE).service_data.decoded_data == SETPOINT_V


def test_a_pending_routine_answers_its_own_tester(pytestconfig, ecu, can_ecu):
    ecu.enter(EXTENDED)
    with can.Bus(interface=pytestconfig.getoption("interface"), channel=pytestconfig.getoption("channel")) as bus:
        bus.set_filters([{"can_id": RESPONSE, "can_mask": 0x7FF}])
        with frames.reading(bus) as seen:
            result = ecu.client.start_routine(SELF_TEST).service_data.routine_status_record
    assert len(result) == 1
    # The 0x78 and the result went over DoIP; nothing for it on CAN.
    assert [m for m in seen if m.arbitration_id == RESPONSE] == []
