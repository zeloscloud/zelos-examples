"""The DC-DC node over raw CAN: its status frame, and how it follows BMS_Limits.

This suite plays the BMS, so it needs the DC-DC without one. Each test sets the
allowance it starts from, so none depends on what the node did before.
"""

import pytest

from zelos_testing.dcdc import LIMITS, LIMITS_PERIOD_S, PERIOD_S, STATUS, BmsLimits, cycles, input_current, settle
from zelos_testing.frames import Periodic, assert_period, collect, count

# From nodes/dcdc: what the 12 V loads draw, and the slew cap per status frame.
DEMAND_A = 4.5
STEP_A = 10.0 * PERIOD_S

pytestmark = pytest.mark.usefixtures("bus_health")


def rises(frames):
    ic = [input_current(m) for m in frames]
    return [b - a for a, b in zip(ic, ic[1:]) if b > a]


@pytest.fixture
def bms(bus):
    """Plays the BMS, starting at zero allowance."""
    # Two BMS_Limits sources would take turns setting the allowance. A second of
    # the node's time sees ten from a BMS.
    assert not count(cycles(bus, 20, [LIMITS.frame_id]), LIMITS.frame_id), "another node sends BMS_Limits; run without a BMS"
    limits = BmsLimits(aux=0.0)
    with Periodic(bus, limits, LIMITS_PERIOD_S) as sender:
        limits.sender = sender
        settle(bus, 0.0)
        yield limits


def test_status_is_periodic(bus):
    frames = cycles(bus, 60)
    counters = [STATUS.decode(m.data)["Counter"] for m in frames]
    assert all((b - a) % 128 == 1 for a, b in zip(counters, counters[1:])), counters
    # 2%: the node's clock is not trimmed, and the host stamps frames on receipt.
    assert_period([b.timestamp - a.timestamp for a, b in zip(frames, frames[1:])], PERIOD_S, 0.02)


def test_idle_signals_plausible(bus, bms):
    s = STATUS.decode(cycles(bus, 1)[-1].data)
    assert s["InputCurrent"] == 0.0
    assert abs(s["OutputVoltage"] - 13.8) < 0.05
    assert 20 <= s["Temperature"] <= 50
    assert s["DerateActive"] == "Inactive"


def test_ramps_up_at_slew_rate(bus, bms):
    bms.aux = 2.0
    frames = settle(bus, 2.0)
    assert max(input_current(m) for m in frames) <= 2.0
    assert all(r <= STEP_A + 0.005 for r in rises(frames)), [input_current(m) for m in frames]


def test_capped_at_demand(bus, bms):
    bms.aux = 10.0
    frames = settle(bus, DEMAND_A)
    frames += cycles(bus, 10)
    assert all(r <= STEP_A + 0.005 for r in rises(frames)), [input_current(m) for m in frames]
    assert max(input_current(m) for m in frames) == DEMAND_A


def test_reduction_honoured_immediately(bus, bms):
    bms.aux = 10.0
    settle(bus, DEMAND_A)

    bms.aux = 1.0
    frames = cycles(bus, 20, [LIMITS.frame_id])

    # The first BMS_Limits carrying the reduction, back from the bus.
    t = next(m.timestamp for m in frames if m.arbitration_id == LIMITS.frame_id and LIMITS.decode(m.data)["AuxCurrentLimit"] == 1.0)
    after = [input_current(m) for m in frames if m.arbitration_id == STATUS.frame_id and m.timestamp > t]
    # One frame may already have been computed when the reduction arrived.
    assert all(ic <= 1.0 for ic in after[1:]), after
    assert after[-1] == 1.0, after


def test_limits_acknowledged(bus, bms):
    # On a physical bus, our frame only comes back once another node acknowledged it.
    sent = bms.sender.sent
    echoed = collect(bus, 1.0, [LIMITS.frame_id])
    sent = bms.sender.sent - sent
    assert sent > 0 and len(echoed) >= sent - 1, f"{len(echoed)} of {sent} echoed"
