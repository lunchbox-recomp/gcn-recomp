"""Checks for bot_script.py, against a fake engine.

The fake engine is this file run as a program (fake_engine below): it connects to GCN_BOT as
the engine does, steps the bot at the frames it's given, writes what the bot answered to a
file and exits with the status it's given. Run with `python3 -m unittest discover -s tools`.
"""
from __future__ import annotations

import os
import socket
import sys
import tempfile
import unittest
from pathlib import Path

import bot_script
from bot_script import Press, ScriptError, pad_line, parse_script

HERE = Path(__file__).resolve()


def fake_engine(out: str, connections: str, status: str) -> int:
    """Steps the bot at each connection's frames ("0,3,7;0,1" is two connections, as a game
    that resets the console makes, and "" none) and writes each answer, up to its done, to
    `out`."""
    answers = []
    for frames in connections.split(";") if connections else []:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(10)
            s.connect(os.environ["GCN_BOT"])
            with s.makefile("rb") as lines:
                for frame in frames.split(","):
                    if frame == "junk":
                        s.sendall(b"hello\n")
                        answers.append(lines.readline().decode())  # the bot hangs up: ""
                        continue
                    s.sendall(f"frame {frame}\n".encode())
                    answer = ""
                    while not answer.endswith("done\n"):
                        line = lines.readline().decode()
                        if not line:
                            break
                        answer += line
                    answers.append(answer)
    Path(out).write_text("|".join(answers))
    return int(status)


class ScriptTest(unittest.TestCase):
    def test_parses_presses(self):
        self.assertEqual(parse_script("900:START:10,2/1200:A+B:5,3/7:SL:2,10:SU40+R:1,"), [
            Press(1, 900, 10, 0x1000),
            Press(2, 1200, 5, 0x0300),
            Press(3, 7, 2, 0, sx=-100),
            Press(1, 10, 1, 0x0020, sy=40),
        ])
        self.assertEqual(parse_script(""), [])

    def test_refuses_what_input_script_would_skip(self):
        for bad in ("900:START", "900:JUMP:10", "5/900:A:10", "900::10", "900:SR128:1", "x:A:1", "900:A:B:1"):
            with self.subTest(bad=bad), self.assertRaises(ScriptError):
                parse_script(bad)

    def test_pad_line_matches_input_script(self):
        presses = parse_script("10:A:5,12:B+SR19:1,12:L:2,2/12:R+SD:1,14:SL:1")
        self.assertEqual(pad_line(presses, 1, 9), "pad 1 0000 128 128 128 128 0 0\n")
        self.assertEqual(pad_line(presses, 1, 10), "pad 1 0100 128 128 128 128 0 0\n")
        self.assertEqual(pad_line(presses, 1, 12), "pad 1 0340 147 128 128 128 255 0\n")
        self.assertEqual(pad_line(presses, 2, 12), "pad 2 0020 128 28 128 128 0 255\n")
        self.assertEqual(pad_line(presses, 1, 14), "pad 1 0100 28 128 128 128 0 0\n")
        self.assertEqual(pad_line(presses, 1, 15), "pad 1 0000 128 128 128 128 0 0\n")

    def test_reads_a_route_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "route.txt"
            path.write_text("# a comment\n120:A:5,\n 300:START:5\n")
            self.assertEqual(bot_script.read_script(str(path)), "120:A:5,300:START:5")
        self.assertEqual(bot_script.read_script("1:A:1"), "1:A:1")


class RunTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.sock = os.path.join(self.dir.name, "bot.sock")
        self.out = os.path.join(self.dir.name, "answers.txt")

    def run_bot(self, *args: str, connections: str = "0", status: int = 0) -> int:
        command = [sys.executable, "-B", str(HERE), "--fake-engine", self.out, connections, str(status)]
        return bot_script.main([*args, "--", *command])

    def answers(self) -> list[str]:
        return Path(self.out).read_text().split("|")

    def test_answers_each_frame_and_exits_with_the_command(self):
        status = self.run_bot("--pads=2", self.sock, "3:A:2,2/7:START:1", connections="0,3,7", status=5)
        self.assertEqual(status, 5)
        self.assertEqual(self.answers(), [
            "pad 1 0000 128 128 128 128 0 0\npad 2 0000 128 128 128 128 0 0\ndone\n",
            "pad 1 0100 128 128 128 128 0 0\npad 2 0000 128 128 128 128 0 0\ndone\n",
            "pad 1 0000 128 128 128 128 0 0\npad 2 1000 128 128 128 128 0 0\ndone\n",
        ])
        self.assertFalse(os.path.exists(self.sock))

    def test_an_empty_script_is_neutral(self):
        self.assertEqual(self.run_bot(self.sock, "", connections="0,1"), 0)
        self.assertEqual(self.answers(), ["pad 1 0000 128 128 128 128 0 0\ndone\n"] * 2)

    def test_a_game_that_connects_again_starts_the_script_over(self):
        self.assertEqual(self.run_bot(self.sock, "0:A:1", connections="0,1;0"), 0)
        self.assertEqual(self.answers(), [
            "pad 1 0100 128 128 128 128 0 0\ndone\n",
            "pad 1 0000 128 128 128 128 0 0\ndone\n",
            "pad 1 0100 128 128 128 128 0 0\ndone\n",
        ])

    def test_a_command_that_never_connects(self):
        self.assertEqual(self.run_bot(self.sock, "", connections="", status=3), 3)
        self.assertEqual(self.answers(), [""])
        self.assertFalse(os.path.exists(self.sock))

    def test_a_line_that_is_not_a_frame_hangs_up(self):
        self.assertEqual(self.run_bot(self.sock, "", connections="junk"), 1)
        self.assertEqual(self.answers(), [""])

    def test_a_bad_script_runs_nothing(self):
        self.assertEqual(self.run_bot(self.sock, "900:JUMP:10"), 2)
        self.assertEqual(self.run_bot(self.sock, "2/900:A:10"), 2)  # port 2 past --pads=1
        self.assertFalse(os.path.exists(self.out))
        self.assertFalse(os.path.exists(self.sock))

    def test_takes_the_place_of_an_old_socket_but_not_a_file(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as old:
            old.bind(self.sock)
        self.assertEqual(self.run_bot(self.sock, ""), 0)
        Path(self.sock).write_text("not a socket")
        self.assertEqual(self.run_bot(self.sock, ""), 2)
        self.assertEqual(Path(self.sock).read_text(), "not a socket")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fake-engine"]:
        sys.exit(fake_engine(*sys.argv[2:]))
    unittest.main()
