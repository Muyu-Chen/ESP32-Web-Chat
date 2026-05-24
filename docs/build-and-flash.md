# Build And Flash

本文档记录当前工程的构建、烧录和常见问题。

## 推荐构建命令

如果当前 shell 没有加载 ESP-IDF，可使用显式环境命令：

```bash
cd /Users/muyu/Documents/Daily/ESP32-Web-Chat

ESP_IDF_VERSION=6.0.1 \
IDF_PYTHON_ENV_PATH=/Users/muyu/.espressif/tools/python/v6.0.1/venv \
IDF_TOOLS_PATH=/Users/muyu/.espressif/tools \
PATH="/Users/muyu/.espressif/tools/cmake/4.0.3/CMake.app/Contents/bin:/Users/muyu/.espressif/tools/ninja/1.12.1:/Users/muyu/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH" \
/Users/muyu/.espressif/tools/python/v6.0.1/venv/bin/python3 \
/Users/muyu/.espressif/v6.0.1/esp-idf/tools/idf.py -G Ninja build
```

如果已经加载 ESP-IDF 环境，也可以直接运行：

```bash
idf.py build
```

## 烧录

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

端口请按实际设备替换。macOS 上常见端口类似 `/dev/cu.usbserial-*` 或 `/dev/cu.SLAB_USBtoUART`。

## 分区

项目使用 `sdkconfig.defaults` 指向 `partitions_example.csv`。

当前分区包含：

- `nvs`：保存 Wi-Fi 设置、管理员密码、消息 ID 状态。
- `factory`：应用固件。
- `storage`：预留 SPIFFS 数据分区，目前主流程未使用。

## 构建检查点

重构目录后重点检查：

```bash
rg "_binary_" main/src main/include
rg "CONFIG_CHAT_|MAX_CLIENTS|MAX_MESSAGES" main/src main/include
```

成功构建后应生成：

```text
build/esp32-chat.bin
```

## 常见问题

### `idf.py: command not found`

说明当前 shell 没有加载 ESP-IDF 环境。使用本文档顶部的显式环境命令，或先 source ESP-IDF 的 export 脚本。

### `PermissionError: Operation not permitted` 来自 `psutil`

构建工具在 macOS 上查询进程信息时可能被沙箱拦截。需要在允许系统进程查询的环境下重新运行构建。

### `ESP_ROM_ELF_DIR environment variable is not defined`

这是生成 gdbinit 时的警告来源之一。当前构建仍可继续完成；如需完整调试符号，运行 ESP-IDF install/export 脚本补齐环境。

### 静态资源无法访问

检查：

- 文件是否在 `main/web`。
- `main/CMakeLists.txt` 是否加入 `EMBED_FILES`。
- `server/http_server.c` 是否注册了对应 URI。
- `_binary_*` 符号是否与文件名一致。
