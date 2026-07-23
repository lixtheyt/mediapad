# Mediapad

---
<!-- HELYX_STATUS_START -->
![Status](https://img.shields.io/badge/status-active-brightgreen)
<!-- HELYX_STATUS_END -->
---

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

<img src=CAD/assets/image.png alt="Schematic" width="500"/>


## PCB
I made my PCB inside KiCad.
Libraries I used were originally from the Hackpad website but I also used some that I had found on [grabcad](https://grabcad.com/library)


### Schematic
<img src=PCB/assets/schematic.png alt="Schematic" width="600"/>


### PCB
<img src=PCB/assets/pcb.png alt="PCB" width="500"/>


## Firmware Overview
This hackpad uses [QMK](https://qmk.fm/) firmware for everything.


### Rotary encoder
Tapping the encoder cycles through five modes; turning it then performs that
mode's action. The mode name flashes on the OLED each time you tap.

1. **Volume** — volume up / down
2. **Track skip** — next / previous track
3. **Arrows** — arrow up / down
4. **Scroll** — mouse wheel up / down
5. **Zoom** — zoom in / out (Ctrl +/-)


### RGB
The lighting is separate from the encoder cycle and is opened with the two middle
keys, so it does not eat one of the modes above:

- **RGB Scene** — 6 premade animations: Pinwheel, Vortex (color swirl around
  the board), Rainbow wave, Chevron, Splash and Raindrops. On this layer the
  encoder switches scene or sets brightness.
- **RGB Custom** — mix your own color (red / green / blue / brightness, each dialled
  in with the encoder) and pick from ten effects, including reactive ones that
  light up outward from the key you press.


### OLED
Shows the currently playing track — title, artist and a progress bar — streamed
from the companion app below, plus the name of each mode when you switch.
When messing with RGB lighting it also shows current informations that are helpful for setting it correctly.


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

## Companion app (now playing on the OLED)

`Companion/mediapad_tray.exe` is a portable application made in Python that feeds Mediapad with the latest
information about current played media on your computer. Without it the OLED just shows "Hackpad".

As I have mentioned... it is a portable application. That means you don't have to install anything.
Just open, mess with it in the tray (where background applications are located) and make sure to enable
the setting that allows it to open on start up of your computer, so you don't have to turn it on manually
every time you turn on your computer. If you've got no idea where the tray with the background applications
is located then please, google it.