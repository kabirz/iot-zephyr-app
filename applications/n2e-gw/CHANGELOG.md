# 手柄接收器 变更记录

## v2.0.2（2026-08-07）

- 发布标签：`v2.0.2-n2e-gw`
- 发布提交：`a27e06a`
- 提交时间：`2026-08-07 00:05:56 +0800`

### 自 v2.0.1-n2e-gw 以来的改动

- `a27e06a` 发布手柄接收器 v2.0.2：版本号改为 `2.0.2`，`EXTRAVERSION` 设为 `release`（`applications/n2e-gw/VERSION`）
- `871df2c` 在配置端口（UDP 8600）新增恢复出厂设置命令（`0x16`）：执行后擦除设置分区，清除全部已保存配置（RF24/IP/端口/DHCP/主机名等），成功时回复 `[0x16][1]` 并在 100ms 后冷重启使默认配置生效，擦除失败时回复 `[0]` 且不重启（`applications/n2e-gw/src/udp.c`、`applications/n2e-gw/include/n2e_gw.h`、`applications/n2e-gw/USER_GUIDE.md`、`applications/n2e-gw/README.md`）
- `2d6a522` 追加 v2.0.1 的变更记录文档（`applications/n2e-gw/CHANGELOG.md`）

## v2.0.1（2026-08-06）

- 发布标签：`v2.0.1-n2e-gw`
- 发布提交：`fc1edf5`
- 提交时间：`2026-08-06 20:00:18 +0800`

### 自仓库初始提交以来的改动

- `9d817d4` 手柄接收器应用初始版本：包含 RF24 无线接收、UDP 网络上报、LED 状态指示、
  配置与参数持久化、网络/RF24 shell 调试等模块，以及 nrf24_f103rct6 板子配置、
  设备树覆盖、MCUboot 签名密钥和 sysbuild 配置（`applications/n2e-gw/` 全目录）
- `bf89def` MCUboot 签名密钥路径改为按板子名变量引用
  `${APP_DIR}/boards/${BOARD}.pem`，避免硬编码板子名（`applications/n2e-gw/sysbuild.conf`）
- `fc1edf5` 发布手柄接收器 v2.0.1：版本号改为 `2.0.1`，`EXTRAVERSION` 设为 `release`（`applications/n2e-gw/VERSION`）
