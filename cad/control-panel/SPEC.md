# Control panel console

A self-contained desk console: six 22 mm operators on a sloped face, the Waveshare Modbus RTU IO 8CH inside,
24 V and RS485 in through glands at the back. Built the way a desk console is built (Rittal TP, Hammond 1599):
the operating surface is a lid hinged at the rear, the equipment sits on a removable mounting plate, and the
cable entry is in a fixed wall. Replaces the Hammond 1554MPB6B option.

- Five prints: body, face plate, face legends (inlay for a second filament), deck plate, mounting plate.
- Lid: face plate and deck bolt together at the ridge, rib to rib, four M3 x 10 socket screws along the
  depth. Heads sit on the deck rib's back face, under the flat deck, where a straight hex driver reaches
  them from behind (no room there to swing an L key). Nuts sit in slots entered from the face rib's bottom
  edge, between the operator blocks, so the bolt cannot push them out and they go in with the operators
  fitted. The ridge ribs also stiffen the plates. Two knuckle hinges on brackets outside the back wall,
  each on an M3 x 50 socket screw with a nylock nut run up snug, not clamped; two bolts out and the lid
  lifts off. Two M3 thumbscrews at the front corners close it; an E-stop slap loads the front wall through
  the plate edge, not the screws. The lid opens to 115 degrees, where the deck top meets a post on each
  hinge bracket. With 60 g per operator the lid's centre of mass is then 6 mm behind the axis, so it rests
  on the stop, and its moment about the back edge is under a sixth of the body's.
- Face at 35 degrees from horizontal, 200 mm wide, 120 mm along the slope, 4 mm thick. Operators on a 55 mm
  pitch across, rows 50 mm apart: E-stop, START (illuminated), MODE (selector) above; RUN, WARN, FAULT
  pilot lamps below. Legends are set in Liberation Sans Bold from `fonts/`, so every machine cuts the same glyphs.
- Operator cutouts 22.3 mm with a 3.2 mm anti-rotation notch at the top (IEC 60947-5-1). Part numbers are
  not chosen yet: confirm the notch and the clamp range (most take 1 to 6 mm) against the datasheets.
- Clearance behind the plate is modelled as a 32 x 44 x 65 mm block per operator and checked against every
  print and the module, closed and through the whole swing. Datasheet depth for a block plus lamp holder is 50 to 60 mm.
- Mounting plate: 190 x 170 x 3 on four floor standoffs with M3 inserts, perforated at 20 mm for whatever
  comes next (terminal strip for the operator loom, duct). The rail stub (75 mm TS35) bolts to it with two
  M3 in the rail slots, nuts under the plate; the module sits in the rear bay with 15 mm wire room past each terminal end.
- Cable entry: two 16.5 mm holes in the back wall for M16 glands, above the module. The operator loom gets a
  service loop sized for the swing.
- 24 V SELV only inside; the supply stays outside. No protective earth on a plastic box.
- Prints, all without supports: body floor down (hinge brackets have a 45 degree underside, the stop posts
  stand on them); face and deck outer face down so the ribs and knuckles stand up, which puts the legend
  inlay in the first layers; mounting plate flat. PETG or ASA. Body about 335 cm3, face about 100 cm3,
  deck about 80 cm3.
- Fasteners: through-bolts and nuts wherever both sides are reachable (ridge, hinges); heat-set inserts only
  for the blind joints (lid thumbscrews into the front columns, mounting plate onto its 7 mm standoffs,
  which keep the insert hole clear of the floor).
- Open: whether the plate flexes under an E-stop slap at 4 mm (the test print settles it; PLATE_T goes
  to 5 if so), the legend wording, and the operator loom: lifting the mounting plate out means unplugging
  it at the module, until a pluggable terminal strip lands on the plate.
