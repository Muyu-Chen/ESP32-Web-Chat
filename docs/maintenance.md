# Maintenance Guide

本文档回答“新增功能去哪里加，删除功能去哪里删”。

## 新增 WebSocket 消息类型

改动入口：

- `main/src/chat/protocol.c`：新增类型分发和校验。
- `main/src/chat/history.c`：如果消息需要重连后可见，需要接入入库和回放。
- `main/src/server/websocket_server.c`：如果需要特殊帧处理或发送策略，改这里。
- `main/web/js/script.js`：新增发送、接收、渲染或本地缓存逻辑。

默认策略：

- 控制类消息不入历史，例如正在输入。
- 聊天类消息应由服务端分配 `id` 和 `timestamp`。
- 需要定向发送时，不要走普通 `chat_ws_broadcast()`。

## 新增设置项

必须同步修改：

- `main/include/chat_types.h`：扩展 `chat_settings_t`。
- `main/include/chat_config.h`：如需编译期默认值或长度限制，放这里。
- `main/Kconfig.projbuild`：新增 menuconfig 项。
- `main/src/common/settings.c`：默认值、NVS 读取、NVS 保存、合法性校验。
- `main/src/server/http_server.c`：`GET/POST /api/settings`。
- `main/web/index.html`、`main/web/js/script.js`、`main/web/css/style.css`：设置页 UI。

如果设置需要立即生效，还要修改对应应用模块，例如 Wi-Fi 参数在 `network/softap.c`。

## 新增静态资源

必须同步修改：

- 把文件放到 `main/web` 的合适子目录。
- 在 `main/CMakeLists.txt` 的 `EMBED_FILES` 中加入文件。
- 在 `main/src/server/http_server.c` 中增加 HTTP handler 和路由。
- 在 `main/web/index.html` 中引用资源。

注意：ESP32 不会自动扫描 `web/` 目录，必须显式写入 CMake。

## 添加服务端消息持久化

当前只持久化消息 ID，不持久化正文。要做真正服务端历史，应新增或扩展：

- `main/src/storage/`：新增 SPIFFS、SDCard 或 NVS 消息正文存储模块。
- `main/src/storage/mount.c`：接入挂载流程。
- `main/src/main.c`：启动时挂载存储并加载历史。
- `main/src/chat/history.c`：写入消息、分页读取、历史边界计算。
- `docs/protocol.md`：记录新增的历史查询能力。

不要只改 `message_id_store.c`，它只负责数字 ID。

## 删除群聊

设备端需要删：

- `chat/protocol.c` 中 `newGroup` 校验与分发。
- 历史恢复校验中对 `newGroup` 的支持。

前端需要删：

- `main/web/index.html` 中创建群聊按钮和弹窗。
- `main/web/js/script.js` 中 `createGroup()`、群聊会话归属、群消息过滤和渲染。
- `main/web/css/style.css` 中群聊弹窗样式。

## 删除私聊

设备端需要重新定义 `to` 的含义：

- `chat/protocol.c` 中目标校验和历史恢复可见性校验。

前端需要删：

- 私聊会话生成和过滤。
- 在线用户列表中的私聊入口。
- `messageTargetForCurrentConversation()` 中 private 分支。

## 删除历史恢复

保留重连补拉时，只保留：

- `join`
- `since_id`
- `chat_history_send_to_client()`

删除跨设备恢复时，需要清理：

- `historyRequest`
- `historyResponse`
- `historyInfo` 的恢复按钮依赖字段
- 前端 Recover 按钮、状态栏、恢复导入逻辑

## 删除设置页

只删除 UI：

- 删除 `main/web` 中设置弹窗和 JS 逻辑。
- 保留 `/api/settings` 供调试或其他客户端调用。

彻底删除功能：

- 删除 `common/settings.c` 的 NVS 设置读写。
- 删除 `server/http_server.c` 中 `/api/settings`。
- `network/softap.c` 改回只使用编译期配置。

## 修改目录或文件名

每次移动 C 文件后检查：

- `main/CMakeLists.txt` 的 `SRCS`。
- `#include` 路径。
- `docs/overview.md` 的目录图。
- README 中的项目结构。

每次移动前端资源后检查：

- `main/CMakeLists.txt` 的 `EMBED_FILES`。
- `server/http_server.c` 中 `_binary_*` 符号。
- 浏览器对外访问路径是否仍兼容。
