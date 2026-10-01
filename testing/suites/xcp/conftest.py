"""Where the A2L of the node under test is: it has this build's addresses."""

import os


def pytest_addoption(parser):
    parser.addoption(
        "--a2l",
        default=os.environ.get("ZELOS_A2L"),
        help="A2L generated beside the node's ELF, e.g. build/dcdc-xcp.a2l (default: $ZELOS_A2L)",
    )
