"""pytest plugin: the hardware bench's options and fixtures.

A PLC that manages the CANopen node and how to pause it, the XCP node's A2L, and
the coexistence soak's length. See zelos_testing.bench.
"""

import ipaddress
import json
import os
import subprocess
from pathlib import Path

import can
import pytest

from zelos_testing import bench, frames
from zelos_testing.plugin import socketcan


def pytest_addoption(parser):
    # A PLC managing the bench's CANopen node: how to pause it (see zelos_testing.bench).
    group = parser.getgroup("bench_quiet", "pausing a foreign CANopen manager for tests that drive the node")
    group.addoption("--quiet-agent", default=os.environ.get("ZELOS_BENCH_QUIET_AGENT"), help="Zelos agent URL running the Modbus extension that reaches the PLC ($ZELOS_BENCH_QUIET_AGENT)")
    group.addoption("--quiet-device", default=os.environ.get("ZELOS_BENCH_QUIET_DEVICE"), help="the Modbus extension's device for the PLC, e.g. codesys/plc ($ZELOS_BENCH_QUIET_DEVICE)")
    group.addoption("--quiet-register", default=os.environ.get("ZELOS_BENCH_QUIET_REGISTER", "setpoints/bench_quiet"), help="bench_quiet's name in the device map ($ZELOS_BENCH_QUIET_REGISTER)")
    group.addoption("--quiet-mask", type=lambda v: int(v, 0), default=os.environ.get("ZELOS_BENCH_QUIET_MASK", "0b10"), help="its bits that pause the CANopen manager ($ZELOS_BENCH_QUIET_MASK)")
    parser.addoption(
        "--soak-s",
        type=float,
        help="Host seconds the coexistence suite runs every protocol at once "
        "(default: 120 on vcan, 10 on a physical channel)",
    )
    parser.addoption(
        "--eth-host",
        default=os.environ.get("ZELOS_TEST_ETH_HOST", "10.1.1.75"),
        help="The node's IPv4 address, lib/eth's default ($ZELOS_TEST_ETH_HOST, else 10.1.1.75)",
    )
    parser.addoption(
        "--eth-iface",
        default=os.environ.get("ZELOS_TEST_ETH_IFACE"),
        help="Host interface on the node's subnet, e.g. enp86s0 ($ZELOS_TEST_ETH_IFACE; "
        "default: the one interface with an address on --eth-host's subnet)",
    )
    parser.addoption(
        "--a2l",
        type=Path,
        help="A2L generated beside an XCP node's ELF, e.g. build/full.a2l "
        "(default: the one *.a2l in /elf, where a bench's tester mounts its ELFs)",
    )


def find_a2l(config) -> Path | None:
    """--a2l, or the one *.a2l in /elf, where a bench's tester mounts its ELFs."""
    path = config.getoption("a2l")
    if path is None:
        found = sorted(Path("/elf").glob("*.a2l"))
        path = found[0] if len(found) == 1 else None
    return path


def eth_link(config) -> tuple[str, str] | None:
    """(host interface, node address): --eth-iface, else the interface on the node's subnet."""
    host = config.getoption("eth_host")
    iface = config.getoption("eth_iface")
    if iface is None:
        out = subprocess.run(["ip", "-json", "-4", "addr"], check=True, capture_output=True, text=True).stdout
        node = ipaddress.ip_address(host)
        found = {
            link["ifname"]
            for link in json.loads(out)
            for a in link.get("addr_info", [])
            if node in ipaddress.ip_interface(f"{a['local']}/{a['prefixlen']}").network
        }
        iface = found.pop() if len(found) == 1 else None
    return (iface, host) if iface else None


def quiet_config(config) -> bench.QuietConfig | None:
    agent, device = config.getoption("quiet_agent"), config.getoption("quiet_device")
    if not agent or not device:
        return None
    return bench.QuietConfig(agent=agent, device=device, register=config.getoption("quiet_register"), mask=config.getoption("quiet_mask"))


@pytest.fixture(scope="session", autouse=True)
def foreign_canopen(pytestconfig) -> bench.ForeignCanopen:
    """A CANopen manager or SYNC producer other than this process, found once before any test sends."""
    if not socketcan(pytestconfig):
        return bench.ForeignCanopen(manager=False, sync=False, node_operational=False)
    with can.Bus(interface="socketcan", channel=pytestconfig.getoption("channel")) as b:
        return bench.foreign_canopen(b)


@pytest.fixture(scope="module")
def canopen_takeover(pytestconfig, foreign_canopen):
    """The CANopen node is this module's: a foreign manager is paused for it, or the module skips."""
    if not foreign_canopen.present:
        yield
        return
    cfg = quiet_config(pytestconfig)
    if cfg is None:
        pytest.skip("a foreign CANopen manager is on the bus and bench_quiet is not configured (--quiet-agent, --quiet-device)")
    import zelos_sdk

    with bench.BenchQuiet(cfg, zelos_sdk.Agent(cfg.agent).actions):
        yield


@pytest.fixture(scope="module")
def foreign_managed(pytestconfig, foreign_canopen) -> bool:
    """Whether a foreign manager runs the PDU; if so, waits until it does again after an earlier module's takeover."""
    if foreign_canopen.present:
        # A bus of its own: frames from before a takeover must not count.
        with can.Bus(interface="socketcan", channel=pytestconfig.getoption("channel")) as b:
            assert bench.wait_managed(b, bench.PDU_HEARTBEAT, frames.bound(15.0)), "the foreign manager did not bring the PDU operational"
    return foreign_canopen.present
