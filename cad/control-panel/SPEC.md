# Control panel console

A self-contained desk console: six 22 mm operators on a sloped face, the Waveshare Modbus RTU IO 8CH inside,
24 V and RS485 in through glands at the back. Built the way a desk console is built (Rittal TP, Hammond 1599):
the operating surface is a lid hinged at the rear, the equipment sits on a removable mounting plate, and the
cable entry is in a fixed wall. Replaces the Hammond 1554MPB6B option.

- Five prints: body, face plate, face legends (inlay for a second filament), deck plate, mounting plate.
- Lid: face plate and deck bolt together at the ridge, rib to rib, three M3 bolts into nuts held captive in
  the deck rib, reachable with the lid open. The ridge ribs also stiffen the plates. Two pin hinges on brackets outside the back wall, 3 mm x 40 mm
  pins (cut rod; ream the deck knuckles to 3.0, printed horizontal bores come out small). Pull the pins and
  the lid lifts off. Two M3 thumbscrews at the front corners close it; an E-stop slap loads the front wall
  through the plate edge, not the screws. The lid opens to 180 and lies back on the hinge; no stay yet.
- Face at 35 degrees from horizontal, 200 mm wide, 120 mm along the slope, 4 mm thick. Two rows at a 55 mm
  pitch: E-stop, START (illuminated), MODE (selector) above; RUN, WARN, FAULT pilot lamps below.
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
- Prints, all without supports: body floor down (hinge brackets have a 45 degree underside); face and deck
  outer face down so the ribs and knuckles stand up, which puts the legend inlay in the first layers; mounting
  plate flat. PETG or ASA. Body about 325 cm3, lid plates about 90 cm3 each.
- Fasteners: through-bolts and nuts wherever both sides are reachable; heat-set inserts only for the blind
  joints (lid thumbscrews into the front columns, mounting plate onto its standoffs).
- Open: whether the plate flexes under an E-stop slap at 4 mm (the test print settles it; PLATE_T goes
  to 5 if so), the legend wording, and a stay or stop for the open lid.
