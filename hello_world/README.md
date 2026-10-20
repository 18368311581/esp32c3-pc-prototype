# ESP32-C3 三角色固件

本目录已从 ESP-IDF Hello World 示例扩展为自制板固件。

| APP_ROLE | 源文件 | 用途 |
| --- | --- | --- |
| standalone（默认） | main/hello_world_main.c | USB 命令、按键、LED、心跳、ADC |
| device | main/device_main.c + main/espnow_link.c | ESP-NOW 设备 |
| receiver | main/receiver_main.c + main/espnow_link.c | USB 终端接收器 |

从仓库根目录执行 `./scripts/build.ps1 -Role standalone`（或 device、receiver）。

详见 [入门与烧录](../docs/getting-started.md) 和 [无线操作](../docs/wireless.md)。
原示例的 pytest 依赖 Hello World 文本，已不适用于当前固件，未作为本项目有效测试收入。
