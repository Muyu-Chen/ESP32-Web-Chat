# ESP32-Web-Chat

ESP32-Web-Chat turns a single ESP32 board into a pocket-sized, offline chat hub. Power it on, connect to the `ESPChat` hotspot, and every phone or laptop nearby can open a browser and start talking in real time, with no router, cloud account, or internet connection required.

It is built for maker demos, classrooms, field teams, events, and any place where a lightweight local message board is more useful than another app install. The ESP32 hosts the Wi-Fi network, serves the responsive web UI, redirects captive-portal requests, and keeps the conversation flowing over WebSockets.

ESP32-Web-Chat 可以把一块 ESP32 变成一个随身携带的离线聊天室。只要给开发板供电，它就会创建 `ESPChat` Wi-Fi 热点；手机或电脑连上后，打开浏览器即可实时聊天，不依赖路由器、云服务或互联网。

它适合 Maker 展示、课堂互动、户外协作、活动现场留言板，以及任何需要“本地、轻量、马上可用”沟通空间的场景。ESP32 同时负责 Wi-Fi 热点、网页服务、Captive Portal 自动跳转和 WebSocket 实时消息转发，让一块小板子也能变成完整的局域网聊天室。

For the Chinese version, see [README-CN.md](README-CN.md).

## Features

- Wi-Fi hotspot mode: the ESP32 creates an independent wireless network
- Real-time WebSocket messaging with low latency
- Responsive web interface for multiple devices
- Cross-platform access from any device with Wi-Fi and a browser
- Message history buffer for the latest 100 messages
- Heartbeat checks to detect and clean up disconnected clients
- DNS redirection to send all domain requests to the chat page
- Zero-configuration setup for quick use

## Specifications

### Hardware Requirements

- ESP32 development board with Wi-Fi support
- USB cable for flashing
- 5V power supply (optional, for standalone use)

### Software Requirements

- ESP-IDF development environment
- Modern web browser (Chrome / Firefox / Safari / Edge)

### Default Parameters

- Wi-Fi SSID: `ESPChat` (stored in NVS from the browser Settings panel)
- Wi-Fi password: `esp-chat` (stored in NVS from the browser Settings panel)
- Default settings admin password: `admin` (change it after first boot)
- Default IP address: `192.168.4.1`
- Maximum WebSocket clients: 10
- Maximum AP station connections: 8
- Message history size: 100 messages
- Heartbeat interval: 30 seconds

## Installation

1. Clone the repository

   ```bash
   git clone https://github.com/your-username/esp32-chat.git
   cd esp32-chat
   ```

2. Flash the firmware

   - Connect the ESP32 to your computer
   - Select the correct board and serial port
   - Build and upload the firmware

3. Configure Wi-Fi on first boot

   Connect to the default `ESPChat` hotspot. Open `Settings` from the chat UI, enter the admin password, and change the Wi-Fi SSID, password, channel, and settings password. Settings are stored in ESP32 NVS and apply after saving and rebooting.

4. Open the chat room

   - Check the serial monitor for the IP address
   - Open that address in a browser

## Connection Steps

1. Power on the device

   - Supply power to the ESP32
   - Wait about 10 seconds for startup

2. Connect to Wi-Fi

   - Find `ESPChat` in the Wi-Fi list on your phone or computer
   - Enter the password: `esp-chat`

3. Open the chat page

   - Visit `http://192.168.4.1` in a browser
   - Or open any URL and let the captive redirect take you to the chat page

4. Start chatting

   - Enter a nickname
   - Send messages and chat with other users in real time

## Project Structure

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

## Documentation

Start with [docs/overview.md](docs/overview.md) for the current architecture and maintenance map.

| Document | Purpose |
| --- | --- |
| [Overview](docs/overview.md) | Project layout, module responsibilities, and where to change features |
| [Architecture](docs/architecture.md) | Startup flow, shared context, FreeRTOS tasks, and locking |
| [Protocol](docs/protocol.md) | HTTP API, WebSocket messages, errors, and history recovery |
| [Maintenance](docs/maintenance.md) | How to add or remove common features |
| [Build and Flash](docs/build-and-flash.md) | ESP-IDF build, flashing, partitions, and troubleshooting |

## Configuration

| Parameter | Default | Description |
| --- | --- | --- |
| Server port | 80 | HTTP server port |
| Max clients | 10 | Maximum simultaneous chat users |
| Message buffer | 100 | Number of messages kept in history |
| Heartbeat interval | 30 seconds | Client liveness check period |

## Troubleshooting

### Common Issues

**Q: I cannot connect to the Wi-Fi network**

- Verify the SSID and password saved in the browser `Settings` panel
- If you forget the new credentials, erase NVS or reflash the firmware to restore defaults

- Make sure the signal is strong enough
- Restart the ESP32

**Q: The web page does not open**

- Confirm that the device is connected to the ESP32 Wi-Fi network
- Check that the IP address is correct
- Try disabling the local firewall temporarily

**Q: Messages fail to send**

- Check the network connection
- Refresh the page and try again
- Inspect serial output for runtime errors

## Contributing

Contributions are welcome. A typical workflow looks like this:

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/AmazingFeature`
3. Commit your changes: `git commit -m 'Add some AmazingFeature'`
4. Push the branch: `git push origin feature/AmazingFeature`
5. Open a Pull Request

## TODO

- [x] Add NVS-backed browser settings for Wi-Fi SSID, password, and channel so deployments do not require source edits
- [x] Build a small admin-password-protected setup panel for changing hotspot credentials and the settings password from the browser
- [x] Assign message IDs and timestamps on the ESP32, validate incoming JSON fields, clamp payload sizes, and return clear errors for malformed WebSocket frames
- [x] Implement `since_id` history sync so reconnecting clients only replay missed messages instead of receiving the whole ring buffer
- [ ] Add optional persistent history in NVS, SPIFFS, or SD card storage with retention controls and a clear-room action
- [x] Complete online user presence basics with nickname registration, online list updates, and heartbeat-driven cleanup visible in the UI
- [ ] Add optional join and leave system notices
- [ ] Finish private chat and group chat flows, or hide unfinished conversation-list UI until the backend protocol fully supports it
- [ ] Improve reconnect UX with exponential backoff, connection-state badges, an offline send queue, and duplicate-message prevention across reloads
- [ ] Harden captive portal support for iOS, Android, and Windows detection URLs, safer DNS response bounds checking, and clearer fallback instructions
- [ ] Optimize front-end asset size and caching headers to reduce RAM and flash pressure and speed up first load on constrained boards
- [ ] Add security and privacy guidance covering local-only scope, WPA2 password choices, setup-page protection, and the fact that no cloud storage is used

## License

This project is licensed under the Apache 2.0 License.
