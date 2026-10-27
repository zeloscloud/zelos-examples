# Energy rig

An open demo rig on a printed baseplate: an mj5208 driven by a moteus-c1 is coupled face to face to a second
mj5208 as the generator, into a 3-phase rectifier, a PZEM-017 meter, two relays and two 10 ohm 50 W load
resistors. A hand crank module bolts in where the drive motor goes, so a person can turn the generator.
Motors stay visible and cooled under a slotted guard; the electronics ride TS35 rail on printed carriers;
the resistors hang on an aluminium bar that nothing printed comes near. The control panel console talks to
it by cable.

- Seventeen prints: baseplate; two motor mounts (one print, placed twice); two coupling hubs (one print);
  crank hub, bearing block, arm, sleeve and grip; guard; carriers for the moteus-c1, the relay module and the
  rectifier; a tray for the PZEM-017; two resistor-bar feet.
- Baseplate 260 x 240 x 5 with a 10 mm skirt underneath, printed face down. Every part bolts through it
  into a nut in the skirt cavity. The motor axis runs along the plate 60 mm from the front edge, 75 mm up.
- mj5208 (mjbots): 63 x 25, 193 g, rotor face 3 x M3 on a 17 mm circle, base 4 x M3 on a 25 mm circle,
  axle stub out of the base. Each motor screws to a 6 mm upright through M3 x 10, reaching 4 mm into the
  stator; the mount's foot is slotted 12 mm along the axis for alignment, heads sunk 4 mm so the M3 x 10
  fills its nut under the plate. Confirm the base pattern's clocking and both thread depths
  on the mjbots drawing before the first print.
- Coupling: two identical 84 x 10 mm flange hubs. Three M3 x 10 from the mating face into each rotor,
  counterbored so they reach 3.5 mm; four M3 x 10 on a 76 mm circle join the hubs outside the 63 mm bells,
  two heads from each side sunk 6 mm into counterbores, each into a nut in a slot entered from the other
  hub's rim 2.5 mm behind its mating face. The generator hub is the drive hub turned round.
  Printed in PETG first; PA-CF or a machined hub if it creeps.
- Crank module: a 44 mm bearing block as long as the motor, on the drive mount's own pattern through four
  M3 inserts, two 608 bearings, an M8 x 60 bolt as the axle. The crank hub carries the bolt head in a hex
  pocket and joins the generator hub like the motor hub does. The arm (70 mm, 42 mm grip on a second M8 x 60) sits
  beyond the mount and past the plate's end, so it swings clear of plate and desk by 10 mm. Swapping motor
  for crank: four join bolts, four mount screws.
- Guard: a 2.5 mm U hood between the mounts, 50 mm inside radius, 5 mm off the hubs, flanges along both
  bottom edges bolted to the plate, three slots on top. Printed standing on an end.
- Electronics: two 240 mm TS35 stubs across the back, each on two M3. Carriers use the clip from parts/din:
  moteus-c1 (38 x 38, M2.5 at 32 x 32, M2.5 inserts), a 2-channel relay module (51 x 39, holes 45 x 33,
  placeholder), an MDS-type 3-phase bridge (80 x 36, M5 holes at 66, placeholder: M3 x 10 up from the
  rail channel through the 2.4 mm bridge, 9 mm washer and nut on the flange; bolt it on before clipping), and a tray with corner lips for the PZEM-017 case (90 x 60 x 23). The drive motor's 80 mm leads
  do not reach the moteus carrier: extend them, or move the carrier.
- Load: two Arcol HS50 (72.5 x 29.7 x 14.8, 10 ohm 50 W) on a 120 x 50 x 3 aluminium bar, one each side,
  bar standing 20 mm deep in two printed feet with M3 inserts in their bottoms, screwed from under the plate. Resistors stay 30 mm or more from every print; the bar
  itself touches only the feet. Drill the bar for the resistors; a bigger bar or a heatsink if the demo runs
  the load for more than a minute at a time.
- Fasteners: M3 x 10 for every joint but the M8 x 60 axle and grip. Prints set the reach: counterbores on
  the motor joints, the feet and the hub join, relief below each insert, 10 mm of skirt for the nuts.
- 24 V and 51 V max inside (moteus bus); the supply stays outside. Nothing printed near the resistors.
- Prints, all without supports: plate face down; mounts on the upright's face; hubs and arm flat; block on
  its mount face; guard on an end; carriers and feet flat. PETG. About 1.1 kg of filament.
- Open: the relay and rectifier modules (placeholders), the PZEM shunt's home, whether the moteus wants a
  bulk capacitor on the rail, and what the first print says about hub creep and bearing seat fit.
