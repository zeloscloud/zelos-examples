"""The DC-DC node over raw CAN: its status frame, and how it follows BMS_Limits.

This suite plays the BMS, so it needs the DC-DC without one. Each test sets the
allowance it starts from, so none depends on what the node did before.
"""

import time

import can
import pytest

from zelos_testing import dbc
from zelos_testing.frames import Periodic, collect

DB = dbc.load("bench")
STATUS = DB.get_message_by_name("DCDC_Status")
LIMITS = DB.get_message_by_name("BMS_Limits")
PERIOD_S = STATUS.cycle_time / 1000

# From nodes/dcdc: what the 12 V loads draw, and the slew cap per status frame.
DEMAND_A = 4.5
STEP_A = 10.0 * PERIOD_S
# Renode's virtual time is not tied to the host clock and runs several times
# slower on a loaded host, so waits are bounded generously and exit early.
TIMEOUT_S = 20.0

pytestmark = pytest.mark.usefixtures("bus_health")


def current(frame) -> float:
    return round(frame[1]["InputCurrent"], 2)


def status(bus, seconds):
    """DCDC_Status frames over `seconds`, as (timestamp, signals)."""
    return [(m.timestamp, STATUS.decode(m.data)) for m in collect(bus, seconds, [STATUS.frame_id])]


def until(bus, done):
    """DCDC_Status frames until done(frames) holds."""
    frames = []
    end = time.monotonic() + TIMEOUT_S
    while time.monotonic() < end:
        frames += status(bus, 0.25)
        if frames and done(frames):
            return frames
    pytest.fail(f"timed out; InputCurrent {[current(f) for f in frames[-20:]]}")


def settle(bus, amps):
    """DCDC_Status frames until InputCurrent reads `amps`."""
    return until(bus, lambda frames: current(frames[-1]) == amps)


def rises(frames):
    ic = [current(f) for f in frames]
    return [b - a for a, b in zip(ic, ic[1:]) if b > a]


class Bms:
    """BMS_Limits at the DBC period, with the allowance the test sets."""

    def __init__(self):
        self.aux = 0.0
        self.counter = 0

    def msg(self) -> can.Message:
        data = LIMITS.encode(
            {
                "DischargeCurrentLimit": 200.0,
                "ChargeCurrentLimit": 50.0,
                "AuxCurrentLimit": self.aux,
                "Counter": self.counter,
            }
        )
        self.counter = (self.counter + 1) & 0xFF
        return can.Message(arbitration_id=LIMITS.frame_id, data=data, is_extended_id=False)


@pytest.fixture
def bms(bus):
    """Plays the BMS, starting at zero allowance."""
    # Two BMS_Limits sources would take turns setting the allowance.
    assert not collect(bus, 1.0, [LIMITS.frame_id]), "another node sends BMS_Limits; run without a BMS"
    bms = Bms()
    with Periodic(bus, bms.msg, LIMITS.cycle_time / 1000) as sender:
        bms.sender = sender
        settle(bus, 0.0)
        yield bms


def test_status_is_periodic(bus, pytestconfig):
    frames = until(bus, lambda f: len(f) >= 60)
    gaps = [b[0] - a[0] for a, b in zip(frames, frames[1:])]
    mean = sum(gaps) / len(gaps)

    counters = [f[1]["Counter"] for f in frames]
    assert all((b - a) % 128 == 1 for a, b in zip(counters, counters[1:])), counters

    if pytestconfig.getoption("channel").startswith("vcan"):
        # Renode: virtual time is not tied to the host clock, so the host
        # timestamps measure how fast the simulation ran (5x slower than real
        # time on the bench host, and it varies with load), not the node's
        # period. What holds regardless is a steady cadence: measured within
        # 2% of the mean, so half or double the mean is a stall or a burst.
        assert mean / 2 < min(gaps) and max(gaps) < 2 * mean, f"gaps {min(gaps):.4f}..{max(gaps):.4f} s"
    else:
        # 2%: the node's clock is not trimmed, and the host stamps frames on receipt.
        assert abs(mean - PERIOD_S) < 0.02 * PERIOD_S, f"mean {mean * 1000:.2f} ms"
        assert max(gaps) < 1.2 * PERIOD_S, f"max {max(gaps) * 1000:.2f} ms"


def test_idle_signals_plausible(bus, bms):
    _, s = status(bus, 0.5)[-1]
    assert s["InputCurrent"] == 0.0
    assert abs(s["OutputVoltage"] - 13.8) < 0.05
    assert 20 <= s["Temperature"] <= 50
    assert s["DerateActive"] == "Inactive"


def test_ramps_up_at_slew_rate(bus, bms):
    bms.aux = 2.0
    frames = settle(bus, 2.0)
    assert max(current(f) for f in frames) <= 2.0
    assert all(r <= STEP_A + 0.005 for r in rises(frames)), [current(f) for f in frames]


def test_capped_at_demand(bus, bms):
    bms.aux = 10.0
    frames = settle(bus, DEMAND_A)
    frames += status(bus, 0.5)
    assert all(r <= STEP_A + 0.005 for r in rises(frames)), [current(f) for f in frames]
    assert max(current(f) for f in frames) == DEMAND_A


def test_reduction_honoured_immediately(bus, bms):
    bms.aux = 10.0
    settle(bus, DEMAND_A)

    bms.aux = 1.0
    frames = []
    end = time.monotonic() + TIMEOUT_S
    while len([f for f in frames if f.arbitration_id == STATUS.frame_id]) < 20 and time.monotonic() < end:
        frames += collect(bus, 0.25, [STATUS.frame_id, LIMITS.frame_id])

    # The first BMS_Limits carrying the reduction, back from the bus.
    t = next(m.timestamp for m in frames if m.arbitration_id == LIMITS.frame_id and LIMITS.decode(m.data)["AuxCurrentLimit"] == 1.0)
    after = [round(STATUS.decode(m.data)["InputCurrent"], 2) for m in frames if m.arbitration_id == STATUS.frame_id and m.timestamp > t]
    # One frame may already have been computed when the reduction arrived.
    assert all(ic <= 1.0 for ic in after[1:]), after
    assert after[-1] == 1.0, after


def test_limits_acknowledged(bus, bms):
    # On a physical bus, our frame only comes back once another node acknowledged it.
    sent = bms.sender.sent
    echoed = collect(bus, 1.0, [LIMITS.frame_id])
    sent = bms.sender.sent - sent
    assert sent > 0 and len(echoed) >= sent - 1, f"{len(echoed)} of {sent} echoed"
