# NUCLEO-144 DIN carrier

Carries a NUCLEO-144 board (MB1137: H753ZI, H743ZI, F429ZI and siblings) on a TS35 rail, long axis along the rail.

- Board 133.34 x 70.00 mm, five M3 holes. Source: UM1974 rev 7, figure 6.
- Hole pattern, board frame, x from the left edge, y from the bottom (USB) edge:
  (10.87, 105.7) top-left, read off the drawing, +-0.5; (59.13, 105.08); (43.89, 54.28); (12.14, 29.21); (57.86, 29.21).
- Five 11 mm bosses, 7 mm tall so the bottom-side pins clear the plate. Each holds an M3 nut in a slot entered
  from the side; the board screws down into it with M3 x 8, and the hole runs on through the plate so the screw
  cannot bottom. No inserts, no threads in plastic.
- Clip lives inside the plate: a rail recess in the underside, two fixed teeth on the top flange, one in-plane
  cantilever tongue with a tooth on the bottom flange, a release rib on its free end. The tongue tooth has a
  45 degree lead-in so the flange cams it aside as the rail clicks in. Rail 35.0 mm, flange 1.0 mm.
- Connectors (ST-LINK USB, user USB, RJ45) overhang the short edges and stay clear of the plate.
- Prints flat, underside on the bed, no supports. The recess ceiling is a 35.6 mm bridge.
- One test print settles what the model cannot: the tongue stiffness and the 0.3 mm rail clearance.
