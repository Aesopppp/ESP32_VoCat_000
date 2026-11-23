# 唤醒首句补传与实时上传

唤醒建连期间继续保留麦克风 PCM。首句结束后不再无限追发连接期间的历史，避免后续正常对话一直落后于现场声音。

## 数据流

1. WakeNet 命中帧及其之前的音频不上传，其后的 PCM 写入原有大 FIFO（约 10.24 秒容量）。
2. 切到 LITE AFE 后，以有效人声启动首句检测；AFE 连续报告静音 160ms 后冻结首句边界。AFE 自身的静音判定时间仍然保留，所以 **160ms 不是从用户停嘴算起的总时间**。该边界是声学停顿，不是语义识别。
3. 边界之后的 PCM 写入独立的 512ms 滚动 FIFO。大 FIFO 中尚未补传的首句不被覆盖。
4. 上传任务按序处理完首句后读取实时 FIFO；旧实时帧可以淘汰，已进入 Opus 编码器或 TX 队列的采样不清空、不重排。无需等平台 ACK，也无需等首句的本地网络发送完成。
5. 如果首句尚未结束，但发送已经追到实时附近，继续沿用原有提前恢复实时的路径。

普通对话仍连续上传，打断继续由平台决定。没有新增 cancel、播放门控或网络协议。新增资源为约 16.5KB PSRAM FIFO；没有新增任务。

## 配置与日志

参数位于 `main/wake_word.h`：

- `WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED=1`：启用本次首句到实时的切换。设为 `0` 并重新编译可恢复此前整段预录音追赶方式。
- `WAKE_WORD_CONNECT_PRE_ROLL_ENABLED=0`：关闭全部唤醒建连预录音，恢复鉴权后才向上传 FIFO 存入音频的行为；AFE 仍持续运行。
- `WAKE_WORD_FIRST_PHRASE_SILENCE_MS=160`：AFE 已报告静音后的额外确认时间。

关键日志：

```text
First phrase boundary captured; later audio uses live FIFO
First phrase queued; switched to live audio (skipped_ms=... live_ms=...)
```

`queued` 表示已交给本地编码上传流程，不能据此认定平台已经收到或识别。`skipped_ms` 是为恢复实时而跳过的旧实时音频。

## 验证范围

`scripts/check_audio.py` 覆盖首句/实时 FIFO 顺序、模式切换残余 VAD、短停顿、纯静音、溢出、采样坐标回绕、关闭开关，以及 512-sample AFE 帧与 320-sample Opus 帧在切换处的全部对齐方式。

烧录后重点测试：紧接唤醒词说问题、等连接后再说、首句中短暂停顿，以及随后播放中的语音打断。主机回归不能证明实际 VAD 边界或端到端延迟。首句之后若又说了多段话，实时 FIFO 只保留最近 512ms，不会把全部旧话继续积压上传。
