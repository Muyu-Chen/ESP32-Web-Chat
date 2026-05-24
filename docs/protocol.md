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
- 回放 `id > since_id` 的服务端缓存消息。
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
  "from": "user-uuid"
}
```

用于回应服务端心跳。

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
