# ESP32-Chat  

此项目是基于esp-32开发的服务器、客户端一体聊天网站。访问服务器ip即可直接加入聊天室。无需外部互联网，适用于无网环境中的聊天，包括但不限于飞机、户外、封闭的教室等等情况。请注意，若在飞机上使用，请使用民航局要求的射电设备。  

## 🚀 功能特性

- 🔥 **WiFi热点模式** - ESP32创建独立WiFi网络
- 💬 **实时WebSocket通信** - 低延迟消息传输
- 🖥️ **响应式Web界面** - 支持多设备同时聊天
- 📱 **跨平台支持** - 任何有WiFi和浏览器的设备都可连接
- 🔄 **消息历史** - 自动保存最近100条消息
- ❤️ **心跳检测** - 自动检测并清理断开的连接
- 🌐 **DNS劫持** - 任何域名都会重定向到聊天界面
- ⚡ **零配置** - 开箱即用，无需复杂设置

## 📋 系统规格

### 硬件要求
- ESP32开发板 (支持WiFi功能)
- USB数据线 (用于烧录程序)
- 5V电源 (可选，用于独立运行)

### 软件要求
- ESP-IDF 开发环境（vscode中）
- 或 Arduino IDE + ESP32插件
- 现代Web浏览器 (Chrome/Firefox/Safari/Edge)

### 系统参数
- **WiFi网络名称**: `ESPChat` 位于src/main.c中
- **WiFi密码**: `esp-chat` 位于src/main.c中
- **访问IP**: `192.168.4.1`（使用“登陆到网络”功能实现自动跳转）
- **最大连接数**: 10个WebSocket连接，可在esp-idf中修改
- **消息缓存**: 100条历史消息
- **心跳间隔**: 30秒


## 🛠️ 安装步骤

1. **克隆项目**
   ```bash
   git clone https://github.com/your-username/esp32-chat.git
   cd esp32-chat
   ```

2. **配置WiFi**
   - 打开配置文件
   - 修改WiFi名称和密码
   ```cpp
      #define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
      #define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
   ```

3. **上传代码**
   - 连接ESP32到电脑
   - 选择正确的开发板和端口
   - 编译并上传代码

4. **访问聊天室**
   - 打开串口监视器查看IP地址
   - 在浏览器中访问显示的IP地址

## 连接步骤

1. **启动设备**
   - 给ESP32供电
   - 等待约10秒完成启动

2. **连接WiFi**
   - 在手机/电脑WiFi列表中找到 `ESPChat`
   - 输入密码：`esp-chat`

3. **打开聊天室**
   - 打开浏览器访问：`http://192.168.4.1`
   - 或访问任意网址（会自动跳转）

4. **开始聊天**
   - 输入昵称
   - 发送消息与其他用户实时交流

## 📁 项目结构

```
esp32-chat/
├── src/
│   ├── main.cpp          # 主程序代码
│   ├── wifi_manager.cpp  # WiFi管理
│   └── web_server.cpp    # Web服务器
├── data/
│   ├── index.html        # 聊天界面
│   ├── style.css         # 样式文件
│   └── script.js         # 前端脚本
├── lib/                  # 第三方库
├── platformio.ini        # 项目配置
└── README.md
```

## ⚙️ 配置选项

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 服务器端口 | 80 | Web服务器端口 |
| 最大连接数 | 10 | 同时在线用户数 |
| 消息缓存 | 50 | 历史消息保存数量 |

## 🔧 故障排除

### 常见问题

**Q: 无法连接WiFi**
- 检查WiFi名称和密码是否正确，位于src/main.c中
  ```c
    #define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
    #define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
  ```
- 确认WiFi信号强度足够
- 重启ESP32设备

**Q: 网页无法访问**
- 确认设备已成功连接WiFi
- 检查IP地址是否正确
- 尝试关闭防火墙

**Q: 消息发送失败**
- 检查网络连接
- 刷新网页重试
- 查看串口输出错误信息

## 🤝 贡献指南

欢迎贡献代码！请遵循以下步骤：

1. Fork 项目
2. 创建功能分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

## 📝 更新日志

### v1.0.0 (2025-10-01)
- 🎉 首次发布
- ✨ 基础聊天功能
- 🌐 Web界面支持
- 📡 WiFi连接管理

## 📄 许可证

本项目采用 Apache 2.0 许可证


⭐ 如果这个项目对你有帮助，请给个星标支持！
