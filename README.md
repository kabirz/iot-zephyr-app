# iot-zephyr-app

基于 Zephyr RTOS 的 IoT 应用合集:一个 west manifest 仓库 + 自定义 Zephyr module,包含三个嵌入式应用。前两个(`angle-handler` / `n2e-gw`)运行于 STM32F103RCT6 并互相配套;第三个(`io-edge-hub`)是独立工业 IO 采集节点,运行于 STM32F407VET6。

## 概述

| 应用 | 板 | 说明 |
|------|----|------|
| **angle-handler** | `nrf24_f103rct6` | 激光测距手持控制器:采集操纵杆角度 / 电池状态,OLED 显示,经 CAN 或 nRF24L01+ (2.4G) 通信,支持休眠与 OTA |
| **n2e-gw** | `nrf24_f103rct6` | 数据中转网关:在 angle-handler 与上位机之间,通过 nRF24L01+ ↔ W5500 以太网 UDP 透传数据 |
| **io-edge-hub** | `io_edge_f407vet6` | 工业 IO 数据采集边缘节点:16 DI / 8 DO / 4 AI,Modbus TCP/RTU,历史存储 + FTP,双通道 (UDP+CAN) 固件升级 |

`angle-handler` 与 `n2e-gw` 共用板 `nrf24_f103rct6`,但**各自独立的 MCUboot RSA-2048 签名密钥**(`applications/<app>/boards/nrf24_f103rct6.pem`),镜像互不通用。`io-edge-hub` 使用独立板 `io_edge_f407vet6` 及其独立密钥。

## 目录结构

```
.
├── west.yml                 # west manifest (Zephyr v4.4.0)
├── zephyr/                  # 本仓库作为 Zephyr module 的声明
│   └── module.yml
├── boards/                  # board_root: nrf24_f103rct6 / io_edge_f407vet6
├── dts/                     # dts_root: 自定义 binding
├── libs/                    # 固件升级库 (CAN/UDP) + 配置头生成脚本
├── drivers/                 # 自定义驱动 (nrf24l01p)
├── applications/
│   ├── angle-handler/       # 手柄应用
│   ├── n2e-gw/              # 网关应用
│   └── io-edge-hub/         # IO 采集边缘节点 (F407VET6)
├── scripts/                 # west 自定义命令 (west archive)
├── tools/                   # 辅助工具脚本
└── .github/workflows/       # CI
```

## 环境要求

- Zephyr SDK + arm 工具链(参考 [官方指南](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
- Python 3.12+、west
- `io-edge-hub` 还需外部 `littlefs` module(见 `west.yml` name-allowlist,`west update` 拉取)

## 快速开始

```shell
west init -m <this_git_url> iot-zephyr-app
cd iot-zephyr-app
west update
west package pip --install     # 安装 zephyr/requirements.txt
```

构建(sysbuild 含 MCUboot):

```shell
# angle-handler
west build -b nrf24_f103rct6 applications/angle-handler --sysbuild

# n2e-gw
west build -b nrf24_f103rct6 applications/n2e-gw --sysbuild

# io-edge-hub (建议独立 build-dir)
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild --build-dir build/io-edge-hub
# 发布模式 (关 LOG/SHELL, 更小体积)
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild -DCONF_FILE=prj_release.conf --build-dir build/io-edge-hub-rel
```

归档镜像(自定义 `west archive` 命令):

```shell
west archive --no-rebuild -o angle-handler
```

烧录:

```shell
west flash                            # 烧全部镜像 (mcuboot + app)
west flash --domain angle-handler     # 只烧 app
```

## License

Apache-2.0
