#!/usr/bin/env python3
"""Control a running NeoHEN from a PC on the same network (TCP 9048).

  python3 scripts/neohen_ctl.py toolbox  <PS5_IP>   # load the Toolbox now
  python3 scripts/neohen_ctl.py shutdown <PS5_IP>   # stop NeoHEN (kstuff stays)

toolbox is how the Toolbox comes back after Rest Mode with
rest_mode.toolbox_after_resume=manual (the default). The result is shown as a
notification on the console.

shutdown stops util, the private loader on 9020, restarts SceShellUI (which
removes the in-memory Toolbox) and exits the daemon. kstuff keeps running.

Wire format (little-endian): u32 magic 0x4F4E494F ('ONIO'), u32 command
(1 = shutdown, 2 = toolbox). Reply: one byte, 0 = accepted.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys

PORT = 9048
MAGIC = 0x4F4E494F
COMMANDS = {"shutdown": 1, "toolbox": 2}


def send(host: str, command: str, port: int = PORT, timeout: float = 5.0) -> int:
    frame = struct.pack("<II", MAGIC, COMMANDS[command])
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.sendall(frame)
            reply = sock.recv(1)
    except OSError as exc:
        print(f"connection failed: {exc}", file=sys.stderr)
        print("Check that the PS5 is on the same network and NeoHEN is running.",
              file=sys.stderr)
        return 2
    if not reply and command == "shutdown":
        print("shutdown requested (connection closed)")
        return 0
    if reply == b"\0":
        print(f"{command}: accepted")
        return 0
    print(f"{command}: rejected (reply={reply!r}); older NeoHEN/OnionHEN?",
          file=sys.stderr)
    return 1


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=sorted(COMMANDS))
    ap.add_argument("ps5_ip")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    sys.exit(send(args.ps5_ip, args.command, args.port))


if __name__ == "__main__":
    main()
