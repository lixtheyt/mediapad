"""
Mediapad companion — a tray-only background app that streams the currently
playing track to the mediapad's OLED over Raw HID.

Single file, no window. Everything it needs is either in the Python stdlib
(ctypes talks to Windows' own hid.dll / setupapi.dll) or bundles cleanly into
one .exe with PyInstaller. See build.bat.

Tray menu lets you toggle "Run at startup" (a plain HKCU Run registry entry,
which also shows up under Task Manager > Startup so you can disable it there
too) and pause sending, or quit outright.
"""
import asyncio
import ctypes
import os
import sys
import threading
import time
import winreg
from ctypes import wintypes

import pystray
from PIL import Image, ImageDraw
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winrt.windows.media import MediaPlaybackAutoRepeatMode

# ---- must match the firmware -------------------------------------------------
VID, PID = 0xFEED, 0x4D50
RAW_USAGE_PAGE, RAW_USAGE_ID = 0xFF60, 0x61
REPORT_LEN = 32          # RAW_EPSIZE
TEXT_BYTES = 24
TITLE_MAX = 20
ARTIST_MAX = 12
PROTO_NOWPLAYING = 0x01

FLAG_PLAYING = 0x01
FLAG_SHUFFLE = 0x02
REPEAT_SHIFT = 2
FLAG_REJECTED = 0x10

PROTO_COMMAND = 0x02
CMD_TOGGLE_SHUFFLE = 0x01
CMD_CYCLE_REPEAT = 0x02
REPEAT_MODES = 3

APP_NAME = "MediapadCompanion"          # registry value / internal id (no spaces)
DISPLAY_NAME = "Mediapad Companion"     # what the user actually sees in dialogs
POLL_INTERVAL = 1.0      # seconds between OLED refreshes
STATUS_PLAYING = 4       # GlobalSystemMediaTransportControlsSessionPlaybackStatus
MUTEX_NAME = "Global\\MediapadCompanion_singleton"
ERROR_ALREADY_EXISTS = 183

# =============================================================================
# Raw HID over the Win32 API (no third-party libraries, no bundled native DLLs)
# =============================================================================
setupapi = ctypes.WinDLL("setupapi")
hid = ctypes.WinDLL("hid")
kernel32 = ctypes.WinDLL("kernel32")

FILE_FLAG_OVERLAPPED = 0x40000000
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x1
FILE_SHARE_WRITE = 0x2
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
DIGCF_PRESENT = 0x2
DIGCF_DEVICEINTERFACE = 0x10
HIDP_STATUS_SUCCESS = 0x00110000

class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]

class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD),
                ("Reserved", ctypes.POINTER(wintypes.ULONG))]

class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
                ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT)]

class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                ("InputReportByteLength", wintypes.USHORT),
                ("OutputReportByteLength", wintypes.USHORT),
                ("FeatureReportByteLength", wintypes.USHORT),
                ("Reserved", wintypes.USHORT * 17), ("fields", wintypes.USHORT * 10)]

HDEVINFO = wintypes.HANDLE
PSP_DID = ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)

setupapi.SetupDiGetClassDevsW.restype = HDEVINFO
setupapi.SetupDiGetClassDevsW.argtypes = [
    ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    HDEVINFO, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD, PSP_DID]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    HDEVINFO, PSP_DID, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HDEVINFO]

kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.CancelIoEx.restype = wintypes.BOOL
kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]

hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]

def _hid_guid():
    g = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(g))
    return g

# Kept alive for the whole process so the mutex stays held. A second launch
# sees it already exists and bows out, so the tray never doubles up.
_singleton_handle = None

def already_running():
    global _singleton_handle
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.GetLastError.restype = wintypes.DWORD
    _singleton_handle = kernel32.CreateMutexW(None, True, MUTEX_NAME)
    # Read the error straight after the create call, before anything else runs.
    return kernel32.GetLastError() == ERROR_ALREADY_EXISTS

def find_keyboard():
    """Open the mediapad's Raw HID interface. Returns (handle, device_path)."""
    guid = _hid_guid()
    info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if info == INVALID_HANDLE_VALUE:
        return None
    try:
        idx = 0
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(iface)
            if not setupapi.SetupDiEnumDeviceInterfaces(
                    info, None, ctypes.byref(guid), idx, ctypes.byref(iface)):
                break
            idx += 1

            need = wintypes.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                info, ctypes.byref(iface), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = 8  # cbSize, x64
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                    info, ctypes.byref(iface), buf, need.value,
                    ctypes.byref(need), None):
                continue
            path = ctypes.wstring_at(
                ctypes.addressof(buf) + ctypes.sizeof(wintypes.DWORD))

            h = kernel32.CreateFileW(
                path, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
            if h == INVALID_HANDLE_VALUE:
                continue

            if _matches(h):
                return h, path
            kernel32.CloseHandle(h)
        return None, None
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)

def open_reader(path):
    """A second handle, read-only, for the commands the pad sends back."""
    h = kernel32.CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        None, OPEN_EXISTING, 0, None)
    return None if h == INVALID_HANDLE_VALUE else h

def _matches(h):
    attrs = HIDD_ATTRIBUTES()
    attrs.Size = ctypes.sizeof(attrs)
    if not hid.HidD_GetAttributes(h, ctypes.byref(attrs)):
        return False
    if attrs.VendorID != VID or attrs.ProductID != PID:
        return False
    pre = ctypes.c_void_p()
    if not hid.HidD_GetPreparsedData(h, ctypes.byref(pre)):
        return False
    try:
        caps = HIDP_CAPS()
        if hid.HidP_GetCaps(pre, ctypes.byref(caps)) != HIDP_STATUS_SUCCESS:
            return False
        return caps.UsagePage == RAW_USAGE_PAGE and caps.Usage == RAW_USAGE_ID
    finally:
        hid.HidD_FreePreparsedData(pre)

def write_report(h, payload):
    """payload: 32 bytes. Windows prepends a report-id byte, so we send 33."""
    buf = bytes([0x00]) + bytes(payload)
    written = wintypes.DWORD(0)
    ok = kernel32.WriteFile(h, buf, len(buf), ctypes.byref(written), None)
    return bool(ok) and written.value == len(buf)

# =============================================================================
# Now-playing packet
# =============================================================================
def _ascii(text, limit):
    """The OLED font is ASCII; drop anything it can't draw and clip to length."""
    out = text.encode("ascii", "ignore")[:limit]
    return out

def build_packet(playing, shuffle, repeat, pos_s, dur_s, title, artist,
                 rejected=False):
    pkt = bytearray(REPORT_LEN)
    pkt[0] = PROTO_NOWPLAYING
    pkt[1] = ((FLAG_PLAYING if playing else 0)
              | (FLAG_SHUFFLE if shuffle else 0)
              | (FLAG_REJECTED if rejected else 0)
              | ((repeat & 0x03) << REPEAT_SHIFT))
    pos_s = max(0, min(0xFFFF, int(pos_s)))
    dur_s = max(0, min(0xFFFF, int(dur_s)))
    pkt[2] = pos_s & 0xFF
    pkt[3] = (pos_s >> 8) & 0xFF
    pkt[4] = dur_s & 0xFF
    pkt[5] = (dur_s >> 8) & 0xFF

    t = _ascii(title, TITLE_MAX)
    a = _ascii(artist, min(ARTIST_MAX, TEXT_BYTES - len(t)))
    pkt[6] = len(t)
    pkt[7] = len(a)
    pkt[8:8 + len(t)] = t
    pkt[8 + len(t):8 + len(t) + len(a)] = a
    return bytes(pkt)

async def apply_command(cmd):
    """Act on a command from the pad. Returns whether the session accepted it."""
    mgr = await MediaManager.request_async()
    session = mgr.get_current_session()
    if session is None:
        return False
    playback = session.get_playback_info()

    if cmd == CMD_TOGGLE_SHUFFLE:
        try:
            current = bool(playback.is_shuffle_active)
        except Exception:
            current = False
        return bool(await session.try_change_shuffle_active_async(not current))

    if cmd == CMD_CYCLE_REPEAT:
        try:
            raw = playback.auto_repeat_mode
            current = 0 if raw is None else int(getattr(raw, "value", raw))
        except Exception:
            current = 0
        nxt = (current + 1) % REPEAT_MODES
        nxt = MediaPlaybackAutoRepeatMode(nxt)
        return bool(await session.try_change_auto_repeat_mode_async(nxt))

    return False

async def read_now_playing():
    """Return (playing, pos_s, dur_s, title, artist) or None if nothing plays."""
    mgr = await MediaManager.request_async()
    session = mgr.get_current_session()
    if session is None:
        return None
    props = await session.try_get_media_properties_async()
    timeline = session.get_timeline_properties()
    playback = session.get_playback_info()

    playing = int(playback.playback_status) == STATUS_PLAYING
    pos = timeline.position.total_seconds()
    dur = timeline.end_time.total_seconds()
    # end_time is 0 for some sources (live streams); guard the progress math.
    if dur <= 0:
        dur = 0

    try:
        shuffle = bool(playback.is_shuffle_active)
    except Exception:
        shuffle = False
    try:
        raw = playback.auto_repeat_mode
        repeat = 0 if raw is None else int(getattr(raw, "value", raw))
    except Exception:
        repeat = 0
    if repeat not in (0, 1, 2):
        repeat = 0

    return (playing, shuffle, repeat, pos, dur,
            props.title or "", props.artist or "")

# =============================================================================
# Startup registry entry (also visible in Task Manager > Startup)
# =============================================================================
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"

def _launch_command():
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    runner = pyw if os.path.exists(pyw) else sys.executable
    return f'"{runner}" "{os.path.abspath(__file__)}"'

def startup_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, APP_NAME)
            return True
    except OSError:
        return False

def set_startup(enable):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
        if enable:
            winreg.SetValueEx(k, APP_NAME, 0, winreg.REG_SZ, _launch_command())
        else:
            try:
                winreg.DeleteValue(k, APP_NAME)
            except OSError:
                pass

# =============================================================================
# Uninstall — remove the startup entry and, when running as the frozen exe,
# have the file delete itself after we exit. Leaves nothing behind.
# =============================================================================
user32 = ctypes.WinDLL("user32")
shell32 = ctypes.WinDLL("shell32")
MB_YESNO = 0x4
MB_ICONQUESTION = 0x20
MB_ICONINFORMATION = 0x40
MB_SETFOREGROUND = 0x10000
MB_TOPMOST = 0x40000
IDYES = 6

user32.MessageBoxW.restype = ctypes.c_int
user32.MessageBoxW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT]
shell32.ShellExecuteW.restype = wintypes.HINSTANCE
shell32.ShellExecuteW.argtypes = [
    wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR,
    wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_int]

_MB_FRONT = MB_SETFOREGROUND | MB_TOPMOST

def confirm(text, title=DISPLAY_NAME):
    return user32.MessageBoxW(
        None, text, title, MB_YESNO | MB_ICONQUESTION | _MB_FRONT) == IDYES

def inform(text, title=DISPLAY_NAME):
    user32.MessageBoxW(None, text, title, MB_ICONINFORMATION | _MB_FRONT)

def uninstall_cleanup():
    """Delete the startup entry; schedule self-delete if we are the frozen exe."""
    set_startup(False)
    if not getattr(sys, "frozen", False):
        return
    exe = sys.executable
    # A running exe can't delete its own file while the process is still mapped.
    # Hand the job to a hidden .bat that retries del until the file unlocks
    # (i.e. once we've exited), then removes itself. Far more reliable than a
    # cmd /c one-liner, whose %, & and () get mangled through ShellExecute.
    import tempfile
    bat = os.path.join(tempfile.gettempdir(), "mediapad_uninstall.bat")
    with open(bat, "w", encoding="ascii") as f:
        f.write(
            "@echo off\r\n"
            ":retry\r\n"
            f'del /f /q "{exe}" 2>nul\r\n'
            f'if exist "{exe}" (\r\n'
            "  ping -n 2 127.0.0.1 >nul\r\n"
            "  goto retry\r\n"
            ")\r\n"
            'del /f /q "%~f0"\r\n'
        )
    shell32.ShellExecuteW(None, "open", bat, None, None, 0)  # 0 == SW_HIDE

# =============================================================================
# Tray application
# =============================================================================
class Companion:
    def __init__(self):
        self.stop = threading.Event()
        self.sending = True
        self.connected = False
        self.read_handle = None
        self.cmd_rejected = False
        self.now_text = "Nothing playing"
        self.icon = pystray.Icon(APP_NAME, self._image(False), "Mediapad companion",
                                 menu=self._menu())

    # ---- icon art (generated, so there is no .ico file to ship) ----
    def _image(self, connected):
        img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        ring = (60, 200, 120, 255) if connected else (120, 120, 120, 255)
        d.rounded_rectangle((6, 6, 58, 58), radius=14, fill=(24, 24, 28, 255),
                            outline=ring, width=4)
        # little "play" glyph
        d.polygon([(26, 22), (26, 42), (44, 32)], fill=ring)
        return img

    def _menu(self):
        return pystray.Menu(
            pystray.MenuItem(lambda i: self.now_text, None, enabled=False),
            pystray.MenuItem(
                lambda i: "Keyboard: connected" if self.connected
                else "Keyboard: not found", None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Send now playing", self._toggle_sending,
                            checked=lambda i: self.sending),
            pystray.MenuItem("Run at startup", self._toggle_startup,
                            checked=lambda i: startup_enabled()),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Uninstall", self._uninstall),
            pystray.MenuItem("Quit", self._quit),
        )

    def _toggle_sending(self, icon, item):
        self.sending = not self.sending

    def _toggle_startup(self, icon, item):
        set_startup(not startup_enabled())

    def _uninstall(self, icon, item):
        # The dialog MUST run off the tray thread: the popup menu still owns the
        # mouse capture while this callback fires, so a MessageBox opened here
        # shows but never receives its own clicks (buttons only hover-highlight).
        # A separate thread runs after the menu loop has released capture.
        threading.Thread(target=self._uninstall_worker, args=(icon,),
                         daemon=True).start()

    def _uninstall_worker(self, icon):
        frozen = getattr(sys, "frozen", False)
        last = ("This program file will delete itself." if frozen
                else "Running from source — the .py file stays where it is.")
        msg = (
            "Uninstall Mediapad Companion?\n\n"
            "•  Run-at-startup will be turned off.\n"
            f"•  {last}\n\n"
            "The keyboard keeps working — only the OLED\n"
            "now-playing display stops."
        )
        if not confirm(msg):
            return
        uninstall_cleanup()
        if not frozen:
            inform("Startup entry removed.\n\nYou can delete mediapad_tray.py now.")
        self.stop.set()
        icon.stop()

    def _quit(self, icon, item):
        self.stop.set()
        icon.stop()

    def _reader(self):
        """Blocking reads on their own thread."""
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        while not self.stop.is_set():
            h = self.read_handle
            if h is None:
                self.stop.wait(0.5)
                continue

            buf = ctypes.create_string_buffer(REPORT_LEN + 1)
            got = wintypes.DWORD(0)
            ok = kernel32.ReadFile(h, buf, REPORT_LEN + 1, ctypes.byref(got), None)
            if not ok or got.value < 2:
                self.stop.wait(0.2)
                continue

            data = buf.raw[1:got.value]
            if len(data) < 2 or data[0] != PROTO_COMMAND:
                continue
            try:
                if not loop.run_until_complete(apply_command(data[1])):
                    self.cmd_rejected = True
            except Exception:
                self.cmd_rejected = True

    # ---- background worker ----
    def _worker(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        handle = None
        last_icon_connected = None

        while not self.stop.is_set():
            if handle is None:
                handle, path = find_keyboard()
                self.connected = handle is not None
                if handle is not None and self.read_handle is None:
                    self.read_handle = open_reader(path)

            if handle is not None and self.sending:
                try:
                    info = loop.run_until_complete(read_now_playing())
                except Exception:
                    info = None

                rejected, self.cmd_rejected = self.cmd_rejected, False

                if info is None:
                    self.now_text = "Nothing playing"
                    pkt = build_packet(False, False, 0, 0, 0, "", "", rejected)
                else:
                    playing, shuffle, repeat, pos, dur, title, artist = info
                    self.now_text = (f"{title} - {artist}" if artist else title) \
                        or "Nothing playing"
                    pkt = build_packet(playing, shuffle, repeat, pos, dur,
                                       title, artist, rejected)

                if not write_report(handle, pkt):
                    kernel32.CloseHandle(handle)
                    handle = None
                    self.connected = False
                    if self.read_handle is not None:
                        kernel32.CancelIoEx(self.read_handle, None)
                        kernel32.CloseHandle(self.read_handle)
                        self.read_handle = None

            # refresh the tray icon colour only when connection state flips
            if self.connected != last_icon_connected:
                last_icon_connected = self.connected
                try:
                    self.icon.icon = self._image(self.connected)
                    self.icon.update_menu()
                except Exception:
                    pass

            self.stop.wait(POLL_INTERVAL)

        if handle is not None:
            kernel32.CloseHandle(handle)
        if self.read_handle is not None:
            kernel32.CancelIoEx(self.read_handle, None)
            kernel32.CloseHandle(self.read_handle)
            self.read_handle = None

    def run(self):
        threading.Thread(target=self._worker, daemon=True).start()
        threading.Thread(target=self._reader, daemon=True).start()
        self.icon.run()

if __name__ == "__main__":
    # One tray icon only: a second launch exits immediately.
    if already_running():
        sys.exit(0)
    Companion().run()
