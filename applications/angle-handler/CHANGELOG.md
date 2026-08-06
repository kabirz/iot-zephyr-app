# 手柄 变更记录

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
