#!/usr/bin/env python3
import posix_ipc
import struct
# Structure C : 4 octets = magick[0], magick[1], pin, state
FMT = "HII"   # 4 unsigned bytes

def main():
    # --- Ouvre la queue d'envoi (/tx_queue) ---
    try:
        tx = posix_ipc.MessageQueue(
            "/to_qemu",
            flags=0,
            read=False,
            write=True
        )
        print("Ouvert /to_qemu en écriture.")
    except Exception as e:
        print("Erreur ouverture /to_qemu :", e)
        return

    # --- Construction du message gpio_msg ---
    # Exemple : magick = {0xAA, 0x55}, pin = 3, state = 1
    magic = 0xABCD
    pin = 0 
    state = 1

    msg = struct.pack(FMT, magic, pin, state)

    print("Message préparé :", list(msg))

    # --- Envoi ---
    tx.send(msg)
    print("Message envoyé.")


if __name__ == "__main__":
    main()
