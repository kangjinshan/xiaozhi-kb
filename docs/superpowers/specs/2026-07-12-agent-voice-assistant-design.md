# XiaoZhi 录音器接入 Agent 语音助手设计

日期：2026-07-12

涉及仓库：`/Users/kanayama/xiaozhi-kb`、`/Users/kanayama/Desktop/AI/agent`

部署目标：`https://agent.jinshanweb.com` / Azure `agent-platform.service`

## 1. 目标与验收边界

把现有 ESP32-C6 的 SD 卡录音器升级为金山用户的专属 AI 语音助手，同时保留已有录音、文件菜单和回放能力。

完成状态必须同时满足：

1. 设备复用已有 Wi-Fi 配置，与 Azure Agent 建立带 TLS 的持久 WebSocket 连接，并能断线重连。
2. 用户点 `REC` 开始、点 `STOP` 停止；录音先可靠写入 SD 卡，再提交给 Agent。
3. Agent 使用 Azure STT 转写，调用绑定到金山用户的 `Opus 4.8` 助手，再用 Azure TTS 生成设备可播放的 WAV。
4. 设备完整接收回复语音、校验并落盘后自动播放。
5. 每轮的用户 WAV、助手 WAV、用户转写、助手文本、时间、turn ID 和处理结果都保存在 SD 卡；服务端也保留数据库消息和音频审计记录。
6. 断网或服务失败不丢录音；待发送轮次重连后重试，服务端依靠 turn ID 保证不会重复调用模型或产生多条回复。
7. Azure Agent 中存在一个 `金山` 用户、一个默认 AI 语音助手、一个专用硬件 Connector/Endpoint/Identity；助手明确使用小鹿项目现有的 `Opus 4.8` Provider。
8. 不暴露、打印或提交小鹿的密钥；配置在 Azure 主机内部复制。
9. 后端测试、固件主机测试、ESP-IDF 构建、Azure 验收和真机录音到自动播放闭环全部通过。

## 2. 方案选择

### 方案 A：逐轮 HTTPS 上传和下载

实现最简单，但每轮重新建连，设备难以稳定展示在线状态；回复下载中断、双向进度和断线续传协议也较笨重。

### 方案 B：持久 WebSocket 上的文件分块协议（采用）

设备在线时保持一条 WSS 连接。录音文件和回复文件都从 SD 卡分块读写，不把整段音频放入 ESP32 内存。连接能承载状态、确认、幂等重试和回复进度，符合“完整建立连接”和“收到语音自动播放”的要求。

### 方案 C：复刻小智官方实时 Opus 音频协议

可实现边录边传和更低延迟，但会绕开当前已经真机验证的录音器控制流。要在音频服务中同时追加双向 SD 存档，并协调 AMOLED/SD 的 SPI2 共享，改动和稳定性风险最高。本需求未要求持续监听或实时打断，因此不采用。

## 3. 总体架构

### 3.1 设备端

在现有 `main/apps/recorder/` 上增加三个独立边界：

- `RecorderNetwork`：只负责 Wi-Fi 生命周期、WSS 连接、鉴权、心跳和指数退避重连。
- `AgentVoiceProtocol`：负责 JSON 控制帧、二进制音频分块、turn 状态机、SHA-256 校验与幂等确认。
- `AgentTurnStore`：负责 SD 目录、manifest、临时文件、原子改名和待发送轮次恢复。

录音 DSP、WAV 写入和播放器继续复用现有实现。WebSocket 回调不能直接操作 LVGL、codec 或 SD 卡，只向有界事件队列发布事件；现有 Recorder 主任务串行执行所有 SD、显示和播放操作，保持 SPI2 仲裁约束。

### 3.2 Agent 服务端

增加专用设备语音 WebSocket 路由和 `HardwareVoiceService`：

1. 按设备 Endpoint 校验凭证并确认 Identity 已绑定 active 用户。
2. 把上传音频先写临时文件，核对大小与 SHA-256 后原子落盘。
3. 按 `endpoint_id + turn_id` 做唯一幂等；已完成的轮次直接重放已存回复。
4. 调用通用语音处理管线：STT → 用户默认助手路由 → 上下文/记忆 → LLM → TTS。
5. 将入站和出站 `Message`、对话上下文、LLM trace、转写和音频路径提交到数据库。
6. 先发送回复元数据，再分块发送回复 WAV，等待设备确认保存完成。

服务端增加硬件 turn 数据表，保存状态、哈希、音频路径、转写、回复文本、关联消息和失败原因。数据库约束保证重连重试不会重复生成回复。

### 3.3 Azure 配置与用户

在 Azure 主机内部执行一次性引导脚本：

- 从 `/home/azureuser/xiaolu/backend/data/xiaolu.db` 复制 `Opus 4.8` Provider 字段到 Agent 数据库，保留协议、模型、端点和密钥，不把值带回本地输出。
- 从小鹿运行配置复制 Azure STT/TTS 凭证到 Agent 的受限 `.env`，权限保持 `0600`。
- 幂等创建 managed user `金山`。
- 幂等创建默认助手 `金山AI语音助手`，Provider 指向 `Opus 4.8`，语音回复简短、自然、口语化。
- 创建专用 hardware Connector、当前设备 Endpoint 和绑定到金山用户的 active Identity。
- 生成单设备随机令牌；服务端只存强哈希，设备令牌写入 NVS，不进入 Git、文档或日志。

全局 `PRODUCTION_SENDS_ENABLED` 继续保持 false。设备的同步请求/响应由独立的 `HARDWARE_VOICE_ENABLED` 与指定 Endpoint 白名单控制，不会开启小鹿、微信、邮件或定时主动发送。

## 4. 设备协议

连接地址为 `wss://agent.jinshanweb.com/api/device/voice/ws`。设备通过 `Authorization: Bearer <device-token>`、`Device-Id` 和协议版本请求头鉴权。

### 4.1 连接

服务端接受后返回：

```json
{"type":"ready","protocol":1,"device_id":"...","heartbeat_seconds":25}
```

设备只在收到 `ready` 后显示 `ONLINE`。双方用 `ping` / `pong` 保活；超时立即关闭并重连。

### 4.2 上传一轮

设备发送 `turn_start`：

```json
{
  "type":"turn_start",
  "turn_id":"<uuid>",
  "audio_format":"wav-pcm-s16le",
  "sample_rate":16000,
  "channels":1,
  "bytes":123456,
  "sha256":"..."
}
```

服务端返回 `turn_ready` 后，设备按最多 4096 字节发送二进制帧，最后发送 `turn_end`。服务端校验成功后回复 `turn_accepted`；校验失败返回可重试错误并删除临时文件。

### 4.3 返回回复

完成 AI 管线后服务端发送 `reply_start`，包含 turn ID、用户转写、助手文本、WAV 大小和 SHA-256；随后发送二进制帧和 `reply_end`。设备写入 `.part`，校验后原子改名为正式 WAV，追加 manifest，再发送 `reply_saved`。只有正式文件完成后才自动播放。

控制帧包含明确的 `type` 和 `turn_id`；二进制帧只允许出现在双方已确认的上传或下载状态中。乱序、超长、未知 turn 或协议版本不匹配会关闭连接，避免错误音频归属。

## 5. SD 卡存储

每轮使用独立目录：

```text
/sdcard/agent/YYYYMMDD/<turn-id>/
  user.wav
  assistant.wav
  turn.json
/sdcard/agent/turns.jsonl
```

`turn.json` 保存协议版本、turn ID、本地时间、服务器时间、两个文件的大小和哈希、转写、回复文本、状态、重试次数及最后错误。`turns.jsonl` 是便于顺序审计的追加索引。

状态按 `recorded → sending → processing → receiving → complete` 演进。写元数据采用临时文件加原子改名。启动时扫描非 complete 轮次：已有 `user.wav` 的轮次重新入队；已有且哈希正确的 `assistant.wav` 不重复下载，直接补发确认。SD 卡不可用或空间不足时禁止开始新录音并在屏幕显示错误。

ESP-IDF 的 FATFS `rename()` 不具备 POSIX 的“覆盖已存在目标”语义，目标存在时会返回
`FR_EXIST`。因此 `turn.json` 和 `assistant.wav` 的发布必须使用可恢复交换：先完整写入并
`fsync` 对应 `.part`，若正式文件存在则先将其改名为 `.bak`，再将 `.part` 改名为正式
文件；第二步失败时恢复 `.bak`。启动扫描读取正式 manifest，正式文件缺失时回退读取
`.bak`，下一次状态写入会重新发布正式文件并清除旧备份。`.part` 和 `.bak` 都不得列入
播放菜单或自动播放。主机回归测试必须模拟 FATFS 的 no-overwrite rename，不能只依赖
macOS/POSIX 文件系统行为。

“所有来往存储到 SD 卡”具体包括所有用户语音、所有助手语音以及每轮可显示的文本和处理结果；TLS 包、心跳等传输层数据不落盘。

## 6. 用户交互与状态

沿用已确认的按键/触屏对讲：

- `REC`：在线或离线均可开始录音。
- `STOP`：结束并保存；在线立即发送，离线显示 `QUEUED`。
- 在线状态依次显示 `ONLINE`、`SENDING`、`THINKING`、`RECEIVING`、`PLAYING`。
- 断线显示 `OFFLINE / saved to SD`，不删除或覆盖录音。
- 收到完整回复后自动播放；播放期间保留暂停/继续和实体音量键。
- 文件菜单同时列出用户和助手语音，便于回放。
- 长按 `MENU` 退出时先完成当前文件写入并安全关闭网络；未发送轮次下次进入后继续。

一轮正在发送、接收或播放时不启动第二轮，避免单任务设备上的 SD、屏幕和 codec 资源竞争。

## 7. 错误处理与安全

- Wi-Fi/WSS：1、2、4、8、16、30 秒退避并加抖动；成功 `ready` 后清零。
- 服务超时：STT、LLM、TTS 各自有超时；失败返回可读错误并保留 turn，设备允许重试。
- 幂等：同一 turn ID 的重复上传最多生成一份服务端回复；完成轮次可重放已有文件。
- 资源限制：单轮默认最多 120 秒和 4 MB；服务端按声明大小限流，设备分块缓冲不超过 8 KB。
- 凭证：仅通过 WSS 传输；令牌按 Endpoint 隔离、可撤销，服务端日志打码。
- SD/SPI2：所有大块 SD I/O 继续用 `RecorderDisplayPause()` / `RecorderDisplayResume()`；不得在 WebSocket 回调或 Wi-Fi 系统任务中写 FATFS。
- 数据库：入站音频、回复音频和消息状态在事务边界内关联；部分失败可由 turn 表恢复。

## 8. 测试与验收

### Agent 后端

测试先行覆盖：

- 设备鉴权成功、失败、撤销和 Endpoint 隔离。
- WebSocket 状态机、大小/哈希校验、非法帧和断线清理。
- 相同 turn ID 的并发和重试只调用一次 STT/LLM/TTS。
- 金山用户只能使用自己的默认助手、上下文和消息。
- Opus 4.8 Provider 选择、STT/TTS 调用、入出音频与消息落库。
- 已完成回复的断线重放。
- 预切换环境仅允许显式硬件白名单，不开启其他生产 Connector。

### 固件

主机测试覆盖协议 reducer、turn manifest、路径/文件恢复、分块校验和网络事件 reducer。随后执行现有 recorder 全套测试与 `idf.py build`。

### Azure 与真机

1. 部署前运行完整后端测试、Alembic upgrade/check、前端测试与构建、敏感信息扫描。
2. 部署 Agent，检查 systemd、Nginx WebSocket 转发、HTTPS health 和日志。
3. 在 Azure 数据库验证金山用户、助手、Opus 4.8、Connector/Endpoint/Identity 的关联，不输出密钥。
4. 写入设备 NVS 凭证、构建并烧录；按本板限制执行物理冷启动。
5. 真机录制一句话，证明 SD 上出现 user WAV；服务端收到、转写并用 Opus 4.8 生成回复；设备收到 assistant WAV、写入 manifest 并自动播放。
6. 断网录音后恢复网络，证明自动补发且服务端没有重复回复。
7. 中断回复下载后重连，证明临时文件不会播放、重试后完整文件可播放。
8. 返回选择器和再次进入 AI 录音模式，验证无 SPI2 断言、栈保护故障或重复重启。

## 9. 非目标

- 不做持续监听、唤醒词、边录边传、全双工打断或回声消除。
- 不开启小鹿、微信、邮件或定时主动发送。
- 不迁移小鹿的历史消息、记忆或生产用户数据。
- 不把 SD 卡暴露为 USB 大容量设备。
