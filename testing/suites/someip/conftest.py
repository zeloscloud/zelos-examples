"""The DC-DC's SOME/IP service at --eth-host; its UDS server on CAN for what it is checked against."""

import contextlib

import pytest

from zelos_testing.bench_plugin import eth_link
from zelos_testing.frames import bound
from zelos_testing.uds import DEFAULT, SECURITY_DELAY_S, UdsEcu


@pytest.fixture(scope="session")
def host(pytestconfig) -> str:
    link = eth_link(pytestconfig)
    if link is None:
        pytest.skip(f"no host interface on {pytestconfig.getoption('eth_host')}'s subnet (--eth-iface)")
    return link[1]


@pytest.fixture
def can_ecu(pytestconfig, uds_server):
    interface = pytestconfig.getoption("interface")
    interface = "zelos-socketcan" if interface == "socketcan" else interface
    with UdsEcu(interface, pytestconfig.getoption("channel"), timeout_s=bound(2.0), delay_s=bound(SECURITY_DELAY_S + 1)) as e:
        yield e
        with contextlib.suppress(Exception):
            e.client.change_session(DEFAULT)
