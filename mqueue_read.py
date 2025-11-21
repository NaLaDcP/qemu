#!/usr/bin/env python3
import posix_ipc
import struct
from datetime import datetime
# Structure C : 4 octets = magick[0], magick[1], pin, state
FMT = "BBII"   # 4 unsigned bytes

def main():
    try:
        rx = posix_ipc.MessageQueue(
            "/from_qemu",
            flags=0,
            read=True,
            write=False
        )
        print("Ouvert /from_qemu en lecture.")
    except Exception as e:
        print("Erreur ouverture /from_qemu :", e)
        return

    while True:
        # # --- Réception ---
        # for i in range(0,32):
        data, prio = rx.receive()
       # print(f"Message reçu brut :", list(data))

        # Décodage identique à C
        m0, m1, pin_r, state_r = struct.unpack(FMT, data)
        now = datetime.now()
        print(f"[{now.hour:02d}:{now.minute:02d}:{now.second:02d}.{int(now.microsecond/1000):03d}]: Décodé : magick=({hex(m0)}, {hex(m1)}), pin={pin_r:08x}, state={state_r:08x}")

if __name__ == "__main__":
    main()
