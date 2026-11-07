# Bought parts

Everything the three designs need besides the prints. The mounting plate (backplane) in the console is a print,
not a purchase. Quantities are for one of each design.

## Operators, 22 mm (control panel)

Two families that fit the cutouts as drawn (22.3 hole, 3.2 notch). Pick one; mixing is fine electrically.

| Qty | Part | Schneider Harmony XB5 (complete units, contacts included) | IDEC YW (cheaper, same cutout) |
|---|---|---|---|
| 1 | E-stop, 40 mm mushroom, turn to release, 1NC | [XB5AS8445](https://www.standardelectricsupply.com/356106-Schneider-Electric-XB5AS8445) | [YW1B-V4E01R](https://octopart.com/part/idec/YW1B-V4E01R) |
| 1 | START, illuminated pushbutton, green, 24 V LED, 1NO+1NC | [XB5AW33B5](https://www.powermatic.net/part/schneider-electric/xb5aw33b5/1165232) | YW1L-M2E10Q4G, confirm |
| 1 | MODE, 2-position selector, 1NO | [XB5AD21](https://www.digikey.com/en/products/detail/schneider-electric/XB5AD21/16613143) | YW1S-2E10, confirm |
| 1 | RUN pilot light, green, 24 V | [XB5AVB3](https://www.steinerelectric.com/Product/Schneider-Electric-XB5AVB3-Harmony-XB5-Pilot-LightPlain-LensPanel-Mount-273235) | YW1P-1EQ4G, confirm |
| 1 | WARN pilot light, orange, 24 V | [XB5AVB5](https://steinerelectric.com/Product/Schneider-Electric-XB5AVB5-PILOT-LIGHT-LED-24V-22M-290650) | YW1P-1EQ4A, confirm |
| 1 | FAULT pilot light, red, 24 V | [XB5AVB4](https://one-elec.com/en/xb5avb4-pilot-light-plastic-red-o22-plain-lens-with-integral-led-24-v-acdc-xb5avb4-schneider.html) | YW1P-1EQ4R, confirm |

US street price for the XB5 E-stop runs 95 to 200 USD; the IDEC E-stop runs 20 to 35 USD. Before ordering, check
the chosen family's clamp range against the 4 mm plate and its notch against the 3.2 mm drawn.

## Electrical (control panel)

| Qty | Part | Link |
|---|---|---|
| 1 | Waveshare Modbus RTU IO 8CH, 8 DI + 8 DO, RS485, 7 to 36 V | [waveshare.com](https://www.waveshare.com/modbus-rtu-io-8ch.htm) |
| 2 | M16 nylon cable gland, 4 to 8 mm cable, with locknut | [Adam Tech CG-P-M16 at DigiKey](https://www.digikey.com/en/products/detail/adam-tech/CG-P-M16/17736937) or [RND 465 M16 at RS](https://uk.rs-online.com/web/p/cable-glands/0490880) |
| 1 | 24 V supply, stays outside the box | existing |
| as needed | 22 AWG hookup wire, ferrules, twisted pair for RS485 | existing |

## Rail

| Qty | Part | Link |
|---|---|---|
| 2 m | TS35 slotted steel DIN rail, 35 x 7.5 (console 75, stand 2 x 300, rig 2 x 240) | [McMaster 8961K15](https://www.mcmaster.com/8961K15/) |
| 4 | DIN rail end clamp, screwless | [Phoenix CLIPFIX 35, 3022218](https://rspsupply.com/p-1838-phoenix-contact-3022218-end-bracket-clipfix-35.aspx) |

## Fasteners

Five screw kinds cover every design, one length per thread. The prints set each screw's reach with
counterbores, relief holes under inserts and nut slots. Socket head, stainless or zinc; McMaster category
pages, any supplier's equivalent is fine. Counts here are for the console, stand and carrier; the rig's are
in its own section at the end.

| Qty | Part | Where it goes | Link |
|---|---|---|---|
| 20 | M3 x 10 socket head | console ridge 4, plate 4, rail stub 2; stand rails 4; carrier board 5 | [McMaster M3 socket head](https://www.mcmaster.com/products/socket-head-screws/thread-size~m3/) |
| 2 | M3 x 50 socket head | console hinge pins | same page |
| 2 | M3 x 9 knurled thumbscrew, 18-8 | console lid | [McMaster 92545A115](https://www.mcmaster.com/92545A115/) |
| 10 | M3 hex nut | ridge 4, rail stub 2, stand 4 | [McMaster hex nuts](https://www.mcmaster.com/products/hex-nuts/thread-size~m3/) |
| 2 | M3 nylon-insert locknut | hinge pins | [McMaster hex locknuts](https://www.mcmaster.com/products/hex-locknuts/thread-size~m3/) |
| 4 | M3 flat washer | under the stand's rail bolt heads | [McMaster washers](https://www.mcmaster.com/products/washers/thread-size~m3/) |
| 11 | M3 heat-set insert, 5.7 long, 4.0 hole | console columns 2, standoffs 4; carrier bosses 5 | [McMaster 94459A140](https://www.mcmaster.com/94459A140/) or [ruthex RX-M3x5.7](https://3djake.com/ruthex/threaded-insert-m3-100-pieces) |
| 1 | heat-set insert tip for the soldering iron, M3 | install | [McMaster heat-set insert tips](https://www.mcmaster.com/products/heat-set-insert-tips/) |

## Print material

PETG, about 850 g for the bench parts (console 600 g, stand 190 g, carrier 50 g, at 1.27 g/cm3); the rig adds 1.1 kg. A second colour for
the legend inlay, a few grams.

## Not bought yet

A pluggable terminal strip for the operator loom on the mounting plate, once the operators are chosen.

## Later: energy rig

Not for the office pass. Everything the motor and generator rig needs, printed parts aside.

| Qty | Part | Link |
|---|---|---|
| 2 | mjbots mj5208 brushless motor, 63 x 25, 330 Kv | [mjbots](https://mjbots.com/products/mj5208) |
| 1 | mjbots moteus-c1 controller, 38 x 38 | [mjbots](https://mjbots.com/products/moteus-c1) |
| 1 | 3-phase bridge rectifier module, MDS type, 50 A | [MDS50-16 at RS](https://uk.rs-online.com/web/p/bridge-rectifiers/0192764), confirm the footprint |
| 1 | PZEM-017 DC meter with 50 A shunt | [robu.in listing](https://st.robu.in/product/pzem-017-module50a-shunt-dc-meter), or any Peacefair seller |
| 1 | 2-channel relay module, 24 V coil | to choose; the carrier takes 51 x 39 with holes at 45 x 33 |
| 2 | Arcol HS50 10R J, 10 ohm 50 W chassis mount | [RS 166-4187](https://uk.rs-online.com/web/p/chassis-mount-resistors/1664187) |
| 1 | Aluminium flat bar, 1/8 x 2 in, 120 mm long | [McMaster aluminium bars](https://www.mcmaster.com/products/aluminum-bars/) |
| 2 | 608 bearings, 8 x 22 x 7 | [McMaster 608 ball bearings](https://www.mcmaster.com/products/ball-bearings/trade-number~608/) |


Rig fasteners, same kinds as above:

| Qty | Part | Where it goes |
|---|---|---|
| 40 | M3 x 10 socket head | motors 8, hubs to rotors 6, hub join 4, block 4, feet 4, guard 4, rails 4, resistor feet 4, rectifier 2, relay 4 |
| 2 | M8 x 60 hex bolt + nylock + washers | crank axle, crank grip |
| 18 | M3 hex nut | hub join, feet, guard, rails, rectifier |
| 2 | M3 fender washer, 9 mm | rectifier flange, over its M5 holes |
| 12 | M3 heat-set insert | block 4, relay carrier 4, resistor feet 4 |
| 4 | M2.5 heat-set insert + M2.5 x 6 screws | moteus carrier |
