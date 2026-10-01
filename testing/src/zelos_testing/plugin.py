"""pytest plugin: which channel the nodes are on, and fixtures for it."""

import json
import os
import subprocess

import can
import pytest

from zelos_testing import frames

# Counters that stay flat on a healthy bus. Arbitration loss is normal traffic.
ERROR_COUNTERS = ("restarts", "bus_error", "error_warning", "error_passive", "bus_off")


def pytest_addoption(parser):
    parser.addoption(
        "--channel",
        default="vcan0",
        help="SocketCAN interface the nodes are on: vcan0 on a Renode bench, "
        "or a physical adapter such as can0 (default: vcan0)",
    )
    parser.addoption(
        "--time-scale",
        type=float,
        help="Host seconds a wait may take per second of the node's time. Waits end "
        "on the node's frames; this only bounds them (default: 1 on a physical "
        "channel, 20 on vcan, where Renode runs several times slower than real time)",
    )
    parser.addoption(
        "--a2l",
        default=os.environ.get("ZELOS_A2L"),
        help="A2L generated beside an XCP node's ELF, e.g. build/dcdc-xcp.a2l (default: $ZELOS_A2L)",
    )


@pytest.fixture
def bus(pytestconfig):
    """The channel under test. Our own frames come back once transmitted."""
    channel = pytestconfig.getoption("channel")
    with can.Bus(interface="socketcan", channel=channel, receive_own_messages=True) as b:
        yield b


@pytest.fixture(scope="session", autouse=True)
def time_scale(pytestconfig):
    """The --time-scale every wait is bounded by, also through frames.bound."""
    scale = pytestconfig.getoption("time_scale")
    if scale is None:
        scale = 20.0 if _link(pytestconfig.getoption("channel"))["info_kind"] == "vcan" else 1.0
    frames.TIME_SCALE = scale
    return scale


def _link(channel: str) -> dict:
    out = subprocess.run(
        ["ip", "-json", "-details", "-statistics", "link", "show", channel],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return json.loads(out)[0]["linkinfo"]


@pytest.fixture
def bus_health(pytestconfig):
    """The controller stays error-active and counts no new errors across the test.

    A virtual channel has no controller, so there this checks nothing.
    """
    channel = pytestconfig.getoption("channel")
    before = _link(channel)
    if before["info_kind"] == "vcan":
        yield
        return

    assert before["info_data"]["state"] == "ERROR-ACTIVE", f"{channel} before: {before['info_data']}"
    yield
    after = _link(channel)
    assert after["info_data"]["state"] == "ERROR-ACTIVE", f"{channel} after: {after['info_data']}"
    grew = {
        k: after["info_xstats"][k] - before["info_xstats"][k]
        for k in ERROR_COUNTERS
        if after["info_xstats"][k] != before["info_xstats"][k]
    }
    assert not grew, f"{channel} error counters grew: {grew}"
