# V2G suite

DC charging sessions between an EVerest EVSE and an EVerest EV, ISO 15118-2 and
DIN 70121, over a veth pair, each captured on the link. Software in the loop:
no modems, no CP line.

```bash
just test-v2g            # from the repository root; Linux Docker
just test-v2g -k din     # pytest arguments pass through
just v2g-record          # record captures/ again
```

## The SIL

| Piece | What it is |
|---|---|
| Image | `everest-demo/manager` 2025.10.0-single-phase, pinned by digest (`Dockerfile`), plus Mosquitto, tshark and pytest |
| EVSE | `EvseV2G` on `v2g-evse`, no TLS, with EVerest's simulated DC supply, isolation monitor and board |
| EV | `PyEvJosev` (Josev, EXIficient codec) driven by `EvManager`, standalone in its own network namespace on `v2g-ev` |
| SLAC | `SlacSimulator`, in process: no HomePlug frames on the link |
| Broker | Mosquitto on a Unix socket, reachable from both namespaces |
| Capture | dumpcap on `v2g-evse`: SDP, the V2G TCP connection, neighbour discovery |

The container needs `CAP_NET_ADMIN` (the veth) and `CAP_SYS_ADMIN` (the EV's
namespace), which `just test-v2g` grants.

## The checks

Truth is the link. Each capture is walked at the V2GTP layer: SDP request and
response, the response's address and port the ones the EV connected to, then
each direction of the one TCP connection split into V2GTP frames with nothing
left over, requests and responses alternating. EXI is not decoded. Frames are
named by the EVSE's record of them (`v2g_messages`), matched on bytes.

| Test | EV does | Asserts |
|---|---|---|
| `test_session[iso2]`, `[din]` | charges 5 s, then stops | protocol selected; SessionSetup first, CableCheck, PreCharge, PowerDelivery, a CurrentDemand loop, PowerDelivery, WeldingDetection, SessionStop last; FIN, no RST |
| `test_ev_unplug_mid_current_demand` | unplugs in CurrentDemand | ends in CurrentDemand: no PowerDelivery stop, no WeldingDetection, no SessionStop |
| `test_isolation_fault` | charges; the isolation monitor reads 10 kOhm | CableCheck, then no PreCharge and no CurrentDemand |
| `test_capture_fixture` | - | the committed captures pass the same link checks, and each sidecar row points at a V2GTP frame's start, in order and direction; needs only tshark |

## Captures

`captures/iso2-dc.pcapng` and `captures/din-dc.pcapng`: one full session each,
from `test_session`, about 20 KB. Link-local addresses from random MACs.

Each has a sidecar, `<name>.messages.csv`: one row per V2GTP frame, `frame`
(the capture's frame number of the segment holding its first byte),
`direction` (`ev` or `evse`) and `message` (e.g. `CurrentDemandReq`), named
from the EVSE's record matched to the wire. No EXI bytes: the record's
responses are not the wire's (below). `just v2g-record` writes both.

## Known EVerest behaviour

- EvseV2G 2025.10 records each response in its request's buffer: the request's
  V2GTP header and length, the response's EXI cut or padded to it. Requests are
  recorded exactly. The suite matches responses on their common prefix.
- Real SLAC (`EvseSlac`, `EvSlac`) over the veth does not match: without a
  modem no `CM_SET_KEY.CNF` comes back, so `EvseSlac` stays in reset and
  `EvSlac` fails. The HomePlug AV frames do cross the link.
