#!/usr/bin/env python3
"""Generate timing include files and check ticket 01 latency instrumentation."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from check_device_auth import extract


def main():
    app = (ROOT / "main/app_main.c").read_text(encoding="utf-8")
    ws = (ROOT / "main/ws_service.c").read_text(encoding="utf-8")
    audio = (ROOT / "main/audio_in.c").read_text(encoding="utf-8")
    output = ROOT / "work/speech-handoff"
    output.mkdir(parents=True, exist_ok=True)
    app_names = (
        "release_reminder", "stop_session_audio", "end_conversation",
        "start_audio_session", "start_conversation", "poll_conversation",
        "handle_talk_action", "poll_reminder",
    )
    (output / "app_session.inc").write_text(
        "\n\n".join(extract(app, name) for name in app_names), encoding="utf-8"
    )
    end = ws.index("} rx_frame_t;") + len("} rx_frame_t;")
    start = ws.rindex("typedef struct {", 0, end)
    ws_names = (
        "reset_speech_status", "ws_service_get_speech_status", "observe_speech_event",
        "observe_rx_packet", "release_rx_frame", "drop_audio", "enqueue_rx_frame",
        "handle_frame", "on_ws", "ws_service_prepare_reminder",
    )
    (output / "ws_platform.inc").write_text(
        ws[start:end] + "\n" + "\n\n".join(extract(ws, name) for name in ws_names),
        encoding="utf-8",
    )

    app_inc = (output / "app_session.inc").read_text(encoding="utf-8")
    ws_inc = (output / "ws_platform.inc").read_text(encoding="utf-8")
    checks = {
        "wake-requested": app.count('latency_log_event("wake-requested"'),
        "audio-upload-ready": app.count('latency_log_event("audio-upload-ready"'),
        "upload-phase-marked": app.count("latency_log_mark_upload_phase()"),
        "ws-before-tcp-connect": ws_inc.count('latency_log_event("ws-before-tcp-connect"'),
        "ws-transport-connected": ws_inc.count('latency_log_event("ws-transport-connected"'),
        "proxy-connected": ws_inc.count('latency_log_event("proxy-connected"'),
        "speech-stopped": ws_inc.count('latency_log_event("speech-stopped"'),
        "agent-first-audio": ws_inc.count('latency_log_event("agent-first-audio"'),
        "first-live-uplink": audio.count('latency_log_event("first-live-uplink"'),
    }
    for event, count in checks.items():
        if count != 1:
            raise SystemExit(f"expected one {event} instrumentation, found {count}")
    print("PASS: wake latency instrumentation present in generated host sources")


if __name__ == "__main__":
    main()
