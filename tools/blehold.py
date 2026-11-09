"""Hold a BLE connection to the C5 for N seconds and count notifications.

The previous test connected, read, subscribed, collected 5 s and closed. That
proves the paths exist; it does not prove the link survives, and the board's own
log showed the peer dropping it (`reason=531`, HCI 0x13 "Remote User Terminated
Connection") at the end of each run - which was Windows' GATT discovery closing,
not something the board did.

This script deliberately keeps the connection open and silent for the whole
window, so a drop is attributable: if the board re-advertises mid-run, the link
was lost, and tools/c5seq output will show it.

Usage: blehold.py [seconds] [name-filter]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

CHR_CNT = "8ec90003-f315-4f60-9fb8-838830daea50"


async def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    name_filter = sys.argv[2] if len(sys.argv) > 2 else "c5-rtt"

    target = None
    deadline = asyncio.get_event_loop().time() + 25
    while asyncio.get_event_loop().time() < deadline and target is None:
        for _addr, (dev, adv) in (await BleakScanner.discover(timeout=5.0, return_adv=True)).items():
            if name_filter in (dev.name or ""):
                target = dev
                print("found %s rssi=%d" % (dev.address, adv.rssi))
                break

    if target is None:
        print("RESULT: not found")
        return 1

    notes = []
    disconnected = []

    def on_notify(_chr, data):
        notes.append(int.from_bytes(data, "little"))

    def on_disconnect(_client):
        disconnected.append(1)

    # `still` and `drops` are sampled INSIDE the block on purpose. The
    # disconnected_callback also fires when this script closes the client itself,
    # so reading the counters after the `with` block counts our own teardown as a
    # failure - which is exactly how the first version of this script reported
    # FAIL on a run that had held the link and received every notification.
    still = False
    drops = 0

    async with BleakClient(target, disconnected_callback=on_disconnect, timeout=25.0) as client:
        print("connected:", client.is_connected)
        await client.start_notify(CHR_CNT, on_notify)
        print("holding for %d s ..." % int(seconds))

        for i in range(int(seconds)):
            await asyncio.sleep(1.0)
            if not client.is_connected:
                print("link lost at %d s" % i)
                break
            if i % 10 == 9:
                print("  %2d s: connected=%s notifications=%d last=%s"
                      % (i + 1, client.is_connected, len(notes),
                         notes[-1] if notes else None))

        still = client.is_connected
        drops = len(disconnected)
        if still:
            await client.stop_notify(CHR_CNT)

    expected = int(seconds * 0.8)      # the board notifies once per second
    ok = still and len(notes) >= expected and drops == 0
    print("notifications=%d expected>=%d still_connected=%s drops_during_run=%d"
          % (len(notes), expected, still, drops))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
