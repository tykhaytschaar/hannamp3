#!/usr/bin/env python3
"""Interaktív TUI a hannamp3 soros vezérléséhez.

Bekéri a parancsot (és ha kell, paramétert), majd elküldi
#parancs$param# formátumban a soros porton.

Használat:
    tools/mp3ctl.py                         # auto-detektálja a portot
    tools/mp3ctl.py /dev/cu.usbmodemXXXX    # explicit port
    tools/mp3ctl.py --baud 115200 PORT
"""
import sys
import glob
import argparse

try:
    import serial
except ImportError:
    sys.exit("pyserial kell: pip3 install pyserial  (vagy a system framework python3)")


COMMANDS = {
    "play":  None,
    "pause": None,
    "stop":  None,
    "next":  None,
    "prev":  None,
    "menu":  None,
    "vol":   "up / down / max / off / 0-100",
}


def find_port():
    cands = glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.usbmodem*")
    cands = [c for c in cands if "Bluetooth" not in c and "debug" not in c]
    return cands[0] if cands else None


def send(ser, cmd, param=""):
    msg = f"#{cmd}${param}#" if param else f"#{cmd}#"
    ser.write(msg.encode())
    print(f"  → elküldve: {msg}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="soros port (auto, ha üres)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("Nem találtam soros portot. Add meg explicit: mp3ctl.py /dev/cu.usbserial-XXXX")

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = args.baud
    ser.dtr = False     # ne resetelje a panelt csatlakozáskor
    ser.rts = False
    ser.open()
    print(f"Csatlakozva: {port} @ {args.baud}")
    print("Parancsok: " + ", ".join(COMMANDS) + "   (q = kilépés)\n")

    try:
        while True:
            cmd = input("parancs> ").strip().lower()
            if cmd in ("q", "quit", "exit"):
                break
            if not cmd:
                continue
            if cmd not in COMMANDS:
                print(f"  ismeretlen: '{cmd}'. Választható: {', '.join(COMMANDS)}")
                continue
            param = ""
            if COMMANDS[cmd]:
                param = input(f"  {cmd} param ({COMMANDS[cmd]})> ").strip().lower()
            send(ser, cmd, param)
    except (EOFError, KeyboardInterrupt):
        print()
    finally:
        ser.close()
        print("Bezárva.")


if __name__ == "__main__":
    main()
