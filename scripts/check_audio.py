"""运行生产全双工音频、持续收包和会话静默回归；不连接设备或平台。"""
import argparse
from pathlib import Path
import shutil
import subprocess
from check_device_auth import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"), help="本机 TCC 路径")
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("请用 --cc 指定已安装的 TCC")
    root = Path(__file__).resolve().parents[1]
    output = root / "work/speech-handoff"
    output.mkdir(parents=True, exist_ok=True)
    # 直接提取生产函数；主机桩只替换 RTOS、网络和外设依赖。
    source = (root / "main/app_main.c").read_text(encoding="utf-8")
    (output / "app_session.inc").write_text("\n\n".join(extract(source, name) for name in
        ("release_reminder", "stop_session_audio", "end_conversation", "start_audio_session",
         "abandon_preconnection", "poll_preconnection", "handle_preconnection_event", "start_conversation",
         "poll_conversation", "handle_talk_action", "tagged_ws_event", "poll_reminder")), encoding="utf-8")
    ws = (root / "main/ws_service.c").read_text(encoding="utf-8")
    end = ws.index("} rx_frame_t;") + len("} rx_frame_t;")
    start = ws.rindex("typedef struct {", 0, end)
    (output / "ws_control.inc").write_text("\n\n".join(extract(ws, name) for name in
        ("publish", "ws_service_take_pending_event")), encoding="utf-8")
    names = ("current", "reset_speech_status", "ws_service_get_speech_status", "observe_speech_event",
             "observe_rx_packet", "release_rx_frame", "drop_audio", "enqueue_rx_frame",
             "ws_service_preconnect_valid", "ws_service_preconnect_refresh_due",
             "handle_frame", "on_ws", "ws_service_prepare_reminder")
    (output / "ws_platform.inc").write_text(ws[start:end] + "\n" +
        "\n\n".join(extract(ws, name) for name in names), encoding="utf-8")
    (output / "ws_upload.inc").write_text(extract(ws, "ws_service_send_opus_frame"), encoding="utf-8")
    wifi = (root / "main/wifi_service.c").read_text(encoding="utf-8")
    (output / "wifi_audio.inc").write_text("\n\n".join(extract(wifi, name) for name in
        ("apply_audio_power_save_locked", "wifi_service_set_audio_active",
         "wifi_event_handler")), encoding="utf-8")
    audio = (root / "main/audio_out.c").read_text(encoding="utf-8")
    (output / "audio_stop.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("ramp_to_zero", "audio_out_write", "audio_out_stop", "audio_out_request_stop",
         "audio_out_stop_requested")), encoding="utf-8")
    (output / "audio_signal.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("measure_pcm_signal", "audio_out_set_streaming", "audio_out_play_opus")), encoding="utf-8")
    (output / "audio_continuous.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("ramp_to_zero", "output_pcm_block", "audio_out_write", "audio_out_set_streaming",
         "audio_out_is_streaming", "audio_out_get_last_voice_ms", "audio_out_is_drained")), encoding="utf-8")
    wake = (root / "main/wake_word.c").read_text(encoding="utf-8")
    (output / "wake_fifo.inc").write_text("\n\n".join(extract(wake, name) for name in
        ("audio_buffer_frames_locked", "discard_oldest_audio_locked", "clear_audio_buffer_locked",
         "afe_frame_energy", "reset_first_phrase_detector_locked", "observe_first_phrase_locked",
         "buffer_afe_audio", "wake_word_set_audio_buffer_enabled",
         "wake_word_set_pre_roll", "wake_word_select_hot_pre_roll",
         "wake_word_fallback_cold_pre_roll", "wake_word_prepare_pre_roll_upload",
         "wake_word_read_audio", "wake_word_confirm_audio_sent")), encoding="utf-8")
    cases = {
        "wifi-audio-checks": ["-Itests/audio_host", "-Iwork/speech-handoff",
                              "tests/audio_host/wifi_audio_checks.c"],
        "ws-upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                             "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/speech-handoff",
                             "tests/audio_host/ws_upload_checks.c", "main/proto_service.c",
                             "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                          "tests/audio_host/upload_checks.c"],
        "audio-stop-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/audio_stop_checks.c"],
        "audio-signal-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/audio_signal_checks.c"],
        "wake-fifo-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/wake_fifo_checks.c"],
        "wake-fifo-disabled-checks": ["-DWAKE_WORD_CONNECT_PRE_ROLL_ENABLED=0",
                                     "-Itests/audio_host", "-Iwork/speech-handoff",
                                     "tests/audio_host/wake_fifo_checks.c"],
        "wake-fifo-legacy-replay-checks": ["-DWAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED=0",
                                          "-Itests/audio_host", "-Iwork/speech-handoff",
                                          "tests/audio_host/wake_fifo_checks.c"],
        "ws-platform-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/speech-handoff",
                               "tests/audio_host/ws_platform_checks.c",
                               "main/proto_service.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "audio-continuous-checks": ["-Itests/audio_host", "-Iwork/speech-handoff",
                                    "tests/audio_host/audio_continuous_checks.c"],
        "app-session-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Iwork/speech-handoff", "tests/audio_host/app_session_checks.c"],
    }
    cases["app-session-preconnect-disabled-checks"] = [
        "-DDEVICE_WS_PRECONNECT_ENABLED=0", *cases["app-session-checks"]]
    for name, sources in cases.items():
        executable = output / (name + ".exe")
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            for command in ([args.cc, "-std=c11", "-Wall", "-Werror", "-o", str(executable), *sources],
                            [str(executable)]):
                result = subprocess.run(command, cwd=root, text=True, encoding="utf-8",
                                        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log.write(result.stdout)
                print(result.stdout, end="")
                if result.returncode:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
