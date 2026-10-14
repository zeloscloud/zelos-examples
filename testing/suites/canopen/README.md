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

## Running it

```bash
cd can-full && just test                        # every suite, on the Renode bench
just test-bench canopen can-full/build/full.elf # this suite alone, from the root
cd can-full && just hil full can0               # flash a NUCLEO-H753ZI, run on can0
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
starts from the node's defaults. Reset-node restores the PDU's whole object
dictionary in place; the MCU does not reboot. A synchronous TPDO already in
the node's transmit queue when the SYNC window closes goes out late rather
than being dropped.

## A bench with its own CANopen manager

A hardware bench may carry a manager (a PLC) that owns the node: NMT, SYNC,
heartbeat `0x77F`. The plugin listens for one before any test sends; the PDU
already operational counts as one, as only a manager starts it. The suites
leave it pre-operational. With one:

- `test_pdu.py` pauses it for the module through its `bench_quiet` Modbus
  register, via the Modbus extension's named-register actions on a Zelos
  agent (one write and read-back each way), and puts the register back as
  found; unconfigured, the module skips.
- `test_pdu_managed.py` checks the node as the manager runs it: operational
  with its heartbeat, one TPDO3 per foreign SYNC, SDO. Without a manager it
  skips.

| Option | Env | Default |
|---|---|---|
| `--quiet-agent` | `ZELOS_BENCH_QUIET_AGENT` | unset: test_pdu.py skips |
| `--quiet-device` | `ZELOS_BENCH_QUIET_DEVICE` | unset: test_pdu.py skips |
| `--quiet-register` | `ZELOS_BENCH_QUIET_REGISTER` | `setpoints/bench_quiet` |
| `--quiet-mask` | `ZELOS_BENCH_QUIET_MASK` | `0b10`, the CANopen manager's bit |
