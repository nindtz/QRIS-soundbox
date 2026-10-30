#!/usr/bin/env python3
"""
Send test payments to the QRIS soundbox over BLE (works on macOS, Linux, Windows).

Setup:     pip3 install bleak
Usage:     python3 ble_test.py 25000                # send one payment
           python3 ble_test.py 25000 1250000 1000   # send several, 3 s apart
           python3 ble_test.py                      # interactive: type amounts, Enter to send
           python3 ble_test.py --scan               # list nearby BLE devices (debugging)

macOS notes:
 * First run: allow your terminal app under System Settings > Privacy & Security > Bluetooth.
 * On first connect macOS shows a pairing dialog: type the passkey printed on the ESP32's
   Serial Monitor (default 482913). You have 60 s. Later runs reconnect without it.
 * macOS shows BLE devices by UUID, not MAC address; this script finds the box by its
   service UUID or its name, so you don't need the address.
"""
import argparse
import asyncio
import secrets

from bleak import BleakClient, BleakScanner

NAME = "QRIS-Soundbox"
SERVICE_UUID = "6f1c0001-5b7a-4c1e-9d3a-2a4f7c9b0001"
CHAR_UUID = "6f1c0002-5b7a-4c1e-9d3a-2a4f7c9b0001"


def is_soundbox(device, adv):
    uuids = [u.lower() for u in (adv.service_uuids or [])]
    return SERVICE_UUID in uuids or device.name == NAME or adv.local_name == NAME


async def scan_only():
    print("Scanning 8 s...")
    found = await BleakScanner.discover(timeout=8, return_adv=True)
    for dev, adv in sorted(found.values(), key=lambda x: -x[1].rssi):
        mark = "  <-- soundbox" if is_soundbox(dev, adv) else ""
        print(f"{adv.rssi:4d} dBm  {dev.name or adv.local_name or '(no name)':24s} {dev.address}{mark}")


async def send(client, amount):
    payload = f"{secrets.token_hex(4)};{amount}"       # unique id every time (device dedupes ids)
    for attempt in range(1, 21):                         # ~60 s: time to type the passkey
        try:
            await client.write_gatt_char(CHAR_UUID, payload.encode(), response=True)
            print(f"sent  {payload}")
            return
        except Exception as e:
            if attempt == 1:
                print("Not paired yet. Watch for a macOS pairing dialog and enter the passkey "
                      "(default 482913). Waiting up to 60 s...")
            print(f"  attempt {attempt}: {str(e).splitlines()[0][:90]}")
            await asyncio.sleep(3)
    print("Gave up: still not authenticated. See the troubleshooting notes.")


async def main(amounts):
    print("Looking for the soundbox...")
    dev = await BleakScanner.find_device_by_filter(is_soundbox, timeout=15)
    if not dev:
        raise SystemExit("Soundbox not found. Is it powered and advertising? Try --scan, "
                         "and check Terminal's Bluetooth permission.")
    print(f"Found {dev.name or NAME} ({dev.address}). Connecting (pair if asked)...")

    async with BleakClient(dev, timeout=60) as client:
        print("Connected.")
        if amounts:
            for i, a in enumerate(amounts):
                if i:
                    await asyncio.sleep(3)               # let the previous announcement finish
                await send(client, a)
        else:
            loop = asyncio.get_running_loop()
            print("Type an amount (e.g. 25000) and press Enter. Empty line quits.")
            while True:
                line = (await loop.run_in_executor(None, input, "amount> ")).strip()
                if not line:
                    break
                if not line.isdigit():
                    print("digits only")
                    continue
                await send(client, int(line))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Send test payments to the QRIS soundbox")
    ap.add_argument("amounts", nargs="*", type=int)
    ap.add_argument("--scan", action="store_true", help="list nearby BLE devices and exit")
    args = ap.parse_args()
    asyncio.run(scan_only() if args.scan else main(args.amounts))