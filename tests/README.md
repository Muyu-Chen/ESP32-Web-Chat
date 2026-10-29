# Host Regression Tests

These checks run without an ESP32, network connections or flashing. They require
Node.js, a C compiler supporting AddressSanitizer/UndefinedBehaviorSanitizer and
the cJSON sources downloaded by the normal ESP-IDF build.

Run from the repository root:

```sh
node --test tests/client-*.test.mjs

for test_source in tests/host/*_regression.c; do
  cc -std=c11 -g -fsanitize=address,undefined \
    -I tests/host/include -I main/include \
    -I managed_components/espressif__cjson/cJSON \
    "$test_source" \
    main/src/chat/protocol.c main/src/chat/history.c \
    main/src/chat/sessions.c main/src/common/utils.c \
    managed_components/espressif__cjson/cJSON/cJSON.c \
    -lm -o /tmp/esp-chat-regression || exit 1
  /tmp/esp-chat-regression || exit 1
done
```

The C tests execute the real protocol, history, session and JSON parsing code.
Small FreeRTOS/HTTP/NVS doubles let the tests exercise successful error responses,
heartbeat scheduling, fd reuse, queue failures and ring-buffer replay. The doubles
assert that sends and closes run on the HTTP task and outside both mutexes.
The transport suite also compiles the production socket sender with deterministic
socket doubles to check short writes, interruption, timeouts and zero progress.
The ID-store suite compiles the production loader against NVS doubles to verify
that failed reads never overwrite an existing counter.

The browser tests execute the complete client script in a Node VM with a minimal
DOM, WebSocket and controllable clock/storage. They cover handshake and join
timeouts, join-before-flush, stale socket events, replaced sessions, corrupt local
data and replay cursor persistence failures.

These tests do not emulate lwIP, the WebSocket wire format or real browser APIs.
Firmware compilation and later hardware/browser testing are still needed for
transport-level behavior.
