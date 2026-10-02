# CANopen suite

An example of testing a CANopen node from the host, with python `canopen` as
the master. Each test in `test_pdu.py` shows one behaviour in plain steps.

## The node

`nodes/pdu` is a CiA 401-style power distribution unit at node-id `0x20`. It
switches eight output channels on command, reports which are powered and what
each draws, and trips channel 8, which is shorted, with an EMCY.

| Object | COB-ID | Carries |
|---|---|---|
| NMT | `0x000` | start, stop, reset, from the master |
| SYNC | `0x080` | from the master |
| TIME | `0x100` | from the master |
| EMCY | `0x0A0` | overcurrent trip and its reset |
| TPDO1 | `0x1A0` | inputs (`0x6000`), on change and every 0.5 s |
| RPDO1 | `0x220` | outputs (`0x6200`) |
| TPDO2 | `0x2A0` | per-channel current (`0x2000`), 0.1 A |
| RPDO2 | `0x320` | outputs, after a dummy byte |
| TPDO3 | `0x3A0` | millisecond counter (`0x2003`) and inputs, on every SYNC |
| SDO | `0x5A0` / `0x620` | server response / request |
| Heartbeat | `0x720` | NMT state, every 1 s |

On the can-full bench the VCU also sends on `0x100`, so the node takes its
frames as TIME: `0x2004` holds VCU data there, and one arriving while the node
is stopped raises EMCY `0x8260` once it leaves stopped.

## Running it

```bash
cd can-full && just test                        # every suite, on the Renode bench
just test-bench canopen can-full/build/pdu.elf  # this suite alone, from the root
cd can-full && just hil pdu can0                # flash a NUCLEO-H753ZI, run on can0
```

Against a node already on a SocketCAN channel:

```bash
cd testing && uv run pytest suites/canopen --channel can0
```

## Adapting it to your node

- `EDS`: your node's EDS. python `canopen` reads object names, types and the
  default PDO mapping from it.
- `NODE_ID`: your node's id; the COB-IDs above follow from it.
- `LOAD_DA`, `SHORTED`, `INPUTS`, `OUTPUTS`: what this node's IO does. Replace
  them, and the tests that use them, with your node's objects.

The `node` fixture resets the node over NMT before each test, so every test
starts from the node's defaults.
