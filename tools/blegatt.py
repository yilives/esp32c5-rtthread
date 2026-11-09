"""Enumerate the C5's GATT database exactly as the PC sees it.

Written after the first version of this test assumed its own idea of the UUIDs and
failed with "Characteristic ... not found": the service list printed empty, which
means the trouble was in discovery, not in the read. Enumerating what is actually
there is the only way to tell a wrong UUID byte order from a service that was
never registered.

Usage: blegatt.py [name-filter] [timeout]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner


async def main() -> int:
    name_filter = sys.argv[1] if len(sys.argv) > 1 else "c5-rtt"
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

    print("scanning for %r ..." % name_filter)
    target = None
    deadline = asyncio.get_event_loop().time() + timeout
    while asyncio.get_event_loop().time() < deadline:
        devices = await BleakScanner.discover(timeout=5.0, return_adv=True)
        for addr, (dev, adv) in devices.items():
            if name_filter in (dev.name or ""):
                target = dev
                print("found %s rssi=%d name=%r" % (addr, adv.rssi, dev.name))
                break
        if target:
            break

    if target is None:
        print("RESULT: not found")
        return 1

    notes = []

    def on_notify(_chr, data):
        notes.append(int.from_bytes(data, "little"))

    async with BleakClient(target, timeout=25.0) as client:
        print("connected:", client.is_connected)

        print("--- services as discovered ---")
        chars = []
        for svc in client.services:
            print("svc %s" % svc.uuid)
            for ch in svc.characteristics:
                print("    chr %s props=%s" % (ch.uuid, ch.properties))
                chars.append(ch.uuid)
        print("--- %d characteristic(s) ---" % len(chars))

        if not chars:
            print("RESULT: FAIL (no characteristics discovered)")
            return 1

        # Find the read-only identity characteristic by property, not by name:
        # whatever UUIDs ended up on air, "readable, 4 bytes, starts with C5RT"
        # identifies it.
        ident = None
        for ch in chars:
            try:
                v = await client.read_gatt_char(ch)
            except Exception as exc:          # noqa: BLE001
                print("read %s -> %s" % (ch, type(exc).__name__))
                continue
            print("read %s = %r" % (ch, v))
            if v.startswith(b"C5RT"):
                ident = ch

        if ident is None:
            print("RESULT: FAIL (no identity characteristic returned C5RT)")
            return 1
        print("identity characteristic: %s" % ident)

        # Subscribe to OUR counter characteristic only.
        #
        # Subscribing to every notifiable characteristic fails, and the failure is
        # interesting rather than incidental: the first attempt included the
        # mandatory GATT Service Changed characteristic (0x2A05), whose CCCD is
        # indicate-only, and the write came back "Access Denied". The board log
        # showed it arriving as `subscribe attr=8 notify=0` - i.e. the
        # controller and the host accepted the ATT write and reported it
        # correctly; the peer (Windows) had rejected the descriptor itself.
        notifiable = []
        for svc in client.services:
            for ch in svc.characteristics:
                if str(ch.uuid).startswith("8ec90003"):
                    notifiable.append(ch.uuid)

        if not notifiable:
            print("RESULT: FAIL (counter characteristic not found)")
            return 1

        print("subscribing to: %s" % notifiable)
        for ch in notifiable:
            await client.start_notify(ch, on_notify)

        print("collecting for 6 s ...")
        await asyncio.sleep(6.0)

        for ch in notifiable:
            await client.stop_notify(ch)

    print("notifications: %d %s" % (len(notes), notes[:10]))

    ok = len(notes) >= 2
    print("RESULT:", "PASS" if ok else "FAIL", "(notifications=%d)" % len(notes))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
