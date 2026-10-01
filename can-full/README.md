# The CAN bench with protocols on it

can-bench's three nodes plus two more, on one virtual bus, speaking the
protocols real vehicle networks carry on top of CAN: XCP for measurement and
calibration, J1939 for the engine, CANopen for a power distribution unit.

```
 ┌───────┐ ┌───────┐ ┌────────┐ ┌────────┐ ┌───────┐
 │  VCU  │ │  BMS  │ │ DC-DC  │ │ genset │ │  PDU  │   Zephyr on STM32H753, in Renode
 │       │ │       │ │ + XCP  │ │ J1939  │ │CANopen│
 └───┬───┘ └───┬───┘ └───┬────┘ └───┬────┘ └───┬───┘
     └─────────┴──────── vcan0 ──────┴──────────┘
                           │
                  agent :2300  ·  tester (pytest)
```

Each protocol is checked by an independent implementation of it: pyxcp as the
XCP master, python-can-j1939 reassembling J1939 transport, python `canopen` as
the CANopen master. A bug shared by the node and the tool cannot hide that way.

## What you need

The same as [can-bench](../can-bench/#what-you-need): Linux with Docker, and
[just](https://just.systems). The first `just build` also pulls the
toolchain and the Zephyr workspace.

## Running it

```bash
just build      # build the five nodes, and the DC-DC's A2L, into build/
just up         # bus, Renode, agent
just test       # the protocol suites against the running bench
just down
```

`just bus` tails the raw bus. The agent decodes `../dbc/bench.dbc` and
`../dbc/genset.dbc` and listens on 2300, or on `BENCH_PORT`; can-bench uses the
same default, so run one bench at a time or move one.

## The nodes

| Node | Built from | Protocol | Identifiers |
|---|---|---|---|
| VCU | `../nodes/vcu` | | `0x100` |
| BMS | `../nodes/bms` | | `0x200`-`0x202` |
| DC-DC | `../nodes/dcdc`, `-S xcp` | XCP on CAN | `0x300`; CRO `0x6F0`, DTO `0x6F1` |
| genset | `../nodes/genset` | J1939 | 29-bit, source address `0x80` |
| PDU | `../nodes/pdu` | CANopen, node-id `0x20` | NMT `0x000`, SYNC `0x080`, EMCY `0x0A0`, TPDO1 `0x1A0`, RPDO1 `0x220`, TPDO2 `0x2A0`, SDO `0x5A0`/`0x620`, heartbeat `0x720` |

The first three are can-bench's, unchanged except that the DC-DC is built with
XCP: its demand and slew rate become calibration parameters, its setpoint,
current, temperature and allowance become measurements, all described by
`build/dcdc-xcp.a2l`, which the build fills with that ELF's addresses.

The genset follows the VCU's mode: it runs hard while driving, idles in standby
and stops while charging. Under load its coolant runs hot, and it raises two
DTCs in DM1, sent by BAM. The PDU switches eight outputs on command, reports
inputs and per-channel current, and trips channel 8, which is shorted.

## What the suites prove

| Suite | Against | Proves |
|---|---|---|
| `../testing/suites/xcp` | DC-DC | Connect and identify; the EPK matches the A2L's; reads restricted to the registered variables; a calibration takes effect on the bus, both ways, and is bounded to the A2L's limits, NaN included; DAQ samples once per cycle, matching the frame the same cycle sent; DAQ properties as the A2L declares them; timestamps from the GET_DAQ_CLOCK clock, one cycle apart (and within 2 % of the host's clock, on hardware); a running list's layout refused changes; invalid modes refused; another master's CONNECT stops DAQ and the first master recovers |
| `../testing/suites/j1939` | genset | Address claim on request; defending against a higher NAME and yielding to a lower one; requests answered; periods; DM1 by BAM, reassembled by python-can-j1939 |
| `../testing/suites/canopen` | PDU | Boot-up and heartbeat; NMT start, stop and pre-operational; SDO identity and downloads; RPDO commands reflected in TPDOs; an overcurrent trip raises EMCY and recovers, and trips again after a communication reset |

The suites drive the other nodes' roles themselves when those nodes are absent,
so the same suites run here, with every node present, and against one board.
Every test sets its own preconditions over the bus rather than assuming a fresh
boot.

The raw-CAN DC-DC suite, `../testing/suites/can_raw`, plays the BMS, so it runs
only where there is none: on hardware, or on a bench of its own, which CI runs
too. From the repository root:

```bash
just test-bench can_raw can-full/build/dcdc-xcp.elf
```

## Timing under Renode

The bench runs at a fifth of real time on a 16-core machine, the same as
can-bench: five nodes cost no more than three there. Renode's clock is not tied
to the host's, so on this bench the suites assert timing relative to the node's
own frames: ten EEC1 per ET1, one DAQ sample per DC-DC cycle, heartbeats never
faster than declared. Waits end on frames counted in the node's time; host
seconds only bound them, scaled by pytest's `--time-scale`: 1 on a physical
channel, 20 on vcan. Absolute periods and latencies are asserted on hardware
only.

## On hardware

The same firmware runs on a NUCLEO-H753ZI. With the board on a SocketCAN
adapter at 500 kbit/s, and probe-rs and uv installed:

```bash
just hil dcdc-xcp can0   # flash, then the xcp suite and the raw-CAN DC-DC suite
just hil genset can0
just hil pdu can0
```

On a physical channel every test also checks that the controller stays
error-active and counts no new errors.

## What this bench cannot show you

- Everything in [can-bench's list](../can-bench/#what-this-bench-cannot-show-you):
  no acknowledgement, arbitration, error frames or bus load in Renode.
- **Protocol timing in real time.** J1939's 250 ms claim delay and BAM packet
  spacing, CANopen heartbeat periods and XCP response times are checked on
  hardware; here only their order and ratios are.
- **J1939 decode after a move.** The agent decodes the genset by its full
  29-bit identifier. The J1939 suite makes the genset yield its address, so
  after `just test` its frames carry a new source address the DBC does not
  name, until `just down && just up`.
