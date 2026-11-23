#!/usr/bin/env python3
"""Host checks for ticket 01: unified wake-latency timeline export."""

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.latency_summary import load_latency_log, summarize


LOG_LINE = re.compile(
    r"^(?P<iso>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z) "
    r"LAT session=(?P<session>\S+) event=(?P<event>[a-z0-9-]+) "
    r"monotonic_ms=(?P<monotonic>\d+)(?: room=(?P<room>\S+))?(?: detail=(?P<detail>.+))?$"
)


def timeline(events, *, session="wake-00000001", room="gw-21260174"):
    lines = []
    for index, (event, monotonic_ms) in enumerate(events):
        room_field = f" room={room}" if event == "proxy-connected" and room else ""
        seconds = 36 + index
        minute = 7 + seconds // 60
        second = seconds % 60
        lines.append(
            f"2026-11-23T08:{minute:02d}:{second:02d}.000Z LAT session={session} "
            f"event={event} monotonic_ms={monotonic_ms}{room_field}"
        )
    return "\n".join(lines) + "\n"


class LatencySummaryChecks(unittest.TestCase):
    def test_parses_messages_without_lat_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text("I (53496) app_main: wake detected\nI (56547) ws: connected\n", encoding="utf-8")
            events = load_latency_log(path)
        self.assertEqual(events, [])

    def test_ignores_malformed_lat_lines(self):
        text = (
            "garbage LAT session=wake-1 event=wake-requested monotonic_ms=100\n"
            "2026-11-23T08:07:36.000Z LAT session=wake-1 event=bad-event monotonic_ms=oops\n"
            "2026-11-23T08:07:37.000Z LAT session=wake-1 event=ws-transport-connected monotonic_ms=3050\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(text, encoding="utf-8")
            events = load_latency_log(path)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].event, "ws-transport-connected")

    def test_summarizes_three_required_durations(self):
        events = timeline([
            ("wake-requested", 53496),
            ("ws-before-tcp-connect", 53520),
            ("ws-transport-connected", 56547),
            ("ws-tls-and-handshake-done", 56547),
            ("proxy-connected", 56719),
            ("audio-upload-ready", 56719),
            ("first-live-uplink", 59130),
            ("speech-stopped", 60305),
            ("agent-first-audio", 60861),
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(events, encoding="utf-8")
            summary = summarize(load_latency_log(path))
        self.assertEqual(summary["session_id"], "wake-00000001")
        self.assertEqual(summary["room"], "gw-21260174")
        self.assertEqual(summary["durations_ms"]["wake_to_ws_transport_connected"], 3051)
        self.assertEqual(summary["durations_ms"]["auth_to_first_live_uplink"], 2411)
        self.assertEqual(summary["durations_ms"]["speech_stopped_to_agent_first_audio"], 556)
        self.assertEqual(summary["overall_ms"], 7365)

    def test_rejects_incomplete_timeline(self):
        events = timeline([
            ("wake-requested", 53496),
            ("ws-before-tcp-connect", 53520),
            ("ws-transport-connected", 56547),
            ("ws-tls-and-handshake-done", 56547),
            ("proxy-connected", 56719),
            ("audio-upload-ready", 56719),
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(events, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "first-live-uplink"):
                summarize(load_latency_log(path))

    def test_warns_when_monotonic_clock_wraps(self):
        events = timeline([
            ("wake-requested", 4294960000),
            ("ws-before-tcp-connect", 4294960100),
            ("ws-transport-connected", 4294967295),
            ("ws-tls-and-handshake-done", 4294967295),
            ("proxy-connected", 10),
            ("audio-upload-ready", 12),
            ("first-live-uplink", 20),
            ("speech-stopped", 30),
            ("agent-first-audio", 40),
        ], session="wake-wrap")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(events, encoding="utf-8")
            summary = summarize(load_latency_log(path))
        self.assertTrue(summary["warnings"])
        self.assertTrue(any("monotonic wrap" in warning for warning in summary["warnings"]))

    def test_reports_zero_when_agent_audio_precedes_speech_stopped(self):
        events = timeline([
            ("wake-requested", 15150),
            ("ws-before-tcp-connect", 15244),
            ("ws-transport-connected", 18318),
            ("ws-tls-and-handshake-done", 18320),
            ("proxy-connected", 18442),
            ("audio-upload-ready", 18444),
            ("first-live-uplink", 18465),
            ("agent-first-audio", 19033),
            ("speech-stopped", 21147),
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(events, encoding="utf-8")
            summary = summarize(load_latency_log(path))
        self.assertEqual(
            summary["durations_ms"]["speech_stopped_to_agent_first_audio"], 0
        )
        self.assertTrue(any(
            "agent-first-audio preceded speech-stopped" in warning
            for warning in summary["warnings"]
        ))

    def test_summary_is_json_serializable(self):
        events = timeline([
            ("wake-requested", 100),
            ("ws-before-tcp-connect", 110),
            ("ws-transport-connected", 200),
            ("ws-tls-and-handshake-done", 200),
            ("proxy-connected", 220),
            ("audio-upload-ready", 225),
            ("first-live-uplink", 250),
            ("speech-stopped", 300),
            ("agent-first-audio", 350),
        ], session="wake-json", room=None)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.log"
            path.write_text(events, encoding="utf-8")
            summary = summarize(load_latency_log(path))
        json.dumps(summary, ensure_ascii=False)
        self.assertEqual(summary["room"], "")


if __name__ == "__main__":
    unittest.main()
