# ESP32-Web-Chat Overview

ESP32-Web-Chat 是一个运行在 ESP32 上的离线局域网聊天室。ESP32 启动后创建 Wi-Fi SoftAP，浏览器通过 HTTP 加载嵌入式前端页面，再通过 WebSocket 与设备实时通信。

## 项目目标

- 在无互联网环境下提供本地聊天室。
- 保持手机和电脑浏览器即可访问，不依赖 App。
- 支持群聊、私聊、在线用户、最近历史补拉、跨设备历史恢复。
- 支持浏览器设置页修改热点 SSID、密码、信道和管理员密码。
- 将设备端拆成可维护模块，方便后续新增、删除和替换功能。

## 目录结构

```text
main/
├── include/
│   ├── app_context.h
│   ├── chat_config.h
│   ├── chat_types.h
│   ├── common/
│   ├── network/
│   ├── server/
│   ├── chat/
│   └── storage/
├── src/
│   ├── main.c
│   ├── common/
│   ├── network/
│   ├── server/
│   ├── chat/
│   └── storage/
├── web/
│   ├── index.html
│   ├── css/style.css
│   ├── js/script.js
│   └── assets/favicon.ico
├── CMakeLists.txt
└── Kconfig.projbuild
```

## 模块职责

| 模块 | 目录 | 职责 |
| --- | --- | --- |
| 启动入口 | `main/src/main.c` | ESP-IDF `app_main()`，只负责初始化和启动编排 |
| 共享上下文 | `main/include/app_context.h` | 保存跨模块共享状态，如 HTTPD、客户端槽、消息缓存、锁、设置 |
| 配置与类型 | `chat_config.h`、`chat_types.h` | 集中宏、长度限制和跨模块结构体 |
| common | `main/src/common` | 设置读写、通用字符串/JSON/时间工具 |
| network | `main/src/network` | SoftAP、静态 IP、DHCP、DNS 劫持 |
| server | `main/src/server` | HTTP 静态资源、设置 API、WebSocket 帧收发 |
| chat | `main/src/chat` | 在线用户、心跳、消息缓存、业务协议、历史恢复 |
| storage | `main/src/storage` | 消息 ID 持久化和未来 SPIFFS/SDCard 挂载能力 |
| web | `main/web` | 编译进固件的前端页面、样式和脚本 |

## 功能定位

| 要改的功能 | 优先查看 |
| --- | --- |
| Wi-Fi 热点参数应用 | `network/softap.c`、`common/settings.c` |
| Captive Portal / DNS 劫持 | `network/dns_server.c`、`server/http_server.c` |
| HTTP 静态资源 | `server/http_server.c`、`main/CMakeLists.txt`、`main/web` |
| 设置页后端 | `common/settings.c`、`server/http_server.c` |
| WebSocket 收发 | `server/websocket_server.c` |
| 新增消息类型 | `chat/protocol.c` |
| 在线用户/心跳 | `chat/sessions.c` |
| 最近消息缓存/历史边界 | `chat/history.c` |
| 消息 ID 持久化 | `storage/message_id_store.c` |
| 前端 UI 和本地状态 | `main/web/index.html`、`main/web/js/script.js`、`main/web/css/style.css` |

## 兼容性约定

- HTTP 外部路径保持 `/`、`/style.css`、`/script.js`、`/favicon.ico`、`/api/settings`。
- WebSocket 外部路径保持 `/ws`。
- NVS namespace 保持 `chatcfg` 和 `chatmsg`。
- `Kconfig.projbuild` 的配置项名称保持不变。
- 普通 `text` 和 `newGroup` 仍广播给所有 WebSocket 客户端，由前端按 `to` 字段过滤显示。

## 当前边界

- ESP32 当前只持久化消息 ID，不持久化消息正文。
- 服务端只保留最近 `CONFIG_CHAT_MESSAGE_HISTORY_SIZE` 条消息。
- 更老的历史恢复依赖其他在线浏览器的 `localStorage`。
- `storage/mount.c` 是预留挂载能力，当前主流程没有调用。
