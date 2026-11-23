#!/usr/bin/env python3
"""Attach a gateway room ID to LAT lines for one wake session."""

import argparse
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Assign a gateway room to device LAT lines")
    parser.add_argument("log", type=Path)
    parser.add_argument("room")
    parser.add_argument("--session", default="")
    parser.add_argument("--output", type=Path, help="write to a new file instead of in place")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", args.room):
        raise SystemExit("room must be a short identifier")
    session = None
    if args.session:
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", args.session):
            raise SystemExit("session must be a short identifier")
        session = args.session
    else:
        sessions = []
        for line in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.search(r"LAT session=(\S+)", line)
            if match and match.group(1) not in sessions:
                sessions.append(match.group(1))
        if len(sessions) != 1:
            raise SystemExit(f"expected one wake session, found {len(sessions)}; use --session")
        session = sessions[0]
    updated = []
    for line in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
        if f"LAT session={session} " in line and " room=" not in line:
            line = re.sub(
                r"(monotonic_ms=\d+)",
                lambda match: f"{match.group(1)} room={args.room}",
                line,
                count=1,
            )
        updated.append(line)
    target = args.output or args.log
    target.write_text("\n".join(updated) + "\n", encoding="utf-8")
    print(f"assigned room={args.room} to session={session}")


if __name__ == "__main__":
    main()
