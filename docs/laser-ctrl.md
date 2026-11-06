# laser-ctrl 激光控制

激光测距设备主控应用，运行于 STM32F103RET6（`laser_f103ret7`）。通过串口与激光测距模块通信，经 CAN 总线与平台交互，支持 FPGA SPI 配置与 OTA 固件升级。

## 功能特性

- **串口激光通信** — 与激光测距模块的协议交互（`laser_serial.c`）
- **CAN 总线** — 与上层平台的数据交换（`can.c`）
- **FPGA SPI 配置** — 当 devicetree 存在 `spi_laser_fpga` 分区时自动编译（`fpga_spi.c`）
- **外部 Flash 存储** — 配置与参数持久化（`flash.c`）
- **固件升级** — MCUboot OTA（`firmware.c`）

## 构建

```bash
west build -b laser_f103ret7 applications/laser-ctrl --sysbuild
```

## Zephyr 依赖

- manifest：本仓库 `west.yml`（Zephyr v4.4.0 / `app-main` fork）
- 串口 7 数据位 + 校验位的 STM32 修复已包含在该 fork（`serial: stm32: fix 7 data bits with parity even`），无需额外打补丁

## 源码结构

```
laser-ctrl/
├── src/
│   ├── main.c           # 主程序
│   ├── laser_serial.c   # 激光模块串口通信
│   ├── can.c            # CAN 总线收发
│   ├── flash.c          # 外部 Flash 读写
│   ├── firmware.c       # OTA 固件升级
│   └── fpga_spi.c       # FPGA SPI 配置 (条件编译)
├── include/             # 公共头文件
├── dts/                 # devicetree (含 spi,laser binding)
├── boards/              # 板级 overlay / conf
└── sysbuild.conf        # MCUboot 配置 (签名密钥 ${APP_DIR}/boards/${BOARD}.pem)
```

## 镜像导出

```bash
west archive --no-rebuild -o laser-ctrl
```
