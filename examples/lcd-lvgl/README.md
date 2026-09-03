# lcd-lvgl — LVGL 9 + ILI9341 (FMC 8080) 显示测试

TFTLCD 2.8 寸 (ILI9341 240x320) 的 LVGL 图形栈验证例程，全部使用
Zephyr 原生驱动栈：

```
LVGL 9.5 (zephyr port, CONFIG_LVGL)
  └─ display API (drivers/display/display_ili9341.c, ili9xxx 族)
       └─ MIPI DBI Type B (drivers/mipi_dbi/mipi_dbi_stm32_fmc.c)
            └─ FMC NE1 bank (板 DTS lcd_bank, 8080 16 位)
                 └─ lcd_mem MPU IO 区 (禁缓存, 板 DTS 声明)
```

界面: 标题 + 500ms 递增计数器 + 进度弧 + 三按钮 + 底部 7 色带;
shell 命令 `lvgl` 输出 LVGL 版本与分辨率。

**触摸** (实测正常): 电阻触摸 XPT2046 经 `zephyr,spi-bitbang` 软 SPI
接入 (触摸 5 线分散在 PH6/PI3/PG3/PI8, 无硬件 SPI 可覆盖), LVGL 指针
输入由 `zephyr,touch` chosen 自动绑定 (LV_Z_POINTER_FROM_CHOSEN_TOUCH)。
校准边界 (min/max-x/y) 与 z-threshold 在板 DTS touch 节点, 按压不准时微调。

## 使用

```shell
source ~/.venv/zephyr/bin/activate
west build -b apollo_v2_h743iit6 apps/examples/lcd-lvgl -d build/apollo_lvgl
west flash -d build/apollo_lvgl
# 串口: python -m serial.tools.miniterm /dev/ttyUSB0 115200 --rts 0 --dtr 0
```

实测: 完整 UI 正常渲染（左右镜像已通过 `h-mirror` 修正 —— 此模块
需要 MADCTL MX 位）。

## 踩坑记录

1. **背光 PB5 需应用侧使能**: 显示栈 (mipi-dbi-stm32-fmc) 的
   `power-gpios` 只配置输出不置电平，且无标准 backlight 属性。
2. **本 fork 的 LVGL port 无自动刷新线程** —— 应用必须自己循环调用
   `lv_timer_handler()`（官方 sample 同样如此），建完 UI 直接 return
   会黑屏/无渲染。
3. **`CONFIG_LV_Z_BUFFER_SIZE` 符号在本 fork 不存在**（上游文档有），
   不要写进 prj.conf，默认 VDB (10%) 即可。
4. 非网络应用编译报 yt8512 PHY 符号缺失 —— 已在驱动层修复
   (Kconfig `depends on ETH_STM32_HAL` + 无网络时的 CONFIG 回退值)。

## 板级 DTS 配套 (已在 apps/boards/apollo_v2_h743iit6/)

- `lcd_bank` (sram@0) 下新增 `fmc-mipi-dbi` 节点:
  `bank-address = <0x60000000>`（绕过 fork 缺失的 FMC_BANK1_x 宏）、
  `register-select-pin = <18>` (RS=A18, 数据口 0x60080000)
- `lcd-panel@0`: ilitek,ili9341, MIPI_DBI_MODE_8080_BUS_16_BIT,
  RGB565, 240x320, `h-mirror`
- `chosen { zephyr,display = &lcd_ili9341; }`
- 复用既有的 `lcd_mem` MPU IO 区 (禁缓存 —— 见 hw-bringup README)
- root 节点内 `touch-spi` (zephyr,spi-bitbang: CLK=PH6/MOSI=PI3/
  MISO=PG3/CS=PI8) + `xpt2046@0` (int-gpios=PH7 T_PEN 低有效)
- **注意**: 新建节点必须放在 root 块内 —— root 之后只允许 `&label {}`
  扩展语法, 顶层 `label: name {}` 会被 dtlib 当作节点扩展而报
  "expected label reference"
