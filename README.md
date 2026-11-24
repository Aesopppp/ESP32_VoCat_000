# ESP32_VoCatXKorvo_RE

在 YMOS Korvo 工程结构上实现的 **VoCat v1.2 触屏语音终端**。使用 ESP-IDF 6.1、C 模块和事件总线，通过 WebSocket 连接语音平台。

公开仓库：[Aesopppp/ESP32_VoCatXKorvo_RE](https://github.com/Aesopppp/ESP32_VoCatXKorvo_RE)，承接 [ESP32_VoCatXKorvo](https://github.com/Aesopppp/ESP32_VoCatXKorvo) 的源码和提交历史。基于 [YMOS ESP32 工程](https://github.com/zhangff-2021/ymos-esp32) 的本地备份提交 `91d1c9aa320349d2d512dd9e7ba90409091386a8` 适配，硬件配置参考已验证的 VoCat 实现。历史从脱敏后的 VoCat 源码快照开始保存。

## 本版功能

- 本地唤醒词 **“你好小智”**。设备可在待机时预连接 WebSocket，唤醒后复用连接，并按顺序上传紧接唤醒词说出的第一句话。网关协议、开关与联测步骤见[待机预连接与首句上传](docs/wake-first-phrase.md)。
- ES7210 麦克风及硬件扬声器回采，AFE 回声消除与 VAD；16 kHz Opus 音频。
- 统一全双工会话。鉴权后连续上传麦克风，同时播放下行 Opus。AI、电话和提醒共用收发链路。平台停发后播完已接收缓存。平台没有标记用户正在说话、下行也持续安静满 30 秒时回到待机。
- 全双工音频优先：上行 Opus 复杂度 0，16 kHz/单声道/20 ms/24 kbps 不变；连续上传期间关闭 Wi-Fi modem sleep，结束后恢复。保留 512 ms 有界收音 FIFO 和丢帧诊断，验收见 [统一全双工](docs/unified-full-duplex.md)。
- 平台负责语音打断；设备不发送 response.cancel、不按本地人声清空播放。短音频没有结束事件也能起播，缓存排空后可直接接续新音频。
- NVS Wi-Fi 保存和本地网页配网；运行中 **BOOT 长按 3 秒进入配网**，短按无操作。
- 360×360 圆屏，复用 VoCat 的 EAF 基础表情；当前采用音频优先档：待机动画 20fps，对话显示静态状态图。底部上滑打开菜单，调节音量/亮度、查看当前 Wi-Fi、进入或退出配网。
- 首页轻触切换会话：待机时唤醒，连接中/监听/播放时结束并关闭 WebSocket。菜单操作不挂断，音量和亮度自动保存，开机恢复。
- 可选设备平台 HTTP 握手：使用平台签发的设备 ID/PSK 获取运行配置，启用后语音连接使用下发的 URL/token，并在接近过期时续期；握手失败不回退到旧 key。仓库默认关闭，本机可通过私有配置启用，详见 [HTTP 握手与语音鉴权](docs/device-http-handshake.md)。
- 设备 MQTT：使用握手凭据、独立 TLS 连接和周期状态上报；待机通知唤醒并开启全双工会话，在线提醒由平台直接插播。保留任务 ID 和待交接通知持久化，不跟踪播报结果、不发播放业务 ACK；重复任务由平台处理，详见 [设备 MQTT 接入](docs/device-mqtt.md)。
- 本版不启用 SD 卡和其他按键。

## 配网

正常启动后长按 BOOT 约 3 秒，手机连接开放热点 `YMOS-XXXX`，**无需热点密码**，浏览器打开 **http://192.168.4.1/** 并提交 Wi-Fi 信息。页面中的密码用于连接目标路由器或手机热点。没有已保存网络时自动进入配网。请勿在上电/复位前一直按住 BOOT，否则进入芯片下载模式。

## 开发与构建

- 硬件：ESP-VoCat **v1.2**，16 MB Flash；Octal PSRAM 自动识别，本次实板日志为 16 MB。
- SDK：ESP-IDF **6.1.x**；在对应 ESP-IDF 终端运行 `idf.py build`，或使用 VS Code 的 ESP-IDF 编译按钮。
- 主程序：`main/app_main.c`；引脚与增益：`main/board_config.h`。
- 控制台使用原生 USB Serial/JTAG。屏幕采用 ST77916 + CST816S，菜单说明见 [屏幕与触屏](docs/vocat-screen-menu.md)。
- 产物：`build/vocat_korvo_agent.bin` 和 `build/ui_assets.bin`；首次更新表情版必须使用该构建目录生成的完整 flash 清单，写入新增资源分区和分区表，不能只烧应用。无需擦除 NVS。
- Wi-Fi 配置沿用 YMOS 备份。ACOS 地址为 `wss://119.29.215.114/v1/realtime`；本机密钥见 Git 忽略的 `main/acos_local_config.h`，首次启动按预设编号更新 NVS，之后保留用户配置。

移植基线、引脚表、音频通道、模块职责、完整操作和验证步骤见 [VoCat 移植说明](docs/vocat-port.md)。原 MQTT/按键等历史文档保留作参考，当前功能以本 README 和移植说明为准。

## 从 GitHub 获取后编译

1. 克隆仓库，并使用 ESP-IDF 6.1.x 环境打开工程。
2. 将 `main/acos_local_config.h.example` 复制为 `main/acos_local_config.h`，填写平台提供的密钥。地址默认值位于 `main/board_config.h`；更换地址或密钥后递增本地预设编号，以更新设备 NVS 中的旧值。
3. 执行 `idf.py build`。依赖由组件管理器根据清单和 `dependencies.lock` 下载，首次构建需要网络；Windows 需安装 Git for Windows，以提供 `patch.exe`。
4. 选择实际设备串口，再执行 `idf.py -p COM端口 flash monitor`。运行中长按 BOOT 3 秒可配网。

仓库包含源码、配置、依赖锁、测试和文档；不包含 `build/`、`managed_components/`、本机 VS Code 设置、调试日志及私有配置。没有本地 Wi-Fi 预设时，设备使用已保存的网络或进入网页配网。

默认分支为 `main`。原 YMOS/XChatbot 工程均未修改。构建通过不等于实板验证通过，声学、触发效果和稳定性需要烧录后测试。第三方组件按各自的许可证使用；本仓库未额外声明或替代这些组件的许可。
