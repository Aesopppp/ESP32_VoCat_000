#!/usr/bin/env python3
"""Summarize device LAT timeline lines into the ticket 01 durations."""

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


LINE_RE = re.compile(
    r"^(?P<iso>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z) "
    r"LAT session=(?P<session>\S+) event=(?P<event>[a-z0-9-]+) "
    r"monotonic_ms=(?P<monotonic>\d+)(?: room=(?P<room>\S+))?(?: detail=(?P<detail>.+))?$"
)
UINT32_WRAP = 1 << 32
REQUIRED_EVENTS = (
    "wake-requested",
    "ws-before-tcp-connect",
    "ws-transport-connected",
    "ws-tls-and-handshake-done",
    "proxy-connected",
    "audio-upload-ready",
    "first-live-uplink",
    "speech-stopped",
    "agent-first-audio",
)


@dataclass(frozen=True)
class LatencyEvent:
    iso: str
    session: str
    event: str
    monotonic_ms: int
    room: str
    detail: str


def load_latency_log(path):
    events = []
    with Path(path).open("r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            match = LINE_RE.match(line.rstrip("\r\n"))
            if not match:
                continue
            event = match.group("event")
            if event not in REQUIRED_EVENTS:
                continue
            events.append(
                LatencyEvent(
                    iso=match.group("iso"),
                    session=match.group("session"),
                    event=event,
                    monotonic_ms=int(match.group("monotonic")),
                    room=match.group("room") or "",
                    detail=match.group("detail") or "",
                )
            )
    return events


def _duration(timeline, start, end):
    if start not in timeline:
        raise ValueError(f"missing required latency event: {start}")
    if end not in timeline:
        raise ValueError(f"missing required latency event: {end}")
    if timeline[end] < timeline[start]:
        return None
    return timeline[end] - timeline[start]


def summarize(events):
    if not events:
        raise ValueError("no LAT lines found")
    sessions = {event.session for event in events}
    if len(sessions) != 1:
        raise ValueError(f"expected one wake session, found {sorted(sessions)}")
    timeline = {}
    warnings = []
    last_ms = None
    room = ""
    for event in events:
        if event.event in timeline:
            warnings.append(f"duplicate latency event ignored: {event.event}")
            continue
        if last_ms is not None and event.monotonic_ms < last_ms:
            if last_ms > UINT32_WRAP // 2 and event.monotonic_ms < UINT32_WRAP // 2:
                warnings.append(f"monotonic wrap before {event.event}")
            else:
                warnings.append(f"monotonic timestamp moved backwards before {event.event}")
        timeline[event.event] = event.monotonic_ms
        last_ms = event.monotonic_ms
        if event.room:
            room = event.room
    missing = [name for name in REQUIRED_EVENTS if name not in timeline]
    if missing:
        raise ValueError(f"missing required latency events: {', '.join(missing)}")
    wake = timeline["wake-requested"]
    speech_stopped_to_audio = _duration(
        timeline, "speech-stopped", "agent-first-audio"
    )
    if speech_stopped_to_audio is None:
        speech_stopped_to_audio = 0
        warnings.append(
            "agent-first-audio preceded speech-stopped; reporting 0 ms for that segment"
        )
    result = {
        "session_id": events[0].session,
        "room": room,
        "start_iso": events[0].iso,
        "end_iso": events[-1].iso,
        "events_ms": timeline,
        "durations_ms": {
            "wake_to_ws_transport_connected": _duration(
                timeline, "wake-requested", "ws-transport-connected"
            ),
            "auth_to_first_live_uplink": _duration(
                timeline, "audio-upload-ready", "first-live-uplink"
            ),
            "speech_stopped_to_agent_first_audio": speech_stopped_to_audio,
        },
        "overall_ms": _duration(timeline, "wake-requested", "agent-first-audio"),
        "warnings": warnings,
    }
    return result


def main():
    parser = argparse.ArgumentParser(description="Summarize device wake latency LAT lines")
    parser.add_argument("logs", nargs="+", type=Path, help="device log files")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    events = []
    for path in args.logs:
        events.extend(load_latency_log(path))
    summary = summarize(events)
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return
    durations = summary["durations_ms"]
    print(f"session={summary['session_id']} room={summary['room'] or '-'}")
    print(f"wake -> WS transport connected: {durations['wake_to_ws_transport_connected']} ms")
    print(f"auth -> first live uplink: {durations['auth_to_first_live_uplink']} ms")
    print(f"speech_stopped -> agent first audio: {durations['speech_stopped_to_agent_first_audio']} ms")
    print(f"overall: {summary['overall_ms']} ms")
    for warning in summary["warnings"]:
        print(f"warning: {warning}")


if __name__ == "__main__":
    main()
