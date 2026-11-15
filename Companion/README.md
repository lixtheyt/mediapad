# Mediapad companion

A tray-only background app for Windows. It reads whatever is currently playing
(Spotify, a browser tab, any app that reports media to Windows) and streams the
title, artist, progress and the shuffle/repeat state to the mediapad's OLED over
Raw HID.

## Running it

Just double-click **`mediapad_tray.exe`**. It is fully portable... one file,
nothing to install, no folders created next to it.

Right-click the tray icon for the menu:

| Item | What it does |
|------|--------------|
| *(top line)* | The track currently being sent |
| *Keyboard: …* | Connection status |
| **Send now playing** | Pause/resume streaming without quitting. The icon turns amber while it is paused |
| **Notify when the keyboard connects** | Shows a Windows notification every time the keyboard is found. Off by default |
| **Run at startup** | Adds/removes an `HKCU\…\Run` entry (also shows under **Task Manager → Startup**, so you can disable it there too) |
| **MediaPad on GitHub** | Opens this repository |
| **Uninstall** | Asks to confirm, removes the startup entry and its settings, then deletes the exe itself |
| **Quit** | Stops the app for this session |

The icon is green while the keyboard is connected, amber while sending is paused and grey when the
keyboard is not found. Hover over it to see what is playing.

**Send now playing** and **Notify when the keyboard connects** are remembered between restarts, in
`HKCU\Software\MediapadCompanion`. Nothing else is written anywhere.

## Uninstalling

Tray icon → **Uninstall** → *Yes*.

## Building the exe yourself

Only needed if you change `mediapad_tray.py`.

```bat
pip install -r requirements.txt
build.bat
```

The result is `dist\mediapad_tray.exe`.

## How it talks to the keyboard

Matches the firmware's `raw_hid_receive` in
`Firmware/QMK/keymaps/lixtheyt/keymap.c`. Both directions are one 32-byte report
to VID `0xFEED` / PID `0x4D50`, Raw HID usage page `0xFF60`. **The app and the
firmware share this layout, so they have to be updated together.**

### App → pad, every ~1 s

| Byte(s) | Meaning |
|---------|---------|
| 0 | `0x01` — now-playing packet id |
| 1 | flags, see below |
| 2–3 | position in seconds, little-endian |
| 4–5 | duration in seconds, little-endian |
| 6 | title length |
| 7 | artist length |
| 8–31 | title bytes, then artist bytes — 24 bytes shared between them |

Byte 1:

| Bit | Meaning |
|-----|---------|
| 0 | playing |
| 1 | shuffle on |
| 2–3 | repeat: 0 none, 1 track, 2 list |
| 4 | the last command was refused by the session |

Title and artist share one run rather than having fixed slots, so a short artist
buys the title room. The title is capped at 20 and the artist at 12, whichever
fits in the 24 bytes the title gets first call.

An empty title means "connected, nothing playing", not "no app". The pad tells
those apart on its idle screen.

### Pad → app

| Byte | Meaning |
|------|---------|
| 0 | `0x02` — command packet id |
| 1 | `0x01` toggle shuffle, `0x02` cycle repeat (none → track → list) |

Sent only while the pad is receiving now-playing packets; with no app running
those keys fall back to `Ctrl+S` / `Ctrl+R` instead. If the session refuses the
change — browsers and Windows Media Player report playback but do not implement
either — the app sets flag bit 4 on its next packet and the pad shows
`not supported here`.

Reading needs a second, read-only handle on its own thread: the write handle is
synchronous, so a blocking read on it would stall the packet loop.

HID is spoken through Windows' own `hid.dll` / `setupapi.dll` via `ctypes`, so
there is no third-party HID library and no native DLL to ship.

### A gotcha worth knowing

`MediaPlaybackAutoRepeatMode` lives in the `Windows.Media` namespace, not
`Windows.Media.Control`. With only the Control package installed, *reading*
`auto_repeat_mode` raises `AttributeError` — repeat then reads as "off" forever
and the cycle key never advances past the first mode. Hence the extra package in
the build requirements above.
