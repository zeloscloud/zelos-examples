# V2G suite

Charging sessions between an EVerest EVSE and an EVerest EV over a veth pair,
each captured on the link: DC over ISO 15118-2 and DIN 70121, DC and AC over
ISO 15118-20, each also bidirectional (BPT). Software in the loop: no modems,
no CP line.

```bash
just test-v2g            # from the repository root; Linux Docker
just test-v2g -k din     # pytest arguments pass through
just test-v2g -k iso20   # the ISO 15118-20 sessions
just v2g-record          # record captures/ again
```

## The SIL

| Piece | What it is |
|---|---|
| Image | `everest-demo/manager` 2025.10.0-single-phase, pinned by digest (`Dockerfile`), plus Mosquitto, tshark and pytest, and EVerest's test PKI installed where its modules look |
| EVSE | `EvseV2G` on `v2g-evse`, no TLS, with EVerest's simulated DC supply, isolation monitor and board (`everest.yaml`). ISO 15118-20: `Evse15118D20` (libiso15118, cbv2g codec) in its place, no TLS; AC with the simulated board alone (`everest-ac.yaml`) |
| EV | `PyEvJosev` (Josev, EXIficient codec) driven by `EvManager`, standalone in its own network namespace on `v2g-ev`. ISO 15118-20: it offers one energy service first, `DC`, `DC_BPT`, `AC` or `AC_BPT`, which the EVSE selects |
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
named by the EVSE's record of them (`v2g_messages`): `EvseV2G`'s matched on
bytes, `Evse15118D20`'s, a name alone, on direction and V2GTP payload type
(0x8002 common, 0x8003 AC, 0x8004 DC).

| Test | EV does | Asserts |
|---|---|---|
| `test_session[iso2]`, `[din]` | charges 5 s, then stops | protocol selected; SessionSetup first, CableCheck, PreCharge, PowerDelivery, a CurrentDemand loop, PowerDelivery, WeldingDetection, SessionStop last; FIN, no RST |
| `test_session[iso20-dc]`, `[iso20-dc-bpt]` | the same over ISO 15118-20 | protocol selected; SessionSetup, AuthorizationSetup, Authorization, ServiceDiscovery first, DC_CableCheck, DC_PreCharge, PowerDelivery, a DC_ChargeLoop, PowerDelivery, DC_WeldingDetection, SessionStop last; FIN, no RST |
| `test_session[iso20-ac]`, `[iso20-ac-bpt]` | draws 16 A on three phases for 5 s, then stops | the same start, AC_ChargeParameterDiscovery, ScheduleExchange, PowerDelivery, an AC_ChargeLoop, PowerDelivery, SessionStop last; FIN, no RST |
| `test_ev_unplug_mid_current_demand` | unplugs in CurrentDemand | ends in CurrentDemand: no PowerDelivery stop, no WeldingDetection, no SessionStop |
| `test_isolation_fault` | charges; the isolation monitor reads 10 kOhm | CableCheck, then no PreCharge and no CurrentDemand |
| `test_capture_fixture` | - | the committed captures pass the same link checks, and each sidecar row points at a V2GTP frame's start, in order and direction; needs only tshark |

## Captures

One full session each, from `test_session`, 13 to 21 KB: `iso2-dc`, `din-dc`,
`iso20-dc`, `iso20-dc-bpt`, `iso20-ac`, `iso20-ac-bpt` (`.pcapng`). Link-local
addresses from random MACs. The ISO 15118-20 sessions run in dynamic control
mode, in plain TCP.

Each has a sidecar, `<name>.messages.csv`: one row per V2GTP frame, `frame`
(the capture's frame number of the segment holding its first byte),
`direction` (`ev` or `evse`) and `message` (e.g. `CurrentDemandReq`,
`DC_ChargeLoopReq`), named from the EVSE's record matched to the wire. No EXI bytes: the record's
responses are not the wire's (below). `just v2g-record` writes both.

## Known EVerest behaviour

- EvseV2G 2025.10 records each response in its request's buffer: the request's
  V2GTP header and length, the response's EXI cut or padded to it. Requests are
  recorded exactly. The suite matches responses on their common prefix.
- Evse15118D20 2025.10 records a message's name alone, its own spelling
  (`DcChargeLoopReq`); the sidecar has the schema's (`DC_ChargeLoopReq`).
- Evse15118D20 does not start without a V2G certificate, and Josev's
  ISO 15118-20 AuthorizationSetup reads the V2G root, TLS or not. The image
  ships empty certificate directories and EVerest's test PKI beside them, which
  the `Dockerfile` installs with EVerest's own script.
- `config-sil-ac-d20.yaml` starts its session with `iso_start_v2g_session AC`,
  which EvManager 2025.10 rejects (three arguments); `conftest.py` passes the
  departure time and energy amount too.
- Josev's ISO 15118-20 EV reports a constant PresentSOC (10).
- Real SLAC (`EvseSlac`, `EvSlac`) over the veth does not match: without a
  modem no `CM_SET_KEY.CNF` comes back, so `EvseSlac` stays in reset and
  `EvSlac` fails. The HomePlug AV frames do cross the link.
