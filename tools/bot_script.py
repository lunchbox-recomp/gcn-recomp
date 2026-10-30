#!/usr/bin/env python3
"""A bot that presses what a script says, the bot link's example bot (docs/diagnostics.md, "Bots").

Usage: bot_script.py [--pads=N] [--late=MS:N] SOCKET SCRIPT -- COMMAND [ARG ...]

It listens on the Unix socket SOCKET first, then runs COMMAND with GCN_BOT set to SOCKET's full
path, so the game COMMAND starts connects to it with nothing left to chance. At each frame the
game steps it, it answers with the presses SCRIPT holds then, on ports 1 to N (1 by default),
as GCN_INPUT would make them.

SCRIPT is in input_script.h's syntax, e.g. "900:START:10,2/1200:A:5", or a file holding that,
whose # lines are comments. An empty SCRIPT presses nothing.

A game that resets the console starts again and connects again, and the script starts over
with it as GCN_INPUT's would. Once COMMAND exits, the bot says how many frames it answered and
exits with COMMAND's status, or 1 if the link sent it something it doesn't take, or 127 if
COMMAND can't be run.

A bad script or a SOCKET it can't listen on exits 2 before COMMAND runs. While it listens it
holds a lock on SOCKET.lock, a file it leaves in place, so a second bot_script.py on the same
SOCKET exits 2 and leaves both files alone. One that takes the lock replaces any socket file an
earlier run left at SOCKET.

--late=MS:N waits MS milliseconds before it answers each frame whose number is a multiple of N.
It's there to check that how long a bot takes to answer changes nothing in a run. The same bot
with and without it leaves the same input log unpaced or paced under GCN_CATCHUP=0. Only the
end line can differ, since it says where --exit-after caught the guest.
"""
from __future__ import annotations

import argparse
import fcntl
import os
import re
import select
import socket
import stat
import subprocess
import sys
import time
from dataclasses import dataclass

# The PADStatus bits (gcn-recomp's runtime/hw/pad.h), by input_script.h's names.
BUTTONS = {
    "LEFT": 0x0001, "RIGHT": 0x0002, "DOWN": 0x0004, "UP": 0x0008, "Z": 0x0010, "R": 0x0020,
    "L": 0x0040, "A": 0x0100, "B": 0x0200, "X": 0x0400, "Y": 0x0800, "START": 0x1000,
}
PRESS = re.compile(r"(?:([1-9])/)?(\d+):([^:]+):(\d+)")
STICK = re.compile(r"S([LRUD])(\d*)")


class ScriptError(ValueError):
    pass


@dataclass(frozen=True)
class Press:
    port: int  # 1 to 4
    at: int  # the first frame it's held at
    frames: int  # how many frames it's held
    buttons: int
    sx: int = 0  # how far it pushes the stick, out of 127; 0 leaves it alone
    sy: int = 0


def parse_press(item: str) -> Press:
    m = PRESS.fullmatch(item)
    if not m:
        raise ScriptError(f"{item}: not [PORT/]FRAME:BUTTON[+BUTTON]:FRAMES")
    port = int(m[1] or 1)
    if port > 4:
        raise ScriptError(f"{item}: no port {port} (1 to 4)")
    buttons = sx = sy = 0
    for name in m[3].split("+"):
        stick = STICK.fullmatch(name)
        if name in BUTTONS:
            buttons |= BUTTONS[name]
        elif stick:
            amount = int(stick[2] or 100)
            if amount > 127:
                raise ScriptError(f"{item}: {name} pushes the stick past 127")
            if stick[1] in "LR":
                sx = -amount if stick[1] == "L" else amount
            else:
                sy = amount if stick[1] == "U" else -amount
        else:
            raise ScriptError(f"{item}: no button {name!r}")
    return Press(port, int(m[2]), int(m[4]), buttons, sx, sy)


def parse_script(text: str) -> list[Press]:
    """The presses in a script; an empty item, as after a trailing comma, is skipped."""
    return [parse_press(item) for item in text.split(",") if item]


def read_script(arg: str) -> str:
    """SCRIPT itself, or the file it names without its # lines and its whitespace."""
    if not os.path.isfile(arg):
        return arg
    with open(arg, encoding="utf-8") as f:
        return "".join("".join(line.split()) for line in f if not line.startswith("#"))


def pad_line(presses: list[Press], port: int, frame: int) -> str:
    """The port's state at the frame, as input_script.cpp would leave a port at rest."""
    buttons, sx, sy, trig_l, trig_r = 0, 128, 128, 0, 0
    for p in presses:
        if p.port == port and p.at <= frame < p.at + p.frames:
            buttons |= p.buttons
            if p.sx:
                sx = 128 + p.sx
            if p.sy:
                sy = 128 + p.sy
            if p.buttons & BUTTONS["L"]:
                trig_l = 255
            if p.buttons & BUTTONS["R"]:
                trig_r = 255
    return f"pad {port} {buttons:04X} {sx} {sy} 128 128 {trig_l} {trig_r}\n"


def answer(presses: list[Press], ports: int, frame: int) -> bytes:
    return ("".join(pad_line(presses, port, frame) for port in range(1, ports + 1)) + "done\n").encode()


class LinkError(Exception):
    pass


def serve(conn: socket.socket, presses: list[Press], ports: int,
          late: tuple[int, int] | None = None) -> tuple[int, int | None]:
    """Answers the game's frames until it hangs up, and returns how many and the last. `late`
    is --late's (MS, N)."""
    count, last = 0, None
    with conn.makefile("rb") as lines:
        try:
            for line in lines:
                words = line.split()
                if len(words) != 2 or words[0] != b"frame" or not words[1].isdigit():
                    raise LinkError(f"the link sent {line!r}, not a frame line")
                last = int(words[1])
                if late and last % late[1] == 0:
                    time.sleep(late[0] / 1000)
                conn.sendall(answer(presses, ports, last))
                count += 1
        except ConnectionError:  # the game ended partway through an answer
            pass
    return count, last


def listen(path: str) -> tuple[socket.socket, int]:
    """A socket listening at `path`, and the lock on `path`.lock held while it listens, as an
    open file to close once the socket file is gone. When another bot_script.py holds the lock,
    it's an OSError and neither file is touched, since the two bots would take each other's
    games. With the lock taken, a socket file at `path` is one an earlier run left, and it's
    replaced."""
    lock = os.open(path + ".lock", os.O_RDWR | os.O_CREAT, 0o644)
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise OSError("another bot is listening on it") from None
        try:
            if stat.S_ISSOCK(os.stat(path).st_mode):
                os.unlink(path)
        except FileNotFoundError:
            pass
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            listener.bind(path)
            listener.listen(1)
        except OSError:
            listener.close()
            raise
    except OSError:
        os.close(lock)
        raise
    return listener, lock


def accept(listener: socket.socket, proc: subprocess.Popen) -> socket.socket | None:
    """The game's next connection, or None once COMMAND has exited without one."""
    while True:
        ready, _, _ = select.select([listener], [], [], 0.2)
        if ready:
            return listener.accept()[0]
        if proc.poll() is not None:
            return None


def run(listener: socket.socket, path: str, presses: list[Press], ports: int, command: list[str],
        late: tuple[int, int] | None = None) -> int:
    """Runs COMMAND and answers each game it starts until it exits, and returns COMMAND's
    status, or 1."""
    try:
        proc = subprocess.Popen(command, env={**os.environ, "GCN_BOT": path})
    except OSError as e:
        print(f"bot_script.py: can't run {command[0]}: {e}", file=sys.stderr)
        return 127
    failed = False
    count, last = 0, None
    try:
        while (conn := accept(listener, proc)) is not None:
            with conn:
                try:
                    n, at = serve(conn, presses, ports, late)
                except LinkError as e:
                    print(f"bot_script.py: {e}", file=sys.stderr)
                    failed = True
                    continue
                count += n
                last = at if at is not None else last
    finally:
        status = proc.wait()
    print(f"bot_script.py: answered {count} frame{'' if count == 1 else 's'}"
          + (f", the last {last}" if last is not None else ""), file=sys.stderr)
    return 1 if failed else status


def late_arg(text: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+):([1-9]\d*)", text)
    if not m:
        raise argparse.ArgumentTypeError(f"MS:N, two whole numbers and N from 1, not {text}")
    return int(m[1]), int(m[2])


def main(argv: list[str]) -> int:
    if "--" not in argv:
        print("usage: bot_script.py [--pads=N] [--late=MS:N] SOCKET SCRIPT -- COMMAND [ARG ...]",
              file=sys.stderr)
        return 2
    split = argv.index("--")
    ap = argparse.ArgumentParser(prog="bot_script.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pads", type=int, choices=range(1, 5), default=1, metavar="N")
    ap.add_argument("--late", type=late_arg, metavar="MS:N")
    ap.add_argument("socket")
    ap.add_argument("script")
    args = ap.parse_args(argv[:split])
    command = argv[split + 1:]
    if not command:
        print("bot_script.py: no COMMAND after --", file=sys.stderr)
        return 2
    try:
        presses = parse_script(read_script(args.script))
        for p in presses:
            if p.port > args.pads:
                raise ScriptError(f"a press on port {p.port}, past --pads={args.pads}")
    except (OSError, ScriptError) as e:
        print(f"bot_script.py: {e}", file=sys.stderr)
        return 2
    path = os.path.abspath(args.socket)
    try:
        listener, lock = listen(path)
    except OSError as e:
        print(f"bot_script.py: can't listen on {path}: {e}", file=sys.stderr)
        return 2
    try:
        return run(listener, path, presses, args.pads, command, args.late)
    finally:
        listener.close()
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        # Only now, or a bot that took the lock first could have its new socket file removed.
        os.close(lock)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
