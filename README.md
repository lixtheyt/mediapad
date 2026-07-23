# Mediapad

Mediapad is an 8 key hackpad with a rotary encoder, an OLED Display. It also has 20 SK6812 MINI-E LEDs, and uses QMK firmware.

## Features:
- 128x32 OLED Display
- EC11 Rotary encode
- 20 SK6812 MINI-E LEDs. We gotta make somethin' for party nah??
- cool designed case

## CAD Model:
Everything fits together using 6 M3 Bolts and heatset inserts. 4 for the case and 2 for the PCB.

It has 4 separate printed pieces.
3 are parts of the case and 1 is a spacer for the glow...

<!-- image heree -->
<img src=CAD/assets/image.png alt="Schematic" width="500"/>

## PCB
I made my PCB inside KiCad.
Libraries I used were originally from the Hackpad website but I also used some that I had found on [grabcad](https://grabcad.com/library)

### Schematic
<!-- image here -->
<img src=PCB/assets/schematic.png alt="Schematic" width="600"/>

### PCB
<!-- image here -->
<img src=PCB/assets/pcb.png alt="PCB" width="500"/>

## Firmware Overview
This hackpad uses [QMK](https://qmk.fm/) firmware for everything. 

- the rotary encoder has few modes like
  1.  Volume mode
  2.  Switching track mode
  3.  Up/Down mode
  4.  Mouse Wheel Up/Down mode
  5.  Zooming mode
  6.  Changing colors
  7.  Changing color scenes
- the OLED is supposed to display the current playing track and when switching between modes then also the name of each mode
- more in future idk


## BOM:

- 8x Cherry MX Switches
- 8x Keycaps
- 6x M3x5x4 Heatset inserts
- 6x M3x16mm Screws
- 8x 1N4148 DO-35 Diodes
- 20x SK6812 MINI-E LEDs
- 1x 0.91" 128x32 OLED Display
- 1x EC11 Rotary Encoder
- 1x XIAO RP2040
- 1x Case (1x bottom case, 1x middle case, 1x top case)
- 6x Spacers (printed part)
