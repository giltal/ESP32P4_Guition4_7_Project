"""Gution P4 — BLE terminal and control panel.

A desktop client for the board's Nordic UART Service and control
characteristics. Windows cannot bond a generic GATT peripheral, so tools that
insist on a paired device will not work with it; this one connects unbonded,
the same way the phone apps do.

Run it directly:

    pip install bleak
    python gution_terminal.py

Or build a standalone executable (no Python needed on the target machine):

    pip install pyinstaller
    pyinstaller --onefile --windowed --name GutionTerminal gution_terminal.py

A windowed executable has nowhere to print a traceback, so unhandled errors are
appended to GutionTerminal.log next to the program and echoed into the console
pane instead of killing the window.
"""
import asyncio
import os
import queue
import sys
import threading
import traceback
import tkinter as tk
from datetime import datetime
from tkinter import ttk, scrolledtext
from typing import Optional

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "gution-p4"

CTRL_MODE = "4f3a0002-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LEVEL = "4f3a0003-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LED = "4f3a0004-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LABEL = "4f3a0005-9d2b-4c7e-a1f5-8b6d3e0c7a21"

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

MODES = ["Idle", "Run", "Test"]

# Breathing room after each GATT operation. Issuing them truly back to back
# congests the link and Windows eventually cancels one, taking the connection
# down. At this spacing a human typing never notices, while a script firing
# dozens of operations still gets through intact.
OP_GAP_S = 0.05


def _log_path() -> str:
    """Beside the executable when frozen, beside the script otherwise."""
    base = os.path.dirname(sys.executable if getattr(sys, "frozen", False)
                           else os.path.abspath(__file__))
    return os.path.join(base, "GutionTerminal.log")


_errors: "queue.Queue[str]" = queue.Queue()


def note(text: str) -> None:
    """Append a plain line to the log.

    Also proves the log is writable: if a crash leaves no traceback, the
    presence of the startup line still distinguishes "died before logging
    could work" from "died somewhere Python could not catch".
    """
    try:
        with open(_log_path(), "a", encoding="utf-8") as fh:
            fh.write(f"[{datetime.now():%Y-%m-%d %H:%M:%S}] {text}\n")
    except OSError:
        pass


def record(where: str, exc: BaseException) -> None:
    """Log an unhandled error and hand it to the GUI if one is running."""
    text = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    entry = f"\n[{datetime.now():%Y-%m-%d %H:%M:%S}] {where}\n{text}"
    try:
        with open(_log_path(), "a", encoding="utf-8") as fh:
            fh.write(entry)
    except OSError:
        pass
    _errors.put(entry)


class BleWorker(threading.Thread):
    """Owns the asyncio loop and the BLE connection.

    bleak is async and tkinter is not, so the two cannot share a thread. The
    loop runs here; the GUI hands work in through `submit()` and receives
    everything back as messages on `out`, which it drains from its own timer.
    Nothing here touches a widget.
    """

    def __init__(self, out: queue.Queue):
        super().__init__(daemon=True)
        self.out = out
        self.loop = asyncio.new_event_loop()
        self.client: Optional[BleakClient] = None
        self.ready = threading.Event()
        self._lock: Optional[asyncio.Lock] = None
        self._stop = False

    # -- called from the GUI thread ------------------------------------
    def submit(self, coro):
        """Schedule work on the BLE loop, waiting for it to come up first."""
        if not self.ready.wait(timeout=5.0) or not self.loop.is_running():
            # Close it explicitly: an un-awaited coroutine raises a
            # RuntimeWarning and leaks its frame.
            coro.close()
            self.out.put(("log", "BLE thread is not running.\n"))
            return
        asyncio.run_coroutine_threadsafe(coro, self.loop)

    def shutdown(self):
        self._stop = True
        if self.loop.is_running():
            self.submit(self._disconnect())

    # -- loop ----------------------------------------------------------
    def run(self):
        asyncio.set_event_loop(self.loop)
        self._lock = asyncio.Lock()
        self.loop.set_exception_handler(self._loop_error)
        self.ready.set()
        try:
            self.loop.run_forever()
        except BaseException as e:            # noqa: BLE001 - last line of defence
            record("BLE loop", e)

    def _loop_error(self, _loop, context):
        exc = context.get("exception")
        if exc is not None:
            record("BLE task", exc)
        else:
            self._post("log", f"BLE: {context.get('message')}\n")

    def _post(self, kind, value=None):
        self.out.put((kind, value))

    # -- connection ----------------------------------------------------
    async def _disconnect(self):
        if self.client is not None:
            try:
                await self.client.disconnect()
            except Exception as e:
                record("disconnect", e)
        self.client = None
        self._post("status", "disconnected")
        if self._stop:
            self.loop.stop()

    async def connect(self):
        if self.client is not None:
            return

        self._post("status", "scanning")
        self._post("log", f"Scanning for {DEVICE_NAME!r} ...\n")

        try:
            device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
        except Exception as e:
            record("scan", e)
            self._post("log", f"Scan failed: {e}\n")
            self._post("status", "disconnected")
            return

        if device is None:
            self._post("log", "Not found. Is it advertising?\n")
            self._post("status", "disconnected")
            return

        self._post("log", f"Found {device.address}, connecting ...\n")
        try:
            # Windows caches a device's GATT database and will happily serve a
            # stale copy — including a partial one recorded when an earlier
            # connection was cut short mid-discovery. That shows up as
            # "characteristic ... was not found" for a service that plainly
            # exists, so always rediscover.
            # Note the nesting: bleak takes backend options in a `winrt` dict.
            # Passed as a bare keyword it is silently ignored, and the stale
            # cache is used anyway.
            kwargs = {"winrt": {"use_cached_services": False}} \
                if sys.platform == "win32" else {}

            client = BleakClient(device, disconnected_callback=self._on_dropped,
                                 **kwargs)
            await client.connect()

            await client.start_notify(NUS_TX, self._on_tx)
            for uuid, name in ((CTRL_MODE, "mode"), (CTRL_LEVEL, "level"),
                               (CTRL_LED, "led"), (CTRL_LABEL, "label")):
                await client.start_notify(uuid, self._make_ctrl_cb(name))
        except Exception as e:
            record("connect", e)
            self._post("log", f"Connect failed: {e}\n")
            self._post("status", "disconnected")
            self.client = None
            return

        self.client = client
        self._post("status", "connected")
        self._post("log", "Connected.\n\n")
        await self.refresh()

    def _on_dropped(self, _client):
        """bleak calls this from its own thread when the link drops."""
        self.client = None
        self._post("log", "\n** disconnected **\n")
        self._post("status", "disconnected")

    def _on_tx(self, _handle, data: bytearray):
        self._post("log", data.decode("utf-8", "replace"))

    def _make_ctrl_cb(self, name):
        def cb(_handle, data: bytearray):
            if name == "label":
                self._post("ctrl", (name, data.decode("utf-8", "replace")))
            elif data:
                self._post("ctrl", (name, data[0]))
        return cb

    # -- operations ----------------------------------------------------
    #
    # Every one of these takes the lock. Sending quickly used to overlap
    # several GATT operations on one client, which the Windows backend does not
    # tolerate — it surfaced as the connection dying after a few sends.
    #
    async def refresh(self):
        async with self._lock:
            client = self.client
            if client is None:
                return
            try:
                for uuid, name in ((CTRL_MODE, "mode"), (CTRL_LEVEL, "level"),
                                   (CTRL_LED, "led")):
                    data = await client.read_gatt_char(uuid)
                    self._post("ctrl", (name, data[0] if data else 0))
                label = await client.read_gatt_char(CTRL_LABEL)
                self._post("ctrl", ("label", label.decode("utf-8", "replace")))
            except Exception as e:
                record("refresh", e)
                self._post("log", f"Read failed: {e}\n")

    async def send_line(self, text: str):
        async with self._lock:
            client = self.client
            if client is None:
                self._post("log", "Not connected.\n")
                return
            try:
                # The console buffers until a newline, so the terminator matters.
                await client.write_gatt_char(NUS_RX, (text + "\r\n").encode(),
                                             response=False)
            except Exception as e:
                record("send", e)
                self._post("log", f"Send failed: {e}\n")
            await asyncio.sleep(OP_GAP_S)

    async def write_char(self, uuid, payload: bytes):
        async with self._lock:
            client = self.client
            if client is None:
                return
            try:
                await client.write_gatt_char(uuid, payload, response=True)
            except Exception as e:
                record("write", e)
                self._post("log", f"Write failed: {e}\n")
            await asyncio.sleep(OP_GAP_S)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Gution P4 — BLE Terminal")
        self.geometry("720x640")
        self.minsize(560, 480)

        self.q: queue.Queue = queue.Queue()
        self.ble = BleWorker(self.q)
        self.ble.start()

        self._suppress = False    # stop widget updates echoing back as writes
        self._closing = False
        self._build()

        self.after(50, self._drain)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # -- layout --------------------------------------------------------
    def _build(self):
        pad = {"padx": 8, "pady": 4}

        bar = ttk.Frame(self)
        bar.pack(fill="x", **pad)

        self.btn_connect = ttk.Button(bar, text="Connect", command=self._connect)
        self.btn_connect.pack(side="left")

        self.lbl_status = ttk.Label(bar, text="disconnected", foreground="#999")
        self.lbl_status.pack(side="left", padx=10)

        ttk.Button(bar, text="Refresh", command=self._refresh).pack(side="right")
        ttk.Button(bar, text="Clear", command=self._clear).pack(side="right", padx=6)

        ctrl = ttk.LabelFrame(self, text="Control state")
        ctrl.pack(fill="x", **pad)

        ttk.Label(ctrl, text="Mode").grid(row=0, column=0, sticky="w", padx=8, pady=6)
        self.var_mode = tk.StringVar(value=MODES[0])
        self.cmb_mode = ttk.Combobox(ctrl, textvariable=self.var_mode, values=MODES,
                                     state="readonly", width=10)
        self.cmb_mode.grid(row=0, column=1, sticky="w")
        self.cmb_mode.bind("<<ComboboxSelected>>", self._mode_changed)

        ttk.Label(ctrl, text="Level").grid(row=0, column=2, sticky="w", padx=(20, 8))
        self.var_level = tk.IntVar(value=50)
        self.scl_level = ttk.Scale(ctrl, from_=0, to=100, variable=self.var_level,
                                   command=self._level_moved, length=180)
        self.scl_level.grid(row=0, column=3, sticky="w")
        # Commit on release, not on every pixel of the drag: one write per
        # gesture instead of a hundred.
        self.scl_level.bind("<ButtonRelease-1>", self._level_committed)
        self.lbl_level = ttk.Label(ctrl, text="50", width=4)
        self.lbl_level.grid(row=0, column=4, sticky="w")

        self.var_led = tk.BooleanVar(value=False)
        self.chk_led = ttk.Checkbutton(ctrl, text="Indicator", variable=self.var_led,
                                       command=self._led_toggled)
        self.chk_led.grid(row=1, column=0, columnspan=2, sticky="w", padx=8, pady=6)

        ttk.Label(ctrl, text="Message").grid(row=1, column=2, sticky="w", padx=(20, 8))
        self.var_label = tk.StringVar()
        ent = ttk.Entry(ctrl, textvariable=self.var_label, width=28)
        ent.grid(row=1, column=3, columnspan=2, sticky="we")
        ent.bind("<Return>", self._label_committed)

        term = ttk.LabelFrame(self, text="Console")
        term.pack(fill="both", expand=True, **pad)

        self.txt = scrolledtext.ScrolledText(term, wrap="word", height=14,
                                             font=("Consolas", 10))
        self.txt.pack(fill="both", expand=True, padx=6, pady=6)
        self.txt.configure(state="disabled")

        entry_row = ttk.Frame(term)
        entry_row.pack(fill="x", padx=6, pady=(0, 6))

        self.var_cmd = tk.StringVar()
        self.ent_cmd = ttk.Entry(entry_row, textvariable=self.var_cmd)
        self.ent_cmd.pack(side="left", fill="x", expand=True)
        self.ent_cmd.bind("<Return>", self._send)

        ttk.Button(entry_row, text="Send", command=self._send).pack(side="left", padx=6)

        ttk.Label(self, foreground="#888",
                  text="Commands: status  mode <0|1|2>  level <0-100>  "
                       "led <on|off>  msg <text>  help"
                  ).pack(fill="x", padx=8, pady=(0, 8))

    # -- actions -------------------------------------------------------
    def _connect(self):
        self.ble.submit(self.ble.connect())

    def _refresh(self):
        self.ble.submit(self.ble.refresh())

    def _clear(self):
        self.txt.configure(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.configure(state="disabled")

    def _send(self, _event=None):
        text = self.var_cmd.get().strip()
        if not text:
            return
        self._append(f"> {text}\n")
        self.ble.submit(self.ble.send_line(text))
        self.var_cmd.set("")

    def _mode_changed(self, _event=None):
        if self._suppress:
            return
        self.ble.submit(self.ble.write_char(CTRL_MODE,
                                            bytes([MODES.index(self.var_mode.get())])))

    def _level_moved(self, _value=None):
        self.lbl_level.configure(text=str(int(float(self.var_level.get()))))

    def _level_committed(self, _event=None):
        if self._suppress:
            return
        self.ble.submit(self.ble.write_char(
            CTRL_LEVEL, bytes([max(0, min(100, int(float(self.var_level.get()))))])))

    def _led_toggled(self):
        if self._suppress:
            return
        self.ble.submit(self.ble.write_char(CTRL_LED,
                                            bytes([1 if self.var_led.get() else 0])))

    def _label_committed(self, _event=None):
        if self._suppress:
            return
        self.ble.submit(self.ble.write_char(CTRL_LABEL,
                                            self.var_label.get().encode()[:32]))

    # -- plumbing ------------------------------------------------------
    def _append(self, text):
        # The device ends lines with CRLF, and Tk's Text widget treats a bare
        # CR as its own line break — leaving a blank line after every line.
        text = text.replace("\r\n", "\n").replace("\r", "\n")

        self.txt.configure(state="normal")
        self.txt.insert("end", text)
        self.txt.see("end")
        self.txt.configure(state="disabled")

    def _drain(self):
        """Pump messages from the BLE thread. Only this thread edits widgets."""
        if self._closing:
            return
        try:
            while True:
                kind, value = self.q.get_nowait()
                if kind == "log":
                    self._append(value)
                elif kind == "status":
                    self._set_status(value)
                elif kind == "ctrl":
                    self._set_ctrl(*value)
        except queue.Empty:
            pass

        # Surface background failures rather than losing them to a log nobody
        # thinks to open.
        try:
            while True:
                self._append(_errors.get_nowait())
        except queue.Empty:
            pass

        self.after(50, self._drain)

    def _set_status(self, state):
        colours = {"connected": "#16a34a", "scanning": "#d97706",
                   "disconnected": "#999"}
        self.lbl_status.configure(text=state, foreground=colours.get(state, "#999"))
        self.btn_connect.configure(
            text="Connect" if state == "disconnected" else "Connected",
            state="normal" if state == "disconnected" else "disabled")

    def _set_ctrl(self, name, value):
        # Guard the widget updates: setting them fires the same handlers that
        # write to the device, which would bounce the value straight back.
        self._suppress = True
        try:
            if name == "mode" and value < len(MODES):
                self.var_mode.set(MODES[value])
            elif name == "level":
                self.var_level.set(value)
                self.lbl_level.configure(text=str(value))
            elif name == "led":
                self.var_led.set(bool(value))
            elif name == "label":
                self.var_label.set(value)
        finally:
            self._suppress = False

    def report_callback_exception(self, exc_type, exc, tb):
        """Tk calls this for errors inside widget callbacks, which would
        otherwise take the window down with no message."""
        record("GUI callback", exc)

    def _on_close(self):
        # Stop the timer before destroying, or a queued `after` fires against
        # dead widgets and raises TclError on the way out.
        self._closing = True
        self.ble.shutdown()
        self.destroy()


def main():
    sys.excepthook = lambda t, e, tb: record("main thread", e)
    if hasattr(threading, "excepthook"):
        threading.excepthook = lambda a: record(f"thread {a.thread.name}", a.exc_value)

    note(f"--- started (frozen={getattr(sys, 'frozen', False)}, "
         f"python {sys.version.split()[0]}) ---")
    try:
        App().mainloop()
    except BaseException as e:                # noqa: BLE001
        record("mainloop", e)
        raise
    finally:
        note("--- exited ---")


if __name__ == "__main__":
    main()
