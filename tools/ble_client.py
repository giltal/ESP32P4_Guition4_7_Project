"""Talk to the Gution P4 over BLE from a PC.

Windows Settings cannot pair a plain GATT peripheral — its "Add device" flow
expects a recognised profile (HID, audio, ...) and answers "Try connecting your
device again" for anything else. That is a limitation of the pairing UI, not of
the device: a GATT client connects directly, no pairing involved. This is that
client.

    python ble_client.py                 scan, connect, read state, run a UART exchange
    python ble_client.py --monitor       stay connected and print notifications
    python ble_client.py --send "mode 2" send one console command and print the reply

Requires:  pip install bleak
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "gution-p4"

# Control service — one characteristic per field.
CTRL_SVC = "4f3a0001-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_MODE = "4f3a0002-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LEVEL = "4f3a0003-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LED = "4f3a0004-9d2b-4c7e-a1f5-8b6d3e0c7a21"
CTRL_LABEL = "4f3a0005-9d2b-4c7e-a1f5-8b6d3e0c7a21"

# Nordic UART Service. Names are from the central's point of view, so the
# device receives on RX and sends on TX.
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

MODES = {0: "Idle", 1: "Run", 2: "Test"}


async def find_device(timeout=10.0):
    print(f"Scanning for {DEVICE_NAME!r} ...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
    if device is None:
        print(f"Not found. Is it advertising? Check the BLE tab on the screen.")
        return None
    print(f"Found {device.name} at {device.address}")
    return device


async def read_state(client):
    mode = (await client.read_gatt_char(CTRL_MODE))[0]
    level = (await client.read_gatt_char(CTRL_LEVEL))[0]
    led = (await client.read_gatt_char(CTRL_LED))[0]
    label = (await client.read_gatt_char(CTRL_LABEL)).decode("utf-8", "replace")

    print("\ncontrol state")
    print(f"  mode    {mode} ({MODES.get(mode, '?')})")
    print(f"  level   {level}")
    print(f"  led     {'on' if led else 'off'}")
    print(f"  message {label!r}")


async def uart_exchange(client, command, wait=1.5):
    """Send one console line and collect whatever comes back."""
    chunks = []

    def on_tx(_handle, data: bytearray):
        chunks.append(bytes(data))

    await client.start_notify(NUS_TX, on_tx)
    # The console buffers until a newline, so the terminator matters.
    await client.write_gatt_char(NUS_RX, (command + "\r\n").encode(), response=False)
    await asyncio.sleep(wait)
    await client.stop_notify(NUS_TX)

    return b"".join(chunks).decode("utf-8", "replace")


async def monitor(client):
    print("\nNotifications (Ctrl+C to stop)")

    def on_ctrl(name):
        def cb(_handle, data: bytearray):
            if name == "message":
                print(f"  {name}: {data.decode('utf-8', 'replace')!r}")
            else:
                print(f"  {name}: {data[0]}")
        return cb

    def on_tx(_handle, data: bytearray):
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()

    for uuid, name in ((CTRL_MODE, "mode"), (CTRL_LEVEL, "level"),
                       (CTRL_LED, "led"), (CTRL_LABEL, "message")):
        await client.start_notify(uuid, on_ctrl(name))
    await client.start_notify(NUS_TX, on_tx)

    while True:
        await asyncio.sleep(1)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor", action="store_true",
                    help="stay connected and print notifications")
    ap.add_argument("--send", metavar="CMD", action="append",
                    help="console command to send; repeat for several, all over "
                         "one connection")
    args = ap.parse_args()

    device = await find_device()
    if device is None:
        return 1

    # Windows serves a cached GATT database, which can be stale or partial if
    # an earlier connection was cut short during discovery — the symptom is
    # "characteristic ... was not found" for a service that is really there.
    # Backend options go in a `winrt` dict; a bare keyword is silently ignored.
    kwargs = {"winrt": {"use_cached_services": False}} \
        if sys.platform == "win32" else {}

    async with BleakClient(device, **kwargs) as client:
        print("Connected.")

        if args.send:
            for cmd in args.send:
                print(f"> {cmd}")
                print(await uart_exchange(client, cmd), end="")
            return 0

        await read_state(client)

        print("\nUART console:")
        print(await uart_exchange(client, "status"))

        if args.monitor:
            await monitor(client)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        pass
