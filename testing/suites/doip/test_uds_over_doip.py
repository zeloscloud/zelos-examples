"""The UDS conformance suite, unchanged, with its tester on DoIP (conftest.py's `ecu`)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "uds"))

from test_uds_conformance import *
