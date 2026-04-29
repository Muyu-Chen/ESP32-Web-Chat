# ESP32-Web-Chat

English: ESP32-Web-Chat is a standalone local chat room built on ESP32. The device creates its own Wi-Fi hotspot, and anyone connected to that network can open the web page and join the conversation.

中文：ESP32-Web-Chat 是一个基于 ESP32 的离线聊天室。设备会创建自己的 Wi-Fi 热点，连接到该网络的用户可以直接通过网页加入聊天。

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

- Wi-Fi SSID: `ESPChat` (defined in `main/main.c`)
- Wi-Fi password: `esp-chat` (defined in `main/main.c`)
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

2. Configure Wi-Fi

   Open `main/main.c` and adjust the SSID and password if needed:

   ```c
   #define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
   #define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
   ```

3. Flash the firmware

   - Connect the ESP32 to your computer
   - Select the correct board and serial port
   - Build and upload the firmware

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
│   ├── main.c
│   ├── mount.c
│   ├── favicon.ico
│   └── src/
│       ├── index.html
│       ├── style.css
│       └── script.js
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions_example.csv
├── LICENSE
├── README.md
└── README-CN.md
```

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

- Verify the SSID and password in `main/main.c`:

  ```c
  #define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
  #define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
  ```

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

- Make Wi-Fi SSID and password configurable without editing source code
- Add persistent storage for message history
- Improve reconnect behavior and user-facing diagnostics
- Add fuller ESP-IDF build and flashing documentation

## License

This project is licensed under the Apache 2.0 License.
