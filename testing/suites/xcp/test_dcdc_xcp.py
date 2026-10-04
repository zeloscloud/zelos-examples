"""The DC-DC node over XCP, tested the way a bench tests its ECU.

Needs the `-S xcp` build and the A2L generated beside its ELF (--a2l). The
suite plays the BMS unless a real one is on the bus. What the node measures
and broadcasts lands in the test's Zelos trace, where `check` asserts on it:
`--zelos-trace-file --zelos-local-artifacts-dir out` writes both.
"""

from itertools import pairwise
from pathlib import Path

import can
import pytest
import zelos_sdk
from zelos_can import CanDecoder

from zelos_testing.dbc import DBC_DIR
from zelos_testing.dcdc import LIMITS, LIMITS_PERIOD_S, STATUS, BmsLimits
from zelos_testing.frames import Periodic, bound, count, play_unless_present, until
from zelos_testing.xcp import Ecu, XcpResponseError

# Below every allowance either BMS grants (a real one: 2 to 6 A by mode), and
# below the node's default demand, so the node visibly settles to it.
LOW_A = 1.5

pytestmark = pytest.mark.usefixtures("bus_health")


@pytest.fixture(scope="module")
def a2l_path(pytestconfig):
    """--a2l, or the one in /elf, where a bench's tester mounts its ELFs."""
    path = pytestconfig.getoption("a2l")
    if path is None:
        found = sorted(Path("/elf").glob("*.a2l"))
        if len(found) != 1:
            pytest.fail(f"{len(found)} A2Ls in /elf: pass --a2l with the one built beside the ELF")
        path = found[0]
    return path


@pytest.fixture
def ecu(pytestconfig, a2l_path):
    with Ecu(pytestconfig.getoption("channel"), a2l_path, timeout_s=bound(2.0)) as ecu:
        yield ecu


@pytest.fixture
def bms(bus):
    """A BMS granting 10 A, unless a real one is on the bus."""
    with play_unless_present(bus, LIMITS.frame_id, BmsLimits(aux=10.0), LIMITS_PERIOD_S, lambda frames: count(frames, STATUS.frame_id) >= 20):
        yield


@pytest.fixture
def status(pytestconfig, bus):
    """DCDC_Status as the node broadcasts it, decoded with the bench DBC into the trace.

    Yields once the node has sent a couple, so `check` has a value from the start.
    """
    source = zelos_sdk.TraceSourceCache("bus")
    decoder = CanDecoder(database_file=str(DBC_DIR / "bench.dbc"), source=source, emit_schemas_on_init=True)
    with can.Bus(interface="socketcan", channel=pytestconfig.getoption("channel")) as decoded:
        notifier = can.Notifier(decoded, [decoder.decode_message])
        try:
            until(bus, lambda frames: len(frames) >= 2, ids=[STATUS.frame_id])
            yield source[f"{STATUS.frame_id:04x}_{STATUS.name}"]
        finally:
            notifier.stop()


def test_epk_matches_a2l(ecu):
    # The A2L is this build's, so its addresses are the ECU's.
    assert ecu.epk() == ecu.a2l["epk"]["string"]


def test_reads_a_measurement(ecu, status, check):
    # What the ECU holds is what it broadcasts, to the frame's 1 degC resolution.
    temperature_c = ecu.read("temperature_c")
    check.that(status.Temperature, "is_close", temperature_c, abs_tol=1.0, temporal="within_duration", duration_s=bound(1.0), name="DCDC_Status.Temperature is temperature_c")


def test_measures_on_its_event(ecu, bms, check):
    # Sampled on the A2L's event, timed by the ECU's own clock.
    with ecu.measuring("input_current_a", "temperature_c") as daq:
        times = daq.wait(21, bound(2.0))
    gaps = [b - a for a, b in pairwise(times)]
    check.that(sum(gaps) / len(gaps), "is_close", ecu.event_period_s, rel_tol=0.02, name="mean sample period, s")
    check.that(max(gaps), "<", 1.5 * ecu.event_period_s, name="no sample missed")
    check.that(ecu.trace[ecu.event["name"]].temperature_c, ">", 0.0, name="temperature_c in the trace")


def test_follows_the_bms_allowance(bus, status, check):
    # Granted less than it demands, the node draws what the BMS allows. Each
    # allowance differs from the last, so a pass is the node's reaction.
    seen = until(bus, lambda frames: frames[-1].arbitration_id == LIMITS.frame_id or count(frames, STATUS.frame_id) >= 20)
    if seen[-1].arbitration_id == LIMITS.frame_id:
        pytest.skip("a BMS is on the bus: the allowance is not the suite's to set")
    bms = BmsLimits(aux=LOW_A)
    with Periodic(bus, bms, LIMITS_PERIOD_S):
        for aux in (LOW_A, LOW_A / 2):
            bms.aux = aux
            check.that(status.InputCurrent, "is_close", aux, abs_tol=0.01, temporal="within_duration", duration_s=bound(2.0), name=f"InputCurrent follows AuxCurrentLimit {aux} A")


def test_calibration_takes_effect(ecu, bms, check):
    # Demand lowered over XCP: the node draws less. The original is put back.
    original = ecu.read("DEMAND_A")
    ecu.write("DEMAND_A", LOW_A)
    try:
        with ecu.measuring("input_current_a"):
            check.that(ecu.trace[ecu.event["name"]].input_current_a, "is_close", LOW_A, abs_tol=0.01, temporal="within_duration", duration_s=bound(2.0), name="input_current_a follows DEMAND_A")
    finally:
        ecu.write("DEMAND_A", original)
    assert ecu.read("DEMAND_A") == original


def test_measurements_are_read_only(ecu):
    with pytest.raises(XcpResponseError):
        ecu.write("input_current_a", 0.0)
