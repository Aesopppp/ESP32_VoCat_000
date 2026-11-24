# 唤醒首句补传与实时上传
唤醒建连期间继续保留麦克风 PCM。鉴权成功后封存当时已保留的预录音，按原顺序全部补传，再切换到实时音频；超过缓存上限时只淘汰最旧帧并显式统计。
## 数据流
1. WakeNet 命中帧及其之前的音频不上传，其后的 PCM 写入大 FIFO。默认目标保留 6000ms，硬上限 8000ms（受 320 帧、约 10.24 秒容量约束）；超过上限时丢弃最旧帧，优先保住最近话音。
2. WakeNet 命中后到鉴权成功前，PCM 持续写入预录音缓存；鉴权前不会提前切到 live，也不会因本地 VAD 判断而截断用户连续讲话。
3. 鉴权成功时封存整个 retained window，不在封存点裁到 800ms。封存后新 PCM 写入独立的 512ms 滚动 live FIFO；上传任务严格按“预录音全部补完 → live FIFO”的顺序发送。旧 live 帧可以淘汰，已进入 Opus 编码器或 TX 队列的采样不清空、不重排。无需等平台 ACK，也无需等本地网络发送完成。
4. 若连接建 2 秒，补传 2 秒；若连接建 4 秒，补传 4 秒；只有超过缓存目标/硬上限才丢弃最旧帧。日志用 `dropped_ms` 报告实际截断量。
5. 当前实现仍从 WakeNet 判定帧末端开始保留音频；“你好小智”本身通常发生在判定前，默认不随预录音上传。若希望模型对唤醒词本身做轻量回应，建议由平台侧配置开场白，不要依赖设备补发唤醒词。
普通对话仍连续上传，打断继续由平台决定。没有新增 cancel、播放门控或网络协议。新增资源为约 16.5KB PSRAM FIFO；没有新增任务。
## 配置与日志
参数位于 `main/wake_word.h`：
- `WAKE_WORD_CONNECT_PRE_ROLL_ENABLED=0`：关闭全部唤醒建连预录音，恢复鉴权后才向上传 FIFO 存入音频的行为；AFE 仍持续运行。
- `WAKE_WORD_REPLAY_CAP_MS=6000`：鉴权封存时希望保留的预录音时长。
- `WAKE_WORD_PRE_ROLL_MAX_MS=8000`：预录音硬上限；超过后保留最近窗口并丢弃最旧帧。
关键日志：
```text
Pre-roll upload ready: boundary_sample=... buffered_ms=... dropped_ms=...
Pre-roll replay ended; switched to live audio (replayed_ms=... skipped_ms=... live_ms=...)
```
`queued` 表示已交给本地编码上传流程，不能据此认定平台已经收到或识别。`skipped_ms` 是为恢复实时而跳过的旧实时音频。01 时间线还会输出 `pre-roll-replay-ready`、`pre-roll-replay-ended` 和 `live-fifo-source-selected`；真正首包成功发送仍记为 `first-live-uplink`。
## 验证范围
`scripts/check_audio.py` 覆盖预录音/实时 FIFO 的严格顺序、2 秒与 5 秒建连完整补传、缓存上限溢出、采样坐标回绕、关闭开关，以及 512-sample AFE 帧与 320-sample Opus 帧在切换处的对齐方式。
烧录后重点测试：紧接唤醒词连续说 3 秒以上、等连接后再说，以及随后播放中的语音打断。主机回归不能证明实际 VAD 边界或端到端延迟。封存后若用户继续长说，实时 FIFO 只保留最近 512ms，不会把全部旧话继续积压上传。


