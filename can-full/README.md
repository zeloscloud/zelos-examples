# The CAN bench with protocols on it

can-bench's VCU and BMS plus one image carrying every protocol real vehicle
networks put on top of CAN: XCP for measurement and calibration, UDS for
diagnostics and AUTOSAR PDUs on the DC-DC, J1939 for the engine, CANopen for a
power distribution unit. One MCU hosts the three as logical ECUs, as one board does
on the hardware bench; the same image is the bench's default and fallback.

```
 ┌───────┐ ┌───────┐ ┌──────────────────────────────────┐
 │  VCU  │ │  BMS  │ │ full: DC-DC   genset    PDU      │   Zephyr on STM32H753, in Renode
 │       │ │       │ │       XCP+UDS J1939     CANopen  │
 │       │ │       │ │       AUTOSAR                    │
 └───┬───┘ └───┬───┘ └────────────────┬─────────────────┘
     └─────────┴──────── vcan0 ───────┘
                           │
                  agent :2300  ·  tester (pytest)
```

Each protocol is checked by an independent implementation of it: pyxcp as the
XCP master, can-isotp and udsoncan as the UDS tester, cantools reading the
AUTOSAR system description, python-can-j1939 reassembling J1939 transport,
python `canopen` as the CANopen master. A bug shared by the node and the tool cannot hide that way.

## What you need

The same as [can-bench](../can-bench/#what-you-need): Linux with Docker, and
[just](https://just.systems). The first `just build` also pulls the
toolchain and the Zephyr workspace.

## Running it

```bash
just build      # build vcu, bms and full, and full's A2L, into build/
just up         # bus, Renode, agent
just test       # the protocol suites against the running bench
just down
```

`just bus` tails the raw bus. The agent decodes `../dbc/full.dbc` (every
classic and J1939 message; the PDU's CANopen frames come from its EDS) and listens on 2300, or on `BENCH_PORT`; can-bench uses the
same default, so run one bench at a time or move one.

## The nodes

| Node | Built from | Protocol | Identifiers |
|---|---|---|---|
| VCU | `../nodes/vcu` | | `0x101` |
| BMS | `../nodes/bms` | | `0x200`-`0x202` |
| DC-DC | `../nodes/full` (`../nodes/dcdc`, XCP, UDS, AUTOSAR and Ethernet) | XCP on CAN, UDS on ISO-TP, AUTOSAR PDUs; IPv4 on Ethernet | `0x300`; AUTOSAR `0x310`-`0x315`; CRO `0x6F0`, DTO `0x6F1`; UDS `0x7E0`, functional `0x7DF`, responses `0x7E8`; `10.1.1.75/24` |
| Signal generator | `../nodes/full/src/waveform.c` | Classic CAN | `0x7F0` `Waveform` every 2 ms: sine, square and sawtooth of one phase, chirped 1 to 100 Hz over 20 s; the lowest-priority id, for plots |
| genset | `../nodes/full` (`../nodes/genset`) | J1939 | 29-bit, source address `0x80` |
| PDU | `../nodes/full` (`../nodes/pdu`) | CANopen, node-id `0x20` | NMT `0x000`, SYNC `0x080`, TIME `0x100`, EMCY `0x0A0`, TPDO1 `0x1A0`, RPDO1 `0x220`, TPDO2 `0x2A0`, RPDO2 `0x320`, TPDO3 `0x3A0`, SDO `0x5A0`/`0x620`, heartbeat `0x720` |

The first three are can-bench's, unchanged except that the DC-DC runs XCP, UDS
and AUTOSAR PDUs, and has Ethernet. On this bench every node sits on one
Renode Ethernet switch, bridged to `tap0` (10.1.1.1/24) where the tester runs; on
hardware the board's RJ45 port joins the bench's LAN, and nothing waits for the
cable: CAN starts at the same time without one. The address is usable about 7 s
after a reset (link up at about 1.7 s, then RFC 5227 conflict detection), so a
tester reconnecting over Ethernet after an ECUReset waits that long. The
DC-DC is a DoIP entity there (logical address `0x1001`, functional `0xE400`,
testers `0x0E00`-`0x0FFF`), with the same UDS server CAN reaches: while one
tester holds a non-default session, the other transport's gets 0x21, except for
OBD requests. It is also a SOME/IP service, DcdcService `0x5A00` instance 1 on
UDP 30509, offered by SD on 224.224.224.245:30490: the event DcdcStatus every
100 ms to subscribers, carrying the DCDC_Status Counter of the same sample, and
the field Setpoint (DID `0x0200`), read-only. `../arxml/can-full.arxml`
describes it. Over XCP its demand and slew rate become calibration parameters, its
setpoint, current, temperature and allowance measurements, all described by
`build/full.a2l`, which the build fills with that ELF's addresses.
Over UDS (ISO 14229-1) it is a diagnostic server as an OEM baseline ECU carries
one: sessions, two security levels, DTCs raised by its model, an output voltage
setpoint the model follows, and download into RAM. On the same identifiers it
is an OBD-II responder (SAE J1979 and J1979-2): live PIDs, DTCs as a scan tool
lists them, a freeze frame, the VIN. Its DTCs follow US OBD-II: pending in the
first failing cycle, confirmed with the MIL in the second, the MIL off after
three passing cycles, permanent DTCs kept in flash. An ECUReset ends a cycle. The contract, which the UDS
extension's demo ECU mirrors, is `../nodes/dcdc/src/uds.c`. The UDS server is
the DC-DC's: CommunicationControl and ControlDTCSetting act on it alone, and
ECUReset reboots the MCU, so all three restart. CANopen's NMT reset-node resets
the PDU alone, without a reboot. Every protocol sends through one queue in
`../lib/can`, lowest identifier first, as an ECU's CAN driver does.

XCP writes the calibration without seed and key while UDS asks level 0x01 for
the same memory: this is a development ECU.

As an AUTOSAR ECU it sends what `../arxml/can-full.arxml` (an AUTOSAR R19-11
System Template of this bench, `bench.dbc` included) describes: E2E profiles 1,
2 and 5, a container with short headers, a multiplexed PDU and a SecOC profile 1
secured PDU, each carrying the model's values. Its tables are generated from the
ARXML (`just arxml` at the root); the SecOC key is RFC 4493's example key. The
ARXML is the node's description for host tools too: Zelos decodes the bench from
it as from the DBC, each AUTOSAR PDU by name with its E2E status.

The DC-DC and PDU single-node images still build, for focused work: `just build
dcdc-xcp-uds pdu`. The genset runs only in `full`.

The VCU's command sits at `0x101`, not CANopen's TIME id `0x100`: CiA 301 keeps
CANopen devices out of `0x101`-`0x180`, so no CANopen node can collide with it.

The genset follows the VCU's mode: rated speed under load while driving, idle
in standby, stopped while charging. It is an engine and a generator set on
J1939: ten engine PGs of J1939-71 (EEC1, EEC2, ET1, EFL/P1, VEP1, HOURS, LFC,
LFE, AMB, IC1) and seven generator PGs of J1939-75 (average and per-phase AC
quantities, total real, apparent and reactive power, power factor, energy), all
from one model, so frequency follows speed, P = 3 V I pf, and energy, fuel and
hours integrate; about 111 frames a second, every layout from `../dbc/genset.dbc`.
Under load its coolant runs hot: DM1 carries SPN 110 in one frame with the
amber lamp, then SPN 100 (oil pressure) joins it with the red lamp and DM1
goes by BAM; cooled, DM1 is empty and DM2, on request, lists what was active.
Asked for DM2 or its software identification (SOFT), it answers the requester
by RTS/CTS, or everyone by BAM; a request for a PGN it lacks gets a NACK. The
PDU switches eight outputs on command, reports inputs and per-channel current,
and trips channel 8, which is shorted.

## What the suites prove

| Suite | Against | Proves |
|---|---|---|
| `../testing/suites/xcp` | DC-DC | Used as a bench uses its ECU: zelos-can opens the bus, pyxcp is the master, the A2L names everything, and `check` asserts on the Zelos trace. The EPK matches the A2L's; a measurement read over XCP matches the broadcast frame; DAQ on the A2L's event at its period, by the ECU's own timestamps; the node follows the BMS allowance; a calibration takes effect and is restored; measurements are read-only. Protocol conformance is covered by the ztests in `../tests/lib/xcp` |
| `../testing/suites/uds` | DC-DC | A conformance suite for any UDS server on any python-can bus: this node, or the UDS extension's demo ECU in-process. Used as a tester uses an ECU: zelos-can opens the bus, can-isotp carries, udsoncan asks. Every Tier A service with its NRCs and their ISO 14229-1 order, physical and functional; sessions, S3, P2; both security levels, lockout and power-up delay; memory by address; erase, download, check, upload of 64 KiB. `test_dcdc_node.py`, against the node itself: 0x28 stops DCDC_Status, the setpoint drives the output, DTCs follow the model through operation cycles, a reset is a fresh boot, XCP and UDS share the ECU. Core conformance: ztests in `../tests/lib/uds` |
| `../testing/suites/obd` | DC-DC | A scan tool's view of the same server, SAE J1979 on 0x7DF: supported PIDs walked from the bitmaps; PIDs against DCDC_Status; MIL and DTC count; a lost BMS pending in its first cycle and confirmed in its second, the MIL off after three passing cycles, the freeze frame stored with the pending code; a permanent DTC surviving 0x04 and gone after one passing cycle; J1979-2 protocol identification, multi-DID subsets, no 0x13 to OBD requests; the VIN (multi-frame) and ECU identity; J1979-2 (OBDonUDS) DIDs and 0x19 0x42/0x55/0x56 equal to the legacy services; silence to unsupported requests. Runs alone in CI (`just test-bench obd`) too, so its fault tests can play the BMS |
| `../testing/suites/autosar` | DC-DC | Decoded as a bench decodes an AUTOSAR ECU, from the ARXML alone: ids, lengths and cycles as described; E2E profile 1 (by hand), 2 and 5 (cantools) CRCs and counters; the container's PDUs in turn; the multiplexed PDU's alternatives; the SecOC MAC verified with the key; every value equal to DCDC_Status's of the same sample; UDS on the CanTp addresses the ARXML gives, its flow control as described; 0x28 silences the PDUs. The ARXML carries `bench.dbc` unchanged. Core: ztests in `../tests/lib/autosar` with the PRS E2E examples |
| `../testing/suites/j1939` | genset | Address claim on request; defending against a higher NAME and yielding to a lower one; every slow PG answered on request, a NACK for one the node lacks; every PG at its period; values agreeing across PGs (frequency and voltage from speed, P = 3 V I pf, S and Q, balanced phases, fuel only while turning) and the counters integrating speed, fuel rate and power; DM1 with one DTC in a frame, then two by BAM, lamps and occurrence counts as they should be, reassembled by python-can-j1939; DM2 by BAM to a global request and by RTS/CTS to ours, python-can-j1939 granting the windows; SOFT by RTS/CTS and by BAM. Originator conformance (windows, retransmit, abort, T3/T4): ztests in `../tests/lib/j1939` |
| `../testing/suites/canopen` | PDU | Boot-up and heartbeat; NMT start, stop and pre-operational; SDO identity and downloads; RPDO commands reflected in TPDOs; an overcurrent trip raises EMCY and recovers; SYNC answered by the synchronous TPDO |
| `../testing/suites/eth` | DC-DC | The board's Ethernet (`../lib/eth`): link 100 Mb/s full duplex, the static address up after conflict detection, ping, the host's neighbour entry equal to the node's MAC, and the node's counters (UDS DID `0x0111`, read over CAN). The host end is whichever interface is on the node's subnet: the bench's `tap0`, or the hardware bench's own |
| `../testing/suites/doip` | DC-DC | Its UDS server as a DoIP entity (ISO 13400-2, `../lib/doip`), checked with a client written from the standard and with zelos-can's (doipclient), as the UDS and OBD extensions connect: vehicle identification by plain request, EID and VIN, answered within 500 ms, and one responder to a broadcast; entity status, power mode, header NACKs; routing activation's codes (0x10, 0x00, 0x02, 0x03 and 0x01 decided by alive checks, 0x06), initial inactivity; diagnostic messages acknowledged, NACKed (0x02, 0x03, 0x04 above MDS) or answered; three announcements 500 ms apart after a reset. The UDS conformance suite, unchanged, over DoIP. One server, two transports: a CAN tester's session gives the DoIP tester 0x21 and only OBD, and S3 still ends it; a closed DoIP owner's output returns at once; a pending routine answers its own tester. Core: ztests in `../tests/lib/doip` |
| `../testing/suites/someip` | DC-DC | Its SOME/IP service (DcdcService `0x5A00`, `../lib/someip`) and its SD, checked with a client written from the PRS: offers every second with TTL 3 and the service's UDP endpoint (where the SD group reaches the host: on this bench, not on the hardware bench, whose switch forwards it to IGMPv2 members only); FindService answered unicast within 100 ms; a subscription acknowledged, the Setpoint field's value at once, then DcdcStatus every 100 ms with none missing, each equal, within the frame's resolution, to the DCDC_Status of the same Counter; the getter equal to DID `0x0200`, and the notifier within 150 ms of a write over UDS; 0x28 stops the events, not SD or the getter; error codes; StopOffer at a reset, and the Reboot flag after it. Core: ztests in `../tests/lib/someip` |
| `../testing/suites/full` | all three | Every protocol at once for `--soak-s` (120 s here, 10 s on hardware): XCP DAQ, a UDS 64 KiB download and upload loop, CANopen operational with SYNC every 10 ms and RPDO1, J1939 and DCDC_Status. No EMCY, no DAQ sample lost, periods kept, and the node's counters (UDS DID `0x0110`) show no TX queue overflow, TX error or RX FIFO overrun |

The suites drive the other nodes' roles themselves when those nodes are absent,
so the same suites run here, with every node present, and against one board.
Every test sets its own preconditions over the bus rather than assuming a fresh
boot.

The raw-CAN DC-DC suite, `../testing/suites/can_raw`, plays the BMS, so it runs
only where there is none: on hardware, or on a bench of its own, which CI runs
too. From the repository root:

```bash
just test-bench can_raw can-full/build/full.elf
```

## Timing under Renode

The nodes keep time with Renode's clock, and Renode keeps its clock to the
host's as long as the host can run the bench's three machines at full speed;
a slower host slows every node together. So the suites assert timing relative to the nodes' own frames: ten EEC1 per ET1,
one DAQ sample per DC-DC cycle, heartbeats never faster than declared. Waits
end on frames counted in the node's time; host seconds only bound them, scaled
by pytest's `--time-scale`: 1 on a physical channel, 10 on vcan, twice what a
host that runs the bench at a fifth of real time needs. Absolute periods and
latencies are asserted on hardware only: a host timestamp can lead or trail
the node's clock by tens of milliseconds while Renode catches up after a pause.

## On hardware

The same firmware runs on a NUCLEO-H753ZI. With the board on a SocketCAN
adapter at 500 kbit/s, and probe-rs and uv installed:

```bash
just hil full can0   # flash, then every suite, raw-CAN DC-DC included
```

On a physical channel every test also checks that the controller stays
error-active and counts no new errors.

## What this bench cannot show you

- Everything in [can-bench's list](../can-bench/#what-this-bench-cannot-show-you):
  no acknowledgement, arbitration, error frames or bus load in Renode.
- **Protocol timing in real time.** J1939's 250 ms claim delay and BAM packet
  spacing, CANopen heartbeat periods, XCP response times and UDS P2, S3 and
  security delays are checked on
  hardware; here only their order and ratios are.
- **J1939 decode after a move.** The agent decodes the genset by its full
  29-bit identifier. The J1939 suite makes the genset yield its address, so
  after `just test` its frames carry a new source address the DBC does not
  name, until `just down && just up`.
