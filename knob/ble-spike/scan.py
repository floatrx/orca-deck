# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak==1.1.1"]
# ///
"""Phase 1 spike: can a bleak scan run (TCC Bluetooth) and see advertisers? Prints one result line."""
import asyncio
import sys
import time

from bleak import BleakScanner


async def main():
    started = time.time()
    try:
        devices = await BleakScanner.discover(timeout=6.0, return_adv=True)
    except Exception as error:
        print(f"{time.strftime('%H:%M:%S')} scan FAILED: {type(error).__name__}: {error}", flush=True)
        sys.exit(1)
    named = sorted({adv.local_name for _, adv in devices.values() if adv.local_name})
    print(f"{time.strftime('%H:%M:%S')} scan ok: {len(devices)} devices, {len(named)} named "
          f"({', '.join(named[:6])}) in {time.time() - started:.1f}s", flush=True)


asyncio.run(main())
