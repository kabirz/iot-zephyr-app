# iot-zephyr-app

基于 Zephyr RTOS 的 IoT 应用合集：一个 west manifest 仓库 + 自定义 Zephyr module，包含运行于 STM32F103RCT6 的两个互相配套的嵌入式应用（角度手柄 + 中转网关）。

## 概述

- **angle-handler** — 激光测距手持控制器：采集操纵杆角度 / 电池状态，OLED 显示，经 CAN 或 nRF24L01+ (2.4G) 与设备通信，支持系统休眠与 OTA 升级。
- **n2e-gw** — 数据中转网关：在 angle-handler 与上位机之间，通过 nRF24L01+ ↔ W5500 以太网 UDP 透传数据。

两个应用共用同一块板 `nrf24_f103rct6`，但**各自独立的 MCUboot RSA-2048 签名密钥**（`applications/<app>/boards/nrf24_f103rct6.pem`），镜像互不通用。

## 目录结构

```
.
├── west.yml                 # west manifest (Zephyr v4.4.0)
├── zephyr/                  # 本仓库作为 Zephyr module 的声明
│   └── module.yml
├── boards/                  # board_root: nrf24_f103rct6
├── dts/                     # dts_root: 自定义 binding
├── libs/                    # 固件升级库 (CAN/UDP) + 配置头生成脚本
├── drivers/                 # 自定义驱动 (nrf24l01p)
├── applications/
│   ├── angle-handler/       # 手柄应用
│   └── n2e-gw/              # 网关应用
├── scripts/                 # west 自定义命令 (west archive)
├── tools/                   # 辅助工具脚本
└── .github/workflows/       # CI: angle-handler.yml / n2e-gw.yml
```

## 环境要求

- Zephyr SDK + arm 工具链（参考 [官方指南](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)）
- Python 3.12+、west

## 快速开始

```shell
west init -m <this_git_url> iot-zephyr-app
cd iot-zephyr-app
west update
west package pip --install     # 安装 zephyr/requirements.txt
```

构建（sysbuild 含 MCUboot）：

```shell
# angle-handler
west build -b nrf24_f103rct6 applications/angle-handler --sysbuild

# n2e-gw
west build -b nrf24_f103rct6 applications/n2e-gw --sysbuild
```

归档镜像（自定义 `west archive` 命令）：

```shell
west archive --no-rebuild -o angle-handler
```

烧录：

```shell
west flash                            # 烧全部镜像 (mcuboot + app)
west flash --domain angle-handler     # 只烧 app
```

## License

Apache-2.0
