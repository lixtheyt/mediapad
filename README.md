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

- **RGB Scene** — six ready-made animations: Pinwheel, Vortex (colour swirl around
  the board), Rainbow wave, Chevron, Splash and Raindrops. On this layer the
  encoder switches scene or sets brightness.
- **RGB Custom** — mix your own colour (red / green / blue / brightness, each dialled
  in with the encoder) and pick from ten effects, including reactive ones that
  light up outward from the key you press.

The OLED turns into a small control panel on both RGB layers, showing the current
effect, the colour and what the encoder is about to change.

### OLED
Shows the currently playing track — title, artist and a progress bar — streamed
from the companion app below, plus the name of each mode when you switch.


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

`Companion/mediapad_tray.exe` is a small, tray-only Windows app that streams
whatever you are currently playing — Spotify, a browser tab, anything Windows
reports media for — to the mediapad's OLED over Raw HID. Without it the OLED just
shows "Hackpad".

It is portable: one file, nothing to install and no folders created next to it.
Double-click it and it lives entirely in the notification-area tray (there is no
window). A grey icon means the keyboard was not found; it turns green once the
mediapad is plugged in with this firmware flashed. Running it a second time does
nothing — only one instance ever stays in the tray.

Right-click the tray icon for the menu:

- **Send now playing** — pause or resume streaming without quitting
- **Run at startup** — add/remove a Windows startup entry (it also appears under
  **Task Manager → Startup**, so it can be disabled there too)
- **Uninstall** — asks to confirm, turns off startup and deletes the app itself
- **Quit** — stop it for this session

To rebuild the exe after editing `Companion/mediapad_tray.py`, run
`Companion/build.bat` (needs `pystray`, `pillow`, the `winrt` packages and
`pyinstaller`).
