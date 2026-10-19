# 手柄 变更记录

## v1.0.3（2026-10-19）

- 发布标签：`v1.0.3-angle-handler`
- 发布提交：`19360f3`
- 提交时间：`2026-10-19 21:59:44 +0800`

### 自 v1.0.2-angle-handler 以来的改动

- `19360f3` 发布手柄 v1.0.3：版本号改为 `1.0.3`，`EXTRAVERSION` 设为 `release`（`applications/angle-handler/VERSION`）
- `93e9da3` 摇杆 X/Y 角度上报单位从 1° 整数改为 0.1°：内部仍保留 10 次采样去极值平均、±1° 中心死区与 1° 量化，最终结果 ×10 后经 HANDLER_STATE 帧 (0x1E3) 上报（-200~+200 代表 -20.0°~+20.0°），与 USER_GUIDE.md / CLAUDE.md 中已定义的协议对齐；ADC 与 GPIO 上报日志补充 (0.1deg) 单位标注（`applications/angle-handler/src/adc.c`、`applications/angle-handler/src/gpio.c`）
- `ef03fff` 支持 MCUboot 引导程序内 CAN 固件升级：CAN1 使能（`zephyr,canbus` 及 PA11/PA12 引脚配置）从应用 overlay 上移到板级 DTS 统一定义，README 补充带 CAN 升级 bootloader 的 sysbuild 构建命令（`applications/angle-handler/boards/nrf24_f103rct6.overlay`、`applications/angle-handler/README.md`）
- `2806fa6` 构建脚本改用 `target_sources_ifdef(CONFIG_SHELL ...)` 条件编译 shell 源文件，移除 `rf24_shell.c` 内的文件级 `#ifdef CONFIG_SHELL` 保护（`applications/angle-handler/CMakeLists.txt`、`applications/angle-handler/src/rf24_shell.c`）
- `6a4c018` 追加 v1.0.2 的变更记录文档（`applications/angle-handler/CHANGELOG.md`）

## v1.0.2（2026-08-07）

- 发布标签：`v1.0.2-angle-handler`
- 发布提交：`48521df`
- 提交时间：`2026-08-07 00:03:36 +0800`

### 自 v1.0.1-angle-handler 以来的改动

- `48521df` 发布手柄 v1.0.2：版本号改为 `1.0.2`，`EXTRAVERSION` 设为 `release`（`applications/angle-handler/VERSION`）
- `64752c7` 摇杆 X/Y 通道改为 0.1° 分辨率采样（300–2700mV 映射到 -200..+200），保留 10 次采样窗口的精度后再换算为 1° 整数上报；并加入中心死区——平均值落在 (-1°, +1°) 之间时归零，消除摇杆居中时的抖动（`applications/angle-handler/src/adc.c`）
- `279ada4` 新增 `adc log [0/1]` shell 命令（顶层 `adc` 命令组），开启后按通道以 INFO 级别打印 X/Y 电压采样值（`x: <mv>` / `y: <mv>`），默认关闭，用于现场调试原始采样（`applications/angle-handler/src/adc.c`、`applications/angle-handler/include/common.h`）
- `753748a` 追加 v1.0.1 的变更记录文档（`applications/angle-handler/CHANGELOG.md`）

## v1.0.1（2026-08-06）

- 发布标签：`v1.0.1-angle-handler`
- 发布提交：`a598610`
- 提交时间：`2026-08-06 19:56:46 +0800`

### 自仓库初始提交以来的改动

- `9d817d4` 手柄应用初始版本：包含 ADC 摇杆采集、CAN 总线通信、RF24 无线收发、
  OLED 显示（含电池/信号/标签图标与 5x8、8x16 字模）、GPIO 按键、参数持久化、
  RF24 shell 调试等模块，以及 nrf24_f103rct6 板子配置、设备树覆盖、MCUboot
  签名密钥和 sysbuild 配置（`applications/angle-handler/` 全目录）
- `bf89def` MCUboot 签名密钥路径改为按板子名变量引用
  `${APP_DIR}/boards/${BOARD}.pem`，避免硬编码板子名（`applications/angle-handler/sysbuild.conf`）
- `d1aa6e8` 将 RF24 无线模块的休眠采样间隔从 60ms 调整为 50ms（`applications/angle-handler/src/adc.c`）
- `a598610` 发布手柄 v1.0.1：版本号改为 `1.0.1`，`EXTRAVERSION` 设为 `release`（`applications/angle-handler/VERSION`）
