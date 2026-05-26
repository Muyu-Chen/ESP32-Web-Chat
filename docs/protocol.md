# Protocol

本文档记录 ESP32-Web-Chat 当前对外协议。结构重构后，协议保持兼容。

## HTTP API

| Path | Method | Response |
| --- | --- | --- |
| `/` | `GET` | HTML 页面 |
| `/style.css` | `GET` | CSS |
| `/script.js` | `GET` | JavaScript |
| `/favicon.ico` | `GET` | favicon |
| `/api/settings` | `GET` | 当前设置摘要 |
| `/api/settings` | `POST` | 保存设置 |
| `/*` | `GET` | 302 到 ESP32 AP 地址 |

### GET `/api/settings`

```json
{
  "ok": true,
  "ssid": "ESPChat",
  "channel": 1,
  "passwordSet": true,
  "maxSsidLength": 32,
  "minPasswordLength": 8,
  "maxPasswordLength": 63,
  "minAdminPasswordLength": 4
}
```

### POST `/api/settings`

```json
{
  "adminPassword": "admin",
  "ssid": "ESPChat",
  "password": "esp-chat",
  "openNetwork": false,
  "channel": 1,
  "newAdminPassword": "",
  "reboot": true
}
```

成功返回：

```json
{
  "ok": true,
  "rebootRequired": true,
  "restarting": true,
  "message": "Settings saved. ESP32 is restarting."
}
```

错误返回：

```json
{
  "ok": false,
  "code": "bad_ssid",
  "message": "SSID must be 1 to 32 bytes"
}
```

## WebSocket

WebSocket 路径是 `/ws`，只支持文本 JSON 帧。

通用规则：

- 除 `join` 和 `pong` 外，客户端必须先完成 `join`，后续消息的 `from` 必须与已注册身份一致。
- 客户端可在任意消息中携带 `timestamp` 作为设备时间同步样本；服务端发送和入库的消息时间戳由 ESP32 统一生成。
- ESP32 在 RTC 时间无效时使用在线客户端时间多数派：至少三分之二有效时间样本在 120 秒内误差一致时，采用该多数派时间；否则回退到设备运行秒数。
- 非文本帧、非法 JSON、未知类型、非法身份、超长 payload 或字段越界都会返回 `error` 消息。

### 客户端发送 `join`

```json
{
  "type": "join",
  "from": "user-uuid",
  "name": "Alice",
  "timestamp": 1710000000,
  "since_id": 123
}
```

行为：

- 绑定 socket 与用户身份。
- `since_id` 可省略；存在时必须是 `0..9007199254740991` 的整数，否则返回 `bad_since_id`。
- 回放 `id > since_id` 的服务端缓存消息；如果 `since_id` 已经大于当前最新消息，则不重复回放。
- 返回 `historyInfo`。
- 返回并广播 `onlineUsers`。

### 客户端发送 `text`

```json
{
  "type": "text",
  "from": "user-uuid",
  "name": "Alice",
  "to": {
    "all": true,
    "users": []
  },
  "data": "hello",
  "timestamp": 1710000000
}
```

服务端会覆盖客户端传入的 `id` 和 `timestamp`，生成正式值后广播。

### 客户端发送 `newGroup`

```json
{
  "type": "newGroup",
  "from": "user-uuid",
  "name": "Alice",
  "groupId": "group-xxx",
  "groupName": "Alice, Bob",
  "to": {
    "all": false,
    "users": ["user-a", "user-b"]
  },
  "data": "Alice created Alice, Bob",
  "timestamp": 1710000000
}
```

`newGroup` 只表示创建群聊。群聊里的普通消息仍是 `text`，额外携带 `groupId` 和 `groupName`。

### 客户端发送 `getOnlineUser`

```json
{
  "type": "getOnlineUser",
  "from": "user-uuid",
  "name": "Alice",
  "timestamp": 1710000000
}
```

服务端向请求者单播 `onlineUsers`。

### 客户端发送 `pong`

```json
{
  "type": "pong",
  "from": "user-uuid",
  "timestamp": 1710000000
}
```

用于回应服务端心跳。`timestamp` 推荐携带，用于服务端时间多数派同步；旧客户端不携带也兼容。

### 历史恢复

请求更老历史：

```json
{
  "type": "historyRequest",
  "from": "user-a",
  "name": "Alice",
  "requestId": "hist-xxxx",
  "restore_before_id": 101
}
```

响应更老历史：

```json
{
  "type": "historyResponse",
  "from": "user-b",
  "name": "Bob",
  "requestId": "hist-xxxx",
  "to": {
    "all": false,
    "users": ["user-a"]
  },
  "message": {
    "type": "text",
    "from": "user-c",
    "name": "Carol",
    "to": {
      "all": false,
      "users": ["user-a", "user-c"]
    },
    "data": "old message",
    "id": 80,
    "timestamp": 1709990000
  }
}
```

服务端会校验历史消息对目标用户可见，并且只转发给 `to.users`。

## 服务端发送

### `ping`

```json
{"type":"ping"}
```

### `onlineUsers`

```json
{
  "type": "onlineUsers",
  "from": "server",
  "timestamp": 1710000000,
  "to": {
    "all": true,
    "users": ["user-a", "user-b"]
  },
  "data": [
    {"id": "user-a", "name": "Alice"},
    {"id": "user-b", "name": "Bob"}
  ]
}
```

`onlineUsers.data` 只包含已完成 `join` 且连接仍存活的 WebSocket 用户。连接关闭、重复登录替换、心跳超时或发送失败清理后，服务端会广播最新列表。

### `historyInfo`

```json
{
  "type": "historyInfo",
  "from": "server",
  "timestamp": 1710000000,
  "to": {
    "all": true,
    "users": []
  },
  "boot_start_id": 121,
  "current_id": 130,
  "earliest_id": 31,
  "latest_id": 130,
  "restore_before_id": 31,
  "count": 100,
  "capacity": 100,
  "has_more_before": true
}
```

### `error`

```json
{
  "type": "error",
  "from": "server",
  "code": "bad_json",
  "data": "Invalid JSON object",
  "timestamp": 1710000000
}
```

常见错误码包括 `bad_json`、`bad_type`、`unknown_type`、`not_joined`、`bad_identity`、`bad_since_id`、`bad_target`、`payload_too_large`。
