# ESP32-Web-Chat

ESP32-Web-Chat 可以把一块 ESP32 变成一个随身携带的离线聊天室。只要给开发板供电，它就会创建 `ESPChat` Wi-Fi 热点；手机或电脑连上后，打开浏览器即可实时聊天，不依赖路由器、云服务或互联网。

它适合 Maker 展示、课堂互动、户外协作、活动现场留言板，以及任何需要“本地、轻量、马上可用”沟通空间的场景。ESP32 同时负责 Wi-Fi 热点、网页服务、Captive Portal 自动跳转和 WebSocket 实时消息转发，让一块小板子也能变成完整的局域网聊天室。

英文说明请见 [README.md](README.md)。

## 功能特性

- Wi-Fi 热点模式：ESP32 创建独立 Wi-Fi 网络
- 实时 WebSocket 通信：低延迟消息传输
- 响应式 Web 界面：支持多设备同时聊天
- 跨平台支持：任何具备 Wi-Fi 和浏览器的设备都可连接
- 消息历史：自动保存最近 100 条消息
- 心跳检测：自动检测并清理断开的连接
- DNS 重定向：任意域名请求都会跳转到聊天界面
- 零配置：开箱即用，无需复杂设置

## 系统规格

### 硬件要求

- ESP32 开发板（支持 Wi-Fi 功能）
- USB 数据线（用于烧录程序）
- 5V 电源（可选，用于独立运行）

### 软件要求

- ESP-IDF 开发环境
- 现代 Web 浏览器（Chrome / Firefox / Safari / Edge）

### 系统参数

- Wi-Fi 网络名称：`ESPChat`（可在网页 Settings 中保存到 NVS）
- Wi-Fi 密码：`esp-chat`（可在网页 Settings 中保存到 NVS）
- 设置页默认管理员密码：`admin`（首次使用后建议立即修改）
- 默认访问 IP：`192.168.4.1`
- 最大 WebSocket 连接数：10
- AP 最大接入设备数：8
- 消息缓存：100 条历史消息
- 心跳间隔：30 秒

## 安装步骤

1. 克隆项目

   ```bash
   git clone https://github.com/your-username/esp32-chat.git
   cd esp32-chat
   ```

2. 烧录程序

   - 将 ESP32 连接到电脑
   - 选择正确的开发板和端口
   - 编译并上传代码

3. 首次配置 Wi-Fi

   首次启动后先连接默认热点 `ESPChat`，进入聊天页的 `Settings`，输入管理员密码后即可修改 Wi-Fi 名称、密码、信道和设置页管理员密码。设置会写入 ESP32 的 NVS，保存并重启后生效。

4. 访问聊天室

   - 打开串口监视器查看 IP 地址
   - 在浏览器中访问显示的地址

## 连接步骤

1. 启动设备

   - 给 ESP32 供电
   - 等待约 10 秒完成启动

2. 连接 Wi-Fi

   - 在手机或电脑的 Wi-Fi 列表中找到 `ESPChat`
   - 输入密码：`esp-chat`

3. 打开聊天室

   - 在浏览器中访问 `http://192.168.4.1`
   - 或访问任意网址（会自动跳转）

4. 开始聊天

   - 输入昵称
   - 发送消息，与其他用户实时交流

## 项目结构

```text
ESP32-Web-Chat/
├── main/
│   ├── include/
│   ├── src/
│   │   ├── main.c
│   │   ├── common/
│   │   ├── network/
│   │   ├── server/
│   │   ├── chat/
│   │   └── storage/
│   ├── web/
│   │   ├── index.html
│   │   ├── css/
│   │   ├── js/
│   │   └── assets/
│   ├── CMakeLists.txt
│   └── Kconfig.projbuild
├── docs/
│   ├── overview.md
│   ├── architecture.md
│   ├── protocol.md
│   ├── maintenance.md
│   └── build-and-flash.md
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions_example.csv
├── LICENSE
├── README.md
└── README-CN.md
```

## 文档

代码结构和后续维护说明请从 [docs/overview.md](docs/overview.md) 开始阅读。

| 文档 | 用途 |
| --- | --- |
| [Overview](docs/overview.md) | 项目布局、模块职责、功能修改入口 |
| [Architecture](docs/architecture.md) | 启动流程、共享上下文、FreeRTOS 任务和锁 |
| [Protocol](docs/protocol.md) | HTTP API、WebSocket 消息、错误格式和历史恢复 |
| [Maintenance](docs/maintenance.md) | 新增或删除常见功能的维护路径 |
| [Build and Flash](docs/build-and-flash.md) | ESP-IDF 构建、烧录、分区和排错 |

## 配置选项

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| 服务器端口 | 80 | Web 服务器端口 |
| 最大连接数 | 10 | 同时在线用户数 |
| 消息缓存 | 100 | 历史消息保存数量 |
| 心跳间隔 | 30 秒 | 连接存活检测周期 |

## 故障排除

### 常见问题

**Q: 无法连接 Wi-Fi**

- 检查网页 `Settings` 中保存的 Wi-Fi 名称和密码是否正确
- 如果忘记新密码，可擦除 NVS 或重新烧录后恢复默认配置

- 确认 Wi-Fi 信号强度足够
- 重启 ESP32 设备

**Q: 网页无法访问**

- 确认设备已成功连接到 Wi-Fi
- 检查 IP 地址是否正确
- 尝试关闭本机防火墙后再访问

**Q: 消息发送失败**

- 检查网络连接
- 刷新网页后重试
- 查看串口输出中的错误信息

## 贡献指南

欢迎贡献代码，请按以下步骤进行：

1. Fork 本项目
2. 创建功能分支：`git checkout -b feature/AmazingFeature`
3. 提交更改：`git commit -m 'Add some AmazingFeature'`
4. 推送分支：`git push origin feature/AmazingFeature`
5. 创建 Pull Request

## TODO

- [x] 增加基于 NVS 的后台设置，让 Wi-Fi 名称、密码和信道无需改源码即可调整
- [x] 提供在线好友列表
- [ ] 支持私聊和新建群聊
- [ ] 优化重连体验，加入指数退避、连接状态标识、离线发送队列，以及刷新后的重复消息防护
- [ ] 类邮件方式发送，消息对所有客户端发送，但客户端只显示 `to.users` 中包含自己的消息
- [x] 增加受管理员密码保护的浏览器设置页，用于修改热点凭据和设置页密码
- [x] 由 ESP32 统一分配消息 ID 与时间戳，校验传入 JSON 字段，限制消息体大小，并对异常 WebSocket 帧返回清晰错误
- [x] 实现基于 `since_id` 的历史同步，让重连客户端只补拉漏掉的消息，而不是每次接收完整环形缓存
- [ ] 支持将消息历史可选持久化到 NVS、SPIFFS 或 SD 卡，并提供保留策略与清空聊天室操作
- [x] 完善在线用户状态基础能力，包括昵称注册、在线列表更新，以及由心跳清理驱动的可视化状态变化
- [ ] 增加可选的加入/离开系统提示
- [ ] 完成私聊与群聊流程，或在后端协议完全支持前隐藏尚未完成的会话列表 UI
- [ ] 优化重连体验，加入指数退避、连接状态标识、离线发送队列，以及刷新后的重复消息防护
- [ ] 强化 Captive Portal 兼容性，覆盖 iOS、Android、Windows 的检测 URL，增加 DNS 响应边界检查，并补充手动访问兜底说明
- [ ] 优化前端资源体积与缓存头，降低 RAM/Flash 压力，并提升受限开发板上的首次加载速度
- [ ] 增加安全与隐私说明，覆盖本地离线范围、WPA2 密码建议、设置页保护，以及不使用云端存储的边界

## 许可证

本项目采用 Apache 2.0 许可证。
