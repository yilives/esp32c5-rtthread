"""Scan for BLE advertisements on the PC's own adapter.

This is the external receiver for the C5's BLE test. The C5 cannot hear its own
advertisements - measured: adv_start succeeded and a 9 s scan heard 769 other
advertisements and none of its own address - so "is it really transmitting?"
has to be answered by a second radio. The ESP32-S3 board would be the natural
choice, but its RT-Thread overlay in `rt-thread/libcpu/xtensa` is mid-edit and
no longer assembles (`unknown opcode or format name 'xt_set_current'`), so the
host adapter is used instead.

Usage: blescan.py [seconds] [target-address]
"""
import asyncio
import sys

from bleak import BleakScanner


async def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    target = sys.argv[2].lower() if len(sys.argv) > 2 else None

    seen = {}
    found = []

    def on_detect(device, advertisement_data):
        seen[device.address] = (device.name, advertisement_data.rssi)
        name = device.name or ""
        # The peripheral advertises "c5-rtt"; match on either the name or an
        # address prefix, so a rename does not silently turn this into a pass.
        if target and device.address.lower().startswith(target):
            found.append(device.address)
            print("*** MATCH %s rssi=%d name=%s" % (device.address, advertisement_data.rssi, name))
        elif "c5-rtt" in name:
            found.append(device.address)
            print("*** C5 NAME %s rssi=%d" % (device.address, advertisement_data.rssi))

    scanner = BleakScanner(detect_callback=on_detect)
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()

    print("--- %d device(s) in %.0f s ---" % (len(seen), seconds))
    for addr, (name, rssi) in sorted(seen.items()):
        print("  %s rssi=%d name=%s" % (addr, rssi, name or ""))

    if found:
        print("RESULT: MATCH")
        return 0
    print("RESULT: no match")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
