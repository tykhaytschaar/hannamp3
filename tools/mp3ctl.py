#!/usr/bin/env python3
"""
hannamp3 CLI REPL — interaktív TUI a soros porthoz.

Protokoll: ##cmd##  vagy  ##cmd$$payload##

Layout:
    Header  : kapcsolat info
    Bal     : küldött parancsok története
    Jobb    : eszköz kimenete (görgethető curses pad)
    Alul    : cmd + payload prompt

Helyi parancsok (nem mennek az eszköznek):
    cmd='port', payload='close' → soros port lezárása (flasheléshez)
    cmd='port', payload='open'  → port visszanyitása

Promptban:
    'quit' / 'exit' / 'q' vagy ESC → kilépés
    PgUp / PgDn     : kimenet-pane görgetése egy oldal fel/le
    Home            : a backlog elejére
    End             : vissza az élő végéhez (auto-follow)
    Ctrl+P / Ctrl+N : soronként fel/le

Használat:
    tools/mp3ctl.py [PORT] [BAUD] [--log [PATH]]

    PORT nélkül felajánlja az elérhető portokat. Default baud: 115200.
    --log érték nélkül cli_repl-YYYYMMDD_HHMMSS.log a script könyvtárában.

pyserial szükséges:
    pip3 install pyserial
"""

import argparse
import curses
import queue
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial kell — telepítsd: pip3 install pyserial", file=sys.stderr)
    sys.exit(1)


DEFAULT_BAUD = 115200
BACKLOG_ROWS = 10000

ANSI_RE = re.compile(r"\x1b\[[\d;]*m")
CTRL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def reader_loop(ser, out_q, stop_ev):
    """Háttérszál: soros byte-okat olvas a queue-ba."""
    while not stop_ev.is_set():
        try:
            data = ser.read(512)
        except (OSError, serial.SerialException):
            time.sleep(0.2)
            continue
        if data:
            out_q.put(data)


def clean_text(text):
    """ANSI escape-ek, CR és nyomtathatatlan C0 karakterek kiszűrése."""
    return CTRL_RE.sub("", ANSI_RE.sub("", text).replace("\r", ""))


class OutputView:
    """Görgethető jobb pane curses pad-del.

    Két mód:
      follow=True  → view_top automatikusan a végén
      follow=False → user scrollozott vissza, új tartalom csak gyűlik
    End-del visszakapcsol follow-ra.
    """

    def __init__(self, top_y, left_x, visible_h, visible_w):
        self.top_y     = top_y
        self.left_x    = left_x
        self.visible_h = visible_h
        self.visible_w = visible_w
        self.right_x   = left_x + visible_w - 1
        self.bottom_y  = top_y + visible_h - 1

        self.pad = curses.newpad(BACKLOG_ROWS, visible_w + 1)
        self.pad.scrollok(True)

        self.follow   = True
        self.view_top = 0

    def _cursor_row(self):
        return self.pad.getyx()[0]

    def write(self, text):
        clean = clean_text(text)
        try:
            self.pad.addstr(clean)
        except (curses.error, ValueError):
            pass
        if self.follow:
            self._snap_to_tail()

    def _snap_to_tail(self):
        cur = self._cursor_row()
        self.view_top = max(0, cur - self.visible_h + 1)

    def scroll(self, delta):
        cur = self._cursor_row()
        max_top = max(0, cur - self.visible_h + 1)
        new_top = max(0, min(self.view_top + delta, max_top))
        self.view_top = new_top
        self.follow   = (new_top == max_top)

    def scroll_to_top(self):
        self.view_top = 0
        self.follow   = False

    def scroll_to_tail(self):
        self.follow = True
        self._snap_to_tail()

    def refresh(self):
        try:
            self.pad.refresh(self.view_top, 0,
                             self.top_y, self.left_x,
                             self.bottom_y, self.right_x)
        except curses.error:
            pass


def main_tui(stdscr, port_path, baud, log_path):
    curses.curs_set(1)
    stdscr.clear()
    h, w = stdscr.getmaxyx()
    if h < 14 or w < 70:
        raise RuntimeError(f"Terminál túl kicsi ({w}x{h}); minimum 70x14")

    header_h = 1
    prompt_h = 4
    pane_h = h - header_h - prompt_h
    left_w = w // 3
    right_w = w - left_w

    header = curses.newwin(header_h, w, 0, 0)
    left   = curses.newwin(pane_h, left_w, header_h, 0)
    prompt = curses.newwin(prompt_h, w, header_h + pane_h, 0)
    output = OutputView(top_y=header_h, left_x=left_w,
                        visible_h=pane_h, visible_w=right_w)

    left.scrollok(True)

    log_fh = None
    if log_path is not None:
        try:
            log_fh = open(log_path, "a", encoding="utf-8", buffering=1)
            log_fh.write(f"\n--- mp3ctl session start {datetime.now().isoformat(timespec='seconds')} "
                         f"@ {port_path} {baud} bps ---\n")
        except OSError as e:
            raise RuntimeError(f"Log fájl nem nyitható ({log_path}): {e}")

    port_closed = [False]   # 'port close' → True; a header is ezt mutatja

    def render_header():
        header.bkgd(' ', curses.A_REVERSE)
        header.erase()
        tail_marker = "" if output.follow else "  [SCROLLED — End-tel élőre]"
        log_marker  = f"  log→{Path(log_path).name}" if log_path else ""
        port_marker = "  [PORT ZÁRVA — 'port open']" if port_closed[0] else ""
        text = (f" hannamp3 CLI REPL  @ {port_path}  {baud} bps   "
                f"'quit' vagy ESC{log_marker}{port_marker}{tail_marker} ")
        header.addstr(0, 0, text[:w - 1])
        header.refresh()

    render_header()

    left.addstr(" parancsok\n", curses.A_BOLD)
    left.addstr(" ---------\n")
    left.refresh()
    output.write(" eszköz kimenete\n")
    output.write(" ----------------\n")
    output.refresh()

    try:
        ser = serial.Serial(port_path, baud, timeout=0.1)
    except serial.SerialException as e:
        raise RuntimeError(f"Nem tudtam nyitni {port_path}: {e}")

    out_q = queue.Queue()
    stop_ev = threading.Event()
    t = threading.Thread(target=reader_loop,
                         args=(ser, out_q, stop_ev), daemon=True)
    t.start()

    def close_port():
        """A soros portot lezárja (pl. flasheléshez), a reader-szálat leállítja."""
        nonlocal ser
        if ser is None:
            return "port már zárva"
        stop_ev.set()
        t.join(timeout=0.5)
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        port_closed[0] = True
        return "port lezárva — most flashelhetsz; 'port open' a visszanyitáshoz"

    def open_port():
        """A portot újranyitja és új reader-szálat indít."""
        nonlocal ser, stop_ev, t
        if ser is not None:
            return "port már nyitva"
        try:
            ser = serial.Serial(port_path, baud, timeout=0.1)
        except serial.SerialException as e:
            return f"nem tudtam nyitni: {e}"
        stop_ev = threading.Event()
        t = threading.Thread(target=reader_loop,
                             args=(ser, out_q, stop_ev), daemon=True)
        t.start()
        port_closed[0] = False
        return "port újranyitva"

    def drain_output():
        wrote = False
        while True:
            try:
                data = out_q.get_nowait()
            except queue.Empty:
                break
            text = data.decode("utf-8", errors="replace")
            output.write(text)
            if log_fh is not None:
                log_fh.write(clean_text(text))
            wrote = True
        if wrote:
            output.refresh()
        return wrote

    def line_edit(label, max_len=400):
        prompt.clear()
        prompt.box()
        prompt.addstr(1, 2, label)
        prompt.refresh()

        buf = []
        x_start = 2 + len(label)
        prompt.move(1, x_start)
        prompt.refresh()

        prompt.timeout(50)
        prompt.keypad(True)

        scroll_dirty = [False]

        def post_scroll():
            output.refresh()
            render_header()
            scroll_dirty[0] = True

        try:
            while True:
                if drain_output() and not output.follow:
                    pass

                prompt.refresh()
                if scroll_dirty[0]:
                    prompt.refresh()
                    scroll_dirty[0] = False

                try:
                    ch = prompt.getch()
                except KeyboardInterrupt:
                    return None

                if ch == -1:
                    continue
                if ch in (10, 13, curses.KEY_ENTER):
                    return "".join(buf).strip()
                if ch == 27:
                    return None
                if ch in (curses.KEY_BACKSPACE, 127, 8):
                    if buf:
                        buf.pop()
                        y, x = prompt.getyx()
                        if x > x_start:
                            prompt.move(y, x - 1)
                            prompt.delch()
                            prompt.refresh()
                    continue

                if ch == curses.KEY_PPAGE:
                    output.scroll(-(output.visible_h - 2))
                    post_scroll(); continue
                if ch == curses.KEY_NPAGE:
                    output.scroll(+(output.visible_h - 2))
                    post_scroll(); continue
                if ch == curses.KEY_HOME:
                    output.scroll_to_top()
                    post_scroll(); continue
                if ch == curses.KEY_END:
                    output.scroll_to_tail()
                    post_scroll(); continue
                if ch == 16:   # Ctrl-P
                    output.scroll(-1)
                    post_scroll(); continue
                if ch == 14:   # Ctrl-N
                    output.scroll(+1)
                    post_scroll(); continue

                if 32 <= ch < 127 and len(buf) < max_len:
                    buf.append(chr(ch))
                    try:
                        prompt.addch(ch)
                    except curses.error:
                        pass
                    prompt.refresh()
        finally:
            prompt.timeout(-1)

    try:
        while True:
            cmd = line_edit("cmd (quit=kilép): ")
            if cmd is None or cmd in ("quit", "exit", "q"):
                break
            if not cmd:
                continue

            # Helyi parancs: a soros portot zárja/nyitja (NEM küld frame-et az
            # eszköznek). Flasheléshez 'port close', utána 'port open'.
            if cmd == "port":
                sub = line_edit("port (open/close): ")
                if sub is None:
                    break
                sub = (sub or "").strip().lower()
                if sub in ("close", "c"):
                    msg = close_port()
                elif sub in ("open", "o"):
                    msg = open_port()
                else:
                    msg = f"port: ismeretlen '{sub}' (open/close)"
                try:
                    left.addstr(f" > port {sub}\n")
                except curses.error:
                    pass
                left.refresh()
                output.write(f"\n[{msg}]\n")
                output.refresh()
                render_header()
                continue

            payload = line_edit("payload (üres OK): ")
            if payload is None:
                break

            frame = f"##{cmd}$${payload}##" if payload else f"##{cmd}##"

            try:
                left.addstr(f" > {frame}\n")
            except curses.error:
                pass
            left.refresh()

            if ser is None:
                output.write("\n[port zárva — 'port open' előbb]\n")
                output.refresh()
                continue
            try:
                ser.write((frame + "\r\n").encode())
                ser.flush()
            except serial.SerialException as e:
                output.write(f"\n[soros írási hiba: {e}]\n")
                output.refresh()
                continue
            if log_fh is not None:
                log_fh.write(f"\n>>> sent: {frame}\n")

    finally:
        stop_ev.set()
        try:
            ser.close()
        except Exception:
            pass
        if log_fh is not None:
            try:
                log_fh.write(f"--- mp3ctl session end "
                             f"{datetime.now().isoformat(timespec='seconds')} ---\n")
                log_fh.close()
            except Exception:
                pass


def pick_port():
    ports = sorted(list_ports.comports(), key=lambda p: p.device)
    ports = [p for p in ports if "Bluetooth" not in p.device and "debug" not in p.device]
    if not ports:
        print("Nem találtam soros portot.", file=sys.stderr)
        return None
    if len(ports) == 1:
        p = ports[0]
        print(f"Az egyetlen elérhető portot használom: {p.device}  ({p.description})")
        return p.device

    print("Elérhető soros portok:")
    for i, p in enumerate(ports, start=1):
        print(f"  [{i}] {p.device}    {p.description}")
    while True:
        try:
            raw = input(f"Válassz [1-{len(ports)}] (Enter=1, q=kilép): ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return None
        if raw in ("q", "quit", "exit"):
            return None
        if raw == "":
            return ports[0].device
        if raw.isdigit():
            idx = int(raw)
            if 1 <= idx <= len(ports):
                return ports[idx - 1].device
        print(f"  érvénytelen: {raw!r}")


def resolve_log_path(arg_value):
    if arg_value == "":
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return Path(__file__).resolve().parent / f"mp3ctl-{stamp}.log"
    return Path(arg_value).expanduser()


def main():
    ap = argparse.ArgumentParser(
        description="Interaktív CLI REPL UART-on a hannamp3-hoz.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("port", nargs="?", help="soros port (pl. /dev/cu.usbmodemXXXX)")
    ap.add_argument("baud", nargs="?", type=int, default=DEFAULT_BAUD,
                    help=f"baud rate (default {DEFAULT_BAUD})")
    ap.add_argument("--log", nargs="?", const="", default=None, metavar="PATH",
                    help="eszköz kimenetét naplózza fájlba; PATH nélkül "
                         "mp3ctl-<timestamp>.log a script könyvtárában")
    args = ap.parse_args()

    port = args.port or pick_port()
    if port is None:
        sys.exit(1)

    log_path = None
    if args.log is not None:
        log_path = resolve_log_path(args.log)
        if not log_path.parent.is_dir():
            print(f"Log könyvtár nincs: {log_path.parent}", file=sys.stderr)
            sys.exit(1)

    try:
        curses.wrapper(main_tui, port, args.baud, log_path)
    except KeyboardInterrupt:
        pass
    except RuntimeError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
