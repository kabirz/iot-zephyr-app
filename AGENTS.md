# AGENTS.md

本文件为 AI 编码助手在本仓库工作的指引（Claude Code / 其他 agent 均原生读取 `AGENTS.md`）。

## 项目

**iot-zephyr-app** — Zephyr RTOS west manifest 仓库，同时本身是一个 Zephyr module。包含四个应用、三块板子：

- `applications/angle-handler` — 激光测距手持控制器（角度采集 + OLED + CAN/nRF24 + OTA），STM32F103RCT6（板 `nrf24_f103rct6`）
- `applications/n2e-gw` — 数据中转网关（nRF24 ↔ W5500 UDP），STM32F103RCT6（板 `nrf24_f103rct6`）
- `applications/io-edge-hub` — 工业 IO 采集边缘网关（16DI/8DO/4AI + Modbus TCP/RTU + LittleFS/FTP + UDP+CAN 双通道固件升级 + MCUboot SWAP_SCRATCH），STM32F407VET6（板 `io_edge_f407vet6`）
- `applications/laser-ctrl` — 激光测距设备主控（串口激光模块 + CAN 上报 + FPGA SPI 配置 + 外部 Flash + MCUboot OTA），STM32F103RET6（板 `laser_f103ret7`）

## 仓库布局

- `west.yml` — manifest，官方 Zephyr **v4.4.2**（`zephyr_site` remote），import 上游 mcuboot / cmsis / hal_stm32 / littlefs / mbedtls / zcbor 等。
- `zephyr/patches.yml` + `zephyr/patches/` — 对官方 Zephyr 的本地补丁（`west patch apply` 打上，见「Zephyr 补丁」）。
- `zephyr/module.yml` — 本仓库作为 Zephyr module 的声明（`name: iot_zephyr_app`，board_root / dts_root，build cmake/kconfig）。
- `boards/` — board_root：`nrf24_f103rct6`（F103）、`io_edge_f407vet6`（F407）、`laser_f103ret7`（F103RET6 激光控制）。
- `dts/` — dts_root，自定义 binding（`nordic,nrf24l01p.yaml`）。
- `libs/` — 固件升级库（`can_fw_upgrade`、`udp_fw_upgrade`）+ 配置头生成（`gen_gitver.py`、`gen_keyhash.py`）。**只有一个顶层 `CMakeLists.txt`**，子模块源码直接在此用 `zephyr_sources_ifdef` / `zephyr_include_directories_ifdef` 汇总。
- `drivers/` — 自定义驱动（`nrf24l01p`）。
- `scripts/west_commands/` — 自定义 west 命令（`west archive`，由 `archive.py` 实现）。
- `tools/` — 辅助脚本（如 `sh1106_font_generator.py`）+ `tools/firmware_upgrade/` 固件升级 CLI（Python/C 双实现, UDP+CAN, 含 MCUboot bootloader 模式）。

## 构建命令

```shell
west update
west patch apply     # 给官方 Zephyr 打本地补丁（west update 后必须执行一次）
west build -b nrf24_f103rct6 applications/angle-handler --sysbuild
west build -b nrf24_f103rct6 applications/n2e-gw --sysbuild
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild "-Dmcuboot_EXTRA_CONF_FILE=$PWD/applications/io-edge-hub/sysbuild/mcuboot.conf;$PWD/libs/can_fw_upgrade/mcuboot_can.conf"   # 让 MCUboot 支持 CAN 固件升级
west build -b laser_f103ret7 applications/laser-ctrl --sysbuild
west archive --no-rebuild -o angle-handler
west flash --domain angle-handler
```

## 关键约定

- **module 入口**：`zephyr/module.yml`；根 `CMakeLists.txt` → `add_subdirectory(libs drivers)`；根 `Kconfig` → `rsource libs/Kconfig drivers/Kconfig`。
- **Zephyr 补丁**：不维护 Zephyr fork，直接用官方 `v4.4.2` + `zephyr/patches/*.patch`（4 个：`NET_PKT_DTCM`、STM32 串口 7 数据位偶校验、`LOG_OUTPUT_TIMEZONE_OFFSET`、ARP 队列上限），由 Zephyr 自带的 `west patch` 命令管理（`patches.yml` 里 `path` 相对 `zephyr/patches/`，含 sha256 校验；改 patch 后需同步更新 sha256sum）。新增本地改动一律做成 patch，不要再 fork。`apply-command: git am` 使补丁落为**真实提交**（zephyr 树保持干净，`BUILD_VERSION` 显示 `v4.4.2-4-g<hash>`）。**`west patch apply` 非幂等**：重复执行会失败（并留下 `.git/rebase-apply` 残留），重打前先 `west patch clean`（其 `checkout-command` 会 `git am --abort` + `git reset --hard v4.4.2` 清场）。**升级 Zephyr 版本时**，`west.yml` 的 `revision` 与 `patches.yml` 的 `checkout-command` 里写死的 `v4.4.2` 要同步改。
- **MCUboot 签名**：每个 app 自带 `boards/${BOARD}.pem`（RSA-2048），在 `sysbuild.conf` 用 `${APP_DIR}/boards/${BOARD}.pem` 引用。各 app 密钥独立、镜像互不通用。
- **固件升级**：`libs/` 的 `can_fw_upgrade` / `udp_fw_upgrade` 由对应 `CONFIG_*_FW_UPGRADE` 控制；签名密钥哈希头由 `gen_keyhash.py` 在 configure 期生成。
- **版本字符串**：`gen_gitver.py` 编译期注入 6 位 git commit hash（`FW_GIT_VERSION`），无 git 时 fallback `000000`。
- **CONFIG 符号前缀**：angle-handler 用 `ANGLE_HANDLER_*`，n2e-gw 用 `N2E_GW_*`，io-edge-hub 用通用 Zephyr 符号 + 应用 Kconfig `IO_*`。

## 代码风格

- C：tab 缩进、K&R；中文注释 OK。
- CMake / Kconfig：tab 缩进。
- 不要提交 `build/`、`*.log`、`compile_commands.json`、`.cache`。
- 签名私钥（`*.pem`）在各 app 的 `boards/` 下，公开仓库需注意泄露风险。

## Git 提交规范（重要）

- **提交前必须先确认**：完成代码改动后不要直接 `git commit` / `git push`，先把待提交的改动、拟定
  的 commit message 一并展示给用户确认，得到明确同意后再提交。绝不擅自提交或推送。
- **commit message 一律用英文**：标题与正文都用英文书写，不要写中文。遵循常规风格——祈使句、
  首行简短（建议 ≤ 72 字符），必要时空一行后写正文。例如：
  - ✅ `ci: align tag trigger patterns with release skill naming`
  - ✅ `release: angle-handler v1.0.1`
  - ❌ `ci: tag 触发模式改为 v*-<app_name>...`（中文）
- 仅在用户明确要求「提交」「commit」「push」时才执行提交与推送。

## 命名约定（重要）

- 目录 / 文档用连字符名：`angle-handler`、`n2e-gw`、`io-edge-hub`。
- 板名用下划线：`nrf24_f103rct6`、`io_edge_f407vet6`。
- CMake `project()` / C 标识符 / CONFIG 符号用下划线形：`angle_handler`、`n2e_gw`、`io_edge_hub`（连字符在 C/CMake 变量中非法）。
- n2e-gw 内部 `gw_*` 函数、`gw` shell 命令、settings 键 `gw/gateway` 为历史缩写，**保持不变**（改名会破坏已部署设备的持久化配置）。
- 概念词不改：`IPv4 gateway`、网络"网关"等。
