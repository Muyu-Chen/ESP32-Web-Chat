# ESP32-Web-Chat 文档入口

项目已经重构为分层目录结构，根目录的 `structure.md` 只保留为兼容入口。后续维护请优先阅读 `docs/` 下的文档。

## 推荐阅读顺序

1. [docs/overview.md](docs/overview.md)：项目总览、目录结构、模块职责、功能定位。
2. [docs/architecture.md](docs/architecture.md)：启动流程、模块依赖、共享上下文、任务与锁。
3. [docs/protocol.md](docs/protocol.md)：HTTP API、WebSocket 消息格式、错误返回、历史恢复流程。
4. [docs/maintenance.md](docs/maintenance.md)：新增/删除功能时应该改哪些模块。
5. [docs/build-and-flash.md](docs/build-and-flash.md)：构建、烧录、分区和常见问题。

## 当前核心目录

```text
main/
├── include/      # C 头文件，按领域分组
├── src/          # C 源文件，src/main.c 是 app_main 入口
├── web/          # 编译进固件的前端静态资源
├── CMakeLists.txt
└── Kconfig.projbuild

docs/
├── overview.md
├── architecture.md
├── protocol.md
├── maintenance.md
└── build-and-flash.md
```
