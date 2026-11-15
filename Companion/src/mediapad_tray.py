import asyncio
import ctypes
import os
import sys
import tempfile
import threading
import webbrowser
import winreg
from ctypes import wintypes

import pystray
from PIL import Image, ImageDraw
from winrt.windows.media import MediaPlaybackAutoRepeatMode
from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as MediaManager

VID, PID = 0xFEED, 0x4D50
RAW_USAGE_PAGE, RAW_USAGE_ID = 0xFF60, 0x61
REPORT_LEN = 32
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

APP_NAME = "MediapadCompanion"
DISPLAY_NAME = "Mediapad Companion"
PROJECT_URL = "https://github.com/lixtheyt/mediapad"
POLL_INTERVAL = 1.0
STATUS_PLAYING = 4
MUTEX_NAME = "Global\\MediapadCompanion_singleton"
ERROR_ALREADY_EXISTS = 183

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
SETTINGS_KEY = r"Software\MediapadCompanion"

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x1
FILE_SHARE_WRITE = 0x2
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
DIGCF_PRESENT = 0x2
DIGCF_DEVICEINTERFACE = 0x10
HIDP_STATUS_SUCCESS = 0x00110000

MB_YESNO = 0x4
MB_ICONQUESTION = 0x20
MB_ICONINFORMATION = 0x40
MB_SETFOREGROUND = 0x10000
MB_TOPMOST = 0x40000
IDYES = 6
SW_HIDE = 0

GREEN = (60, 200, 120, 255)
AMBER = (230, 170, 50, 255)
GREY = (120, 120, 120, 255)
DARK = (24, 24, 28, 255)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", ctypes.POINTER(wintypes.ULONG))]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
                ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                ("InputReportByteLength", wintypes.USHORT),
                ("OutputReportByteLength", wintypes.USHORT),
                ("FeatureReportByteLength", wintypes.USHORT),
                ("Reserved", wintypes.USHORT * 17), ("fields", wintypes.USHORT * 10)]


setupapi = ctypes.WinDLL("setupapi")
hid = ctypes.WinDLL("hid")
kernel32 = ctypes.WinDLL("kernel32")
user32 = ctypes.WinDLL("user32")
shell32 = ctypes.WinDLL("shell32")

HDEVINFO = wintypes.HANDLE
INTERFACE_DATA = ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)

setupapi.SetupDiGetClassDevsW.restype = HDEVINFO
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [HDEVINFO, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD, INTERFACE_DATA]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [HDEVINFO, INTERFACE_DATA, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HDEVINFO]

kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.CancelIoEx.restype = wintypes.BOOL
kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
kernel32.CreateMutexW.restype = wintypes.HANDLE
kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.GetLastError.restype = wintypes.DWORD

hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]

user32.MessageBoxW.restype = ctypes.c_int
user32.MessageBoxW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT]
shell32.ShellExecuteW.restype = wintypes.HINSTANCE
shell32.ShellExecuteW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_int]

singleton = None


def already_running():
    global singleton

    singleton = kernel32.CreateMutexW(None, True, MUTEX_NAME)

    return kernel32.GetLastError() == ERROR_ALREADY_EXISTS


def is_mediapad(handle):
    attributes = HIDD_ATTRIBUTES()
    attributes.Size = ctypes.sizeof(attributes)

    if not hid.HidD_GetAttributes(handle, ctypes.byref(attributes)):
        return False

    if attributes.VendorID != VID or attributes.ProductID != PID:
        return False

    preparsed = ctypes.c_void_p()

    if not hid.HidD_GetPreparsedData(handle, ctypes.byref(preparsed)):
        return False

    try:
        caps = HIDP_CAPS()

        if hid.HidP_GetCaps(preparsed, ctypes.byref(caps)) != HIDP_STATUS_SUCCESS:
            return False

        return caps.UsagePage == RAW_USAGE_PAGE and caps.Usage == RAW_USAGE_ID
    finally:
        hid.HidD_FreePreparsedData(preparsed)


def find_keyboard():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))

    devices = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)

    if devices == INVALID_HANDLE_VALUE:
        return None, None

    try:
        index = 0

        while True:
            interface = SP_DEVICE_INTERFACE_DATA()
            interface.cbSize = ctypes.sizeof(interface)

            if not setupapi.SetupDiEnumDeviceInterfaces(devices, None, ctypes.byref(guid), index, ctypes.byref(interface)):
                return None, None

            index += 1

            needed = wintypes.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(devices, ctypes.byref(interface), None, 0, ctypes.byref(needed), None)

            detail = ctypes.create_string_buffer(needed.value)
            ctypes.cast(detail, ctypes.POINTER(wintypes.DWORD))[0] = 8

            if not setupapi.SetupDiGetDeviceInterfaceDetailW(devices, ctypes.byref(interface), detail, needed.value, ctypes.byref(needed), None):
                continue

            path = ctypes.wstring_at(ctypes.addressof(detail) + ctypes.sizeof(wintypes.DWORD))
            handle = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)

            if handle == INVALID_HANDLE_VALUE:
                continue

            if is_mediapad(handle):
                return handle, path

            kernel32.CloseHandle(handle)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(devices)


def open_reader(path):
    handle = kernel32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)

    return None if handle == INVALID_HANDLE_VALUE else handle


def write_report(handle, payload):
    report = bytes([0x00]) + bytes(payload)
    written = wintypes.DWORD(0)

    return bool(kernel32.WriteFile(handle, report, len(report), ctypes.byref(written), None)) and written.value == len(report)


def build_packet(playing, shuffle, repeat, position, duration, title, artist, rejected=False):
    packet = bytearray(REPORT_LEN)
    position = max(0, min(0xFFFF, int(position)))
    duration = max(0, min(0xFFFF, int(duration)))
    title = title.encode("ascii", "ignore")[:TITLE_MAX]
    artist = artist.encode("ascii", "ignore")[:min(ARTIST_MAX, TEXT_BYTES - len(title))]

    packet[0] = PROTO_NOWPLAYING
    packet[1] = ((FLAG_PLAYING if playing else 0)
                 | (FLAG_SHUFFLE if shuffle else 0)
                 | (FLAG_REJECTED if rejected else 0)
                 | ((repeat & 0x03) << REPEAT_SHIFT))
    packet[2] = position & 0xFF
    packet[3] = (position >> 8) & 0xFF
    packet[4] = duration & 0xFF
    packet[5] = (duration >> 8) & 0xFF
    packet[6] = len(title)
    packet[7] = len(artist)
    packet[8:8 + len(title)] = title
    packet[8 + len(title):8 + len(title) + len(artist)] = artist

    return bytes(packet)


def shuffle_of(playback):
    try:
        return bool(playback.is_shuffle_active)
    except Exception:
        return False


def repeat_of(playback):
    try:
        mode = playback.auto_repeat_mode
        mode = 0 if mode is None else int(getattr(mode, "value", mode))
    except Exception:
        return 0

    return mode if mode in (0, 1, 2) else 0


async def apply_command(command):
    manager = await MediaManager.request_async()
    session = manager.get_current_session()

    if session is None:
        return False

    playback = session.get_playback_info()

    if command == CMD_TOGGLE_SHUFFLE:
        return bool(await session.try_change_shuffle_active_async(not shuffle_of(playback)))

    if command == CMD_CYCLE_REPEAT:
        return bool(await session.try_change_auto_repeat_mode_async(
            MediaPlaybackAutoRepeatMode((repeat_of(playback) + 1) % REPEAT_MODES)))

    return False


async def read_now_playing():
    manager = await MediaManager.request_async()
    session = manager.get_current_session()

    if session is None:
        return None

    properties = await session.try_get_media_properties_async()
    timeline = session.get_timeline_properties()
    playback = session.get_playback_info()

    return (int(playback.playback_status) == STATUS_PLAYING,
            shuffle_of(playback),
            repeat_of(playback),
            timeline.position.total_seconds(),
            max(0, timeline.end_time.total_seconds()),
            properties.title or "",
            properties.artist or "")


def setting(name, default):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SETTINGS_KEY) as key:
            return bool(winreg.QueryValueEx(key, name)[0])
    except OSError:
        return default


def save_setting(name, value):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, SETTINGS_KEY) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(value))


def startup_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, APP_NAME)

            return True
    except OSError:
        return False


def set_startup(enable):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
        if not enable:
            try:
                winreg.DeleteValue(key, APP_NAME)
            except OSError:
                pass

            return

        windowless = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
        runner = windowless if os.path.exists(windowless) else sys.executable

        winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ,
                          f'"{sys.executable}"' if getattr(sys, "frozen", False)
                          else f'"{runner}" "{os.path.abspath(__file__)}"')


def uninstall():
    set_startup(False)

    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, SETTINGS_KEY)
    except OSError:
        pass

    if not getattr(sys, "frozen", False):
        return

    script = os.path.join(tempfile.gettempdir(), "mediapad_uninstall.bat")

    with open(script, "w", encoding="ascii") as file:
        file.write("@echo off\r\n"
                   ":retry\r\n"
                   f'del /f /q "{sys.executable}" 2>nul\r\n'
                   f'if exist "{sys.executable}" (\r\n'
                   "  ping -n 2 127.0.0.1 >nul\r\n"
                   "  goto retry\r\n"
                   ")\r\n"
                   'del /f /q "%~f0"\r\n')

    shell32.ShellExecuteW(None, "open", script, None, None, SW_HIDE)


class Companion:
    def __init__(self):
        self.stop = threading.Event()
        self.sending = setting("Sending", True)
        self.notify = setting("Notify", False)
        self.connected = False
        self.reader = None
        self.rejected = False
        self.playing = "Nothing playing"
        self.icon = pystray.Icon(APP_NAME, self.image(), DISPLAY_NAME, menu=pystray.Menu(
            pystray.MenuItem(lambda item: self.playing, None, enabled=False),
            pystray.MenuItem(lambda item: "Keyboard: connected" if self.connected else "Keyboard: not found", None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Send now playing", self.toggle_sending, checked=lambda item: self.sending),
            pystray.MenuItem("Notify when the keyboard connects", self.toggle_notify, checked=lambda item: self.notify),
            pystray.MenuItem("Run at startup", self.toggle_startup, checked=lambda item: startup_enabled()),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("MediaPad on GitHub", lambda icon, item: webbrowser.open(PROJECT_URL)),
            pystray.MenuItem("Uninstall", self.ask_uninstall),
            pystray.MenuItem("Quit", self.quit)))

    def image(self):
        color = GREY if not self.connected else GREEN if self.sending else AMBER
        image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)

        draw.rounded_rectangle((6, 6, 58, 58), radius=14, fill=DARK, outline=color, width=4)

        if self.connected and not self.sending:
            draw.rectangle((23, 22, 29, 42), fill=color)
            draw.rectangle((35, 22, 41, 42), fill=color)
        else:
            draw.polygon([(26, 22), (26, 42), (44, 32)], fill=color)

        return image

    def refresh(self):
        try:
            self.icon.icon = self.image()
            self.icon.title = (f"{DISPLAY_NAME}\n{self.playing}" if self.connected and self.sending
                               else f"{DISPLAY_NAME}\nPaused" if self.connected
                               else f"{DISPLAY_NAME}\nKeyboard not found")[:127]
            self.icon.update_menu()
        except Exception:
            pass

    def toggle_sending(self, icon, item):
        self.sending = not self.sending

        save_setting("Sending", self.sending)
        self.refresh()

    def toggle_notify(self, icon, item):
        self.notify = not self.notify

        save_setting("Notify", self.notify)

    def toggle_startup(self, icon, item):
        set_startup(not startup_enabled())

    def ask_uninstall(self, icon, item):
        threading.Thread(target=self.confirm_uninstall, daemon=True).start()

    def confirm_uninstall(self):
        frozen = getattr(sys, "frozen", False)
        front = MB_SETFOREGROUND | MB_TOPMOST

        answer = user32.MessageBoxW(None,
                                    "Uninstall Mediapad Companion?\n\n"
                                    "•  Run at startup will be turned off.\n"
                                    "•  Its settings will be removed.\n"
                                    f"•  {'This program file will delete itself.' if frozen else 'Running from source, so the .py file stays where it is.'}\n\n"
                                    "The keyboard keeps working, only the now playing\n"
                                    "screen on the OLED stops.",
                                    DISPLAY_NAME, MB_YESNO | MB_ICONQUESTION | front)

        if answer != IDYES:
            return

        uninstall()

        if not frozen:
            user32.MessageBoxW(None, "Startup entry and settings removed.\n\nYou can delete mediapad_tray.py now.",
                               DISPLAY_NAME, MB_ICONINFORMATION | front)

        self.quit(self.icon, None)

    def quit(self, icon, item):
        self.stop.set()
        icon.stop()

    def close_reader(self):
        if self.reader is None:
            return

        kernel32.CancelIoEx(self.reader, None)
        kernel32.CloseHandle(self.reader)

        self.reader = None

    def read_commands(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        while not self.stop.is_set():
            handle = self.reader

            if handle is None:
                self.stop.wait(0.5)
                continue

            report = ctypes.create_string_buffer(REPORT_LEN + 1)
            received = wintypes.DWORD(0)

            if not kernel32.ReadFile(handle, report, REPORT_LEN + 1, ctypes.byref(received), None) or received.value < 2:
                self.stop.wait(0.2)
                continue

            data = report.raw[1:received.value]

            if len(data) < 2 or data[0] != PROTO_COMMAND:
                continue

            try:
                self.rejected = not loop.run_until_complete(apply_command(data[1])) or self.rejected
            except Exception:
                self.rejected = True

    def send_now_playing(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        handle = None
        shown = None

        while not self.stop.is_set():
            if handle is None:
                handle, path = find_keyboard()
                self.connected = handle is not None

                if handle is not None and self.reader is None:
                    self.reader = open_reader(path)

                if handle is not None and self.notify:
                    try:
                        self.icon.notify("The keyboard is connected.", DISPLAY_NAME)
                    except Exception:
                        pass

            if handle is not None and self.sending:
                try:
                    now = loop.run_until_complete(read_now_playing())
                except Exception:
                    now = None

                rejected, self.rejected = self.rejected, False
                playing, shuffle, repeat, position, duration, title, artist = now or (False, False, 0, 0, 0, "", "")

                self.playing = (f"{title} - {artist}" if artist else title) or "Nothing playing"

                if not write_report(handle, build_packet(playing, shuffle, repeat, position, duration, title, artist, rejected)):
                    kernel32.CloseHandle(handle)

                    handle = None
                    self.connected = False

                    self.close_reader()

            if (self.connected, self.sending, self.playing) != shown:
                shown = (self.connected, self.sending, self.playing)

                self.refresh()

            self.stop.wait(POLL_INTERVAL)

        if handle is not None:
            kernel32.CloseHandle(handle)

        self.close_reader()

    def run(self):
        threading.Thread(target=self.send_now_playing, daemon=True).start()
        threading.Thread(target=self.read_commands, daemon=True).start()

        self.icon.run()


if __name__ == "__main__":
    if already_running():
        sys.exit(0)

    Companion().run()
