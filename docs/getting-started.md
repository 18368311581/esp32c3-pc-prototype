# 构建、烧录和监视

## 环境

使用 ESP-IDF v6.0.2 和 `esp32c3` target。先安装并激活乐鑫工具链，在终端运行 `idf.py --version` 确认版本；本仓库不内置工具链。Windows 使用 PowerShell 7。

在仓库根目录执行：

```powershell
./scripts/build.ps1 -Role standalone
./scripts/build.ps1 -Role device
./scripts/build.ps1 -Role receiver
```

三种角色各自使用 `hello_world/build-<角色>/sdkconfig`。更换角色时保持使用对应目录，避免把另一种角色烧入板子。`sdkconfig.defaults` 保留 USB 控制台设置，`sdkconfig.defaults.esp32c3` 保留本地基线中的 DIO/80 MHz/2 MB Flash 配置；不要把 Flash 配置值当作模组实际容量检测结果。

等价的单角色命令（在 hello_world 目录执行）：

```powershell
idf.py -B build-standalone -DSDKCONFIG=build-standalone/sdkconfig -DIDF_TARGET=esp32c3 -DAPP_ROLE=standalone build
```

## USB-JTAG 烧录

1. 使用支持数据的 USB 线接板，不按住 BOOT。
2. 确认出现 USB 串行设备和 USB JTAG/serial debug unit。
3. 确认没有其他 OpenOCD/Monitor 占用设备。
4. 使用乐鑫 ESP-IDF 配套 OpenOCD，速度 1000 kHz。

以下命令在 `hello_world` 目录执行，示例烧录 standalone；`openocd` 必须来自已激活的乐鑫工具链：

```powershell
openocd -f ./openocd/esp32c3-builtin-1mhz.cfg -c "program_esp build-standalone/bootloader/bootloader.bin 0x0 verify" -c "program_esp build-standalone/partition_table/partition-table.bin 0x8000 verify" -c "program_esp build-standalone/hello_world.bin 0x10000 verify reset exit"
```

这些地址对应当前默认单应用分区布局；若调整分区，先检查构建目录中的 `flasher_args.json`，按实际地址操作。烧录 device 或 receiver 时，将命令中全部 `build-standalone` 一并换成对应目录。

VS Code 也可使用 `ESP-IDF: Flash (with JTAG)`，但必须先确认扩展选择了对应角色的构建路径、APP_ROLE 和 SDKCONFIG；公开仓库不提供与个人电脑绑定的 `.vscode`。自定义 OpenOCD 配置选择 `openocd/esp32c3-builtin-1mhz.cfg`，Flash Type 选择 JTAG。

## 串口日志

短按 RESET 后，在 `hello_world` 中执行（将 COMx 改为设备管理器中的实际端口）：

```powershell
idf.py -B build-standalone -p COMx monitor
```

波特率 115200，退出按 `Ctrl+]`。串口端口可能变化；VS Code 可重新运行 `ESP-IDF: Select Port to Use`。COM 号影响 Monitor，不能据此判断 USB-JTAG 是否可用。

单板角色输出 `BOOT`、`BOARD_TEST`、`HEARTBEAT`、`BATTERY` 等日志。按键动作打印 `BUTTON 1/0`，按下时切换 LED。发送以 LF 换行结束的 `LED 1` 或 `LED 0`，应收到相应 `OK LED`；默认 Monitor 的本地输入模式请按终端设置使用，必要时使用能明确发送 LF 的串口终端。无线角色的终端规则见无线说明。

## 常见故障

- `waiting for download`：松开 BOOT，退出 Monitor，短按 RESET 后重试。
- `LIBUSB_ERROR_NOT_FOUND`：检查数据线、USB-JTAG 驱动，确认使用的是乐鑫 OpenOCD。
- `all zeroes`：检查供电、EN、BOOT 状态及 1 MHz 配置，避免重复运行 OpenOCD。
- `esptool Write timeout`：本板 GPIO8 无外部上拉，ROM 串口下载不可靠，使用上述 JTAG 路径。

本次发布整理只验证构建；上述硬件流程沿用原开发记录，未在整理版本上重新执行。
