AudioLink - PCB fabrication specification
=========================================

Board            AudioLink
Revision         1.0
Size             100.0 x 56.0 mm  (rounded corners, R3.0; the Gerber job file
                 says 100.1 x 56.1 because it includes the outline line width)
Layers           4
Thickness        1.6 mm
Material         FR4, Tg 130 or better
Surface finish   HASL, leaded (SnPb)  - see note 1
Copper weight    outer 1 oz, inner 1 oz
Solder mask      both sides
Silkscreen       top side only  (the bottom silkscreen file is present but
                 empty - nothing is printed on the bottom)
Panelisation     single board

Stackup          no controlled impedance - use your standard 4-layer 1.6 mm build

Design rules used
-----------------
  min track width        0.15 mm
  min clearance          0.15 mm  (most nets routed at 0.20 mm)
  min via                0.60 mm pad / 0.30 mm drill
  min hole-to-hole       0.50 mm
  min copper-to-hole     0.20 mm
  min copper-to-edge     0.30 mm
  min silk width         0.12 mm

Files
-----
  gerbers/         standard KiCad Gerber and Excellon drill files (metric,
                   absolute) with a Gerber job file. In1 is a solid GND plane,
                   In2 is signal + GND.

Assembly (SMT + THT)
--------------------
  AudioLink-BOM.csv               bill of materials
  AudioLink-centroid.csv          centroid / pick-and-place
  AudioLink-assembly-top.pdf      assembly drawing (F.Fab + outline + silk)
  AudioLink-schematic.pdf         schematic

  Placements       83, all on the TOP side. Nothing is placed on bottom.
  THT parts        J1, J2 (Neutrik NRJ6HM-1 jacks), J4 (2x5 1.27 mm header),
                   J3 (USB-C: SMD signal pins plus 4 plated shell slots, so
                   the BOM lists it as THT)
  SMT parts        all others, including U1 (66-pad module) and
                   U2 (VQFN-32 with exposed pad - needs a stencil)
  R6               NOT FITTED - see note 8
  H1-H4            mounting holes, no part - correctly absent from both files

Centroid conventions
--------------------
  Units            mm
  Origin           absolute, same coordinate system as the gerbers and drill
                   files - use the values as they are, no offset or flip.
                   The board occupies X 101.0 to 201.0 and Y -128.0 to -72.0
                   in all three (Y-up, so every Y value is negative).
  Rotation         counter-clockwise, degrees, 0 = footprint as drawn
  Layer column     "top" for every part

Notes
-----
1. Surface finish is leaded HASL (SnPb).
   U1 (MinewSemi MS45SF1) is a bottom-terminated LGA module: 17 of its pads
   sit under the module body, 0.4 mm wide on a 0.7 mm pitch, next to two
   1.2 x 2.2 mm ground pads. Those joints cannot be inspected or reworked
   once the module is placed, and HASL is not a coplanar finish, so please
   take extra care with pad levelling under U1. X-ray inspection of U1 is
   requested if your assembly service offers it.
   If the board is assembled with leaded solder paste, that is preferred -
   it matches the leaded finish and avoids a mixed SnPb/SAC alloy.

2. Silkscreen outside the board outline is expected and may be clipped.
   Three parts deliberately overhang the edge: U1 (module antenna, so it
   clears the ground plane), J1 and J2 (jack mouths, so they clear an
   enclosure panel). Their body outlines cross Edge.Cuts by design.
   J3 (USB-C) has only its courtyard and fabrication outline past the edge,
   no silkscreen.

3. Reference designators are on the fabrication layer, not silkscreen.
   Use the assembly drawing and position file for placement.

4. J3 (USB-C receptacle) has a pad 0.185 mm from its own NPTH mounting
   hole. This is the connector vendor's footprint geometry, not a design
   error - please build as drawn.

5. 17 BOM lines have no manufacturer part number. These are generic passives
   (resistors and ceramic capacitors) - 61 parts in total.
   Please substitute equivalents to the Value field, which carries the full
   spec: value / voltage / dielectric. The Notes column of every BOM line
   carries a purchase link: an LCSC part page where one exists, otherwise a
   Digikey or Mouser search. Three constraints are NOT optional:

     - C0G/NP0 on C15, C16 (crystal load) and C30-C33 (audio output EMI
       shunts). X7R is piezoelectric and measurably degrades THD. All
       other capacitors are X7R and must not be "upgraded" to C0G.
     - 1% tolerance on the 100R output resistors R8-R11. They set the
       balanced source impedance and their match sets CMRR.
     - 1% tolerance on R1, R2 (5.1k USB-C CC pulldowns). Out-of-tolerance
       parts make e-marked cables read as invalid and deliver 0 V.

6. D5 is a blue 0603 LED on a 3.3 V rail through 470R, so it has very little
   forward-voltage headroom. The specified part (GL0603UB01, LCSC C51933294)
   is rated Vf 2.6-2.9 V, which puts it at 0.85-1.5 mA. Any substitute must
   also be a LOW-Vf blue - a 3.2 V part draws 0.2 mA and is invisible.

7. Build all vias as standard tented vias, NOT resin filled or capped. The
   only vias in a pad are two 0.3 mm ground vias in the U2 (PCM5242) exposed
   pad, between its paste windows - build them as drawn, unfilled.

8. R6 is intentionally NOT fitted. It is absent from the BOM and centroid
   and crossed out on the assembly drawing. Its pads still have paste
   apertures in the stencil file, which is expected. Do not fit R6: with R7
   fitted it would short a microcontroller pin to ground.
