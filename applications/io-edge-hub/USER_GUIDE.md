# io-edge-hub 用户指南

本指南面向设备集成与现场使用,介绍 io-edge-hub 的功能、硬件接线、通信协议与默认参数。代码实现细节请参考 `CLAUDE.md` 与源码注释。

---

## 1. 功能总览

| 功能 | 接口 | 说明 |
|------|------|------|
| 数字输入 (DI) | 16 路 | 光耦隔离,24V 工业电平,MCU 侧下拉 |
| 数字输出 (DO) | 8 路 | 驱动输出 + LED 联动指示 |
| 模拟输入 (AI) | 4 路 | AI1/AI2 电流 4-20mA,AI3/AI4 电压 0-10V |
| Modbus TCP | 以太网 502 | Server,最多 3 客户端 |
| Modbus RTU | RS485 (USART2) | Slave,波特率/ID 可配 |
| FTP | 以太网 21 | 历史文件下载/管理 |
| 远程配置 | UDP 8600 | IP/Modbus/采样/CAN 参数 + 设备发现 |
| 固件升级 | UDP 8600 / CAN | 双通道,MCUboot 签名验证 |
| 历史记录 | LittleFS | DI/AI 采样,10 文件轮转 |
| 时间 | RTC (LSI) | 日志时间戳,Modbus/UDP 可设时间 |

---

## 2. 硬件配置

### 2.1 系统框图

```
STM32F407VET6 ── SPI1  ── W25Q128 (16MB Flash: 升级镜像+参数+历史)
             ── SPI2  ── W5500   (以太网)
             ── USART2 ── MAX485 (RS485 Modbus RTU)
             ── USART1 ── 调试串口 (115200, Console/Shell)
             ── CAN1   ── CAN 总线 (业务 + 固件升级)
             ── ADC1   ── 4 路 AI (PC0-PC3)
             ── GPIO   ── 16 DI / 8 DO / 8 LED
             ── RTC/IWDG/RNG (片内)
```

### 2.2 引脚分配

**SPI1 — W25Q128 Flash**:PA4(CS)/ PA5(SCK)/ PA6(MISO)/ PA7(MOSI)

**SPI2 — W5500 以太网**:PB12(CS)/ PB13(SCK)/ PB14(MISO)/ PB15(MOSI)/ PD0(RST)/ PD1(INT)

| 信号 | MCU 引脚 |
|------|---------|
| 以太网状态 LED | PE7 |

**USART2 — RS485**:PA2(TX)/ PA3(RX)/ PA1(DE/RE)

**USART1 — 调试**:PA9(TX)/ PA10(RX),115200bps

**CAN1**:PA11(RX)/ PA12(TX),250kbps

**ADC1 — 模拟输入**:

| 通道 | 引脚 | 信号 | 量程 |
|------|------|------|------|
| AI1 | PC0 (IN10) | 电流 | 4-20mA |
| AI2 | PC1 (IN11) | 电流 | 4-20mA |
| AI3 | PC2 (IN12) | 电压 | 0-10V |
| AI4 | PC3 (IN13) | 电压 | 0-10V |

**数字输入 (DI1-16)**:PD3, PD4, PD5, PD6, PB5, PB6, PB7, PB8, PB9, PB10, PB11, PD2, PB0, PB1, PB3, PB4

**数字输出 (DO1-8)**:PD7, PD8, PD9, PD10, PD11, PD12, PD13, PD14

**DO 状态 LED (LED1-8)**:PE8, PE9, PE10, PE11, PE12, PE13, PE14, PE15

**调试**:PA13(SWDIO)/ PA14(SWCLK)

> 时钟:外部 HSE 13MHz 晶振(PH0/PH1)→ PLL 168MHz(HCLK),PCLK1=42MHz,PCLK2=84MHz。W5500 自带 25MHz 独立晶振。MAC 地址运行时从 STM32 96-bit UID 派生(OUI 00:08:DC),每板唯一。

### 2.3 Flash 布局

**内部 Flash (512KB)**:MCUboot 64KB(0x08000000)+ 应用 Primary Slot 448KB(0x08010000)

**外部 W25Q128 (16MB)**:

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| slot1 | 0x000000 | 448KB | 升级镜像暂存 |
| scratch | 0x070000 | 448KB | MCUboot SWAP 交换区 |
| storage | 0x0E0000 | 64KB | 设备参数 (FCB) |
| littlefs | 0x0F0000 | ~15MB | 历史记录文件 |

---

## 3. Modbus

### 3.1 Input Registers(只读,地址 30001+)

| 地址 | 说明 |
|------|------|
| 0x00 | 固件版本(major<<8 \| minor) |
| 0x01 | AI1 电流(0.01mA) |
| 0x02 | AI2 电流(0.01mA) |
| 0x03 | AI3 电压(0.01V) |
| 0x04 | AI4 电压(0.01V) |
| 0x05 | DI1-16 状态(16-bit bitmap) |

### 3.2 Holding Registers(读写,地址 40001+)

| 地址 | 默认 | 说明 |
|------|------|------|
| 0x00 | 0 | DO1-8 输出控制 |
| 0x01 | 0xFFFF | DI1-16 使能 |
| 0x02 | 0x000F | AI1-4 使能 |
| 0x03 | 200 | DI 采样间隔(ms) |
| 0x04 | 200 | AI 采样间隔(ms) |
| 0x05 | 0 | 历史保存使能 |
| 0x06 | 0x0111 | CAN ID |
| 0x07 | 10 | CAN 波特率(x1000) |
| 0x08 | 9600 | RS485 波特率 |
| 0x09 | 1 | Modbus RTU Slave ID |
| 0x0A-0x0D | 192.168.12.101 | IP 地址(4 字节) |
| 0x0E-0x0F | 0 | 时间戳高/低16位(写后设 RTC) |
| 0x10 | 0 | 参数保存触发(写非0 → 持久化) |
| 0x11 | 0 | 写1 → 系统重启 |

> 写 0x10(非0)触发全量参数保存到 Flash;写 0x11 触发重启。掩码固定 255.255.255.0,网关 = IP 末段改 1。主站连接存活由 TCP Keepalive 检测(不占用寄存器),Modbus TCP 客户端 socket 空闲 30s 后每 5s 探测,连续 3 次无响应判定掉线并断开连接。

---

## 4. 远程配置(UDP 端口 8600)

UDP 配置端口同时承载固件升级命令(0x01-0x05,库处理)与应用业务命令(0x10+)。帧格式 `[cmd 1B][data...]`。

| Cmd | 名称 | Payload |
|-----|------|---------|
| 0x10 | SET_IP | ip(4B) |
| 0x11 | GET_NET | → ip(4B)+slave_id(1B)+tcp_port(2B) |
| 0x12 | SET_MODBUS | slave_id(1B)+rs485_baud(4B) |
| 0x13 | GET_MODBUS | 同 SET |
| 0x14 | SET_SAMPLE | di_en(2B)+ai_en(2B)+di_si(2B)+ai_si(2B) |
| 0x15 | GET_SAMPLE | 同 SET |
| 0x16 | SET_CAN | can_id(2B)+can_baud(4B) |
| 0x17 | GET_CAN | 同 SET |
| 0x18 | DISCOVER | → "io-edge-hub \<ip\> v\<ver\>"(允许广播发现) |
| 0x19 | FACTORY_RESET | 擦除参数分区 + 重启 |
| 0x1A | SET_HIS | his_save(1B) |
| 0x1B | GET_HIS | → his_save(1B) |

> 多字节字段为网络序(大端)。SET 类命令改参数后持久化(重启生效 IP)。跨子网广播回复发往 8601 端口。

---

## 5. CAN

CAN1 同时用于业务通信与 CAN 固件升级,默认 250kbps。`can_fw_upgrade` 库处理升级帧(0x101-0x105),其他帧分发给应用(业务帧 ID = holding 0x06,默认 0x0111)。

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台→设备 | 命令(升级/确认/版本/重启) |
| 0x102 | 设备→平台 | 应答 |
| 0x103 | 平台→设备 | 固件数据(8B/帧) |
| 0x104 | 平台→设备 | 密钥哈希(5×7B) |
| 0x105 | 设备→平台 | 版本字符串 |

---

## 6. FTP

FTP Server 端口 21,根目录为 LittleFS(`/lfs1`),存放历史记录文件。

- 认证:`admin`/`admin`(读写),`anonymous`(只读)
- 数据连接:**PASV / EPSV 被动 + PORT / EPRT 主动**(RFC 959 + RFC 2428)
- 最大客户端:**3 个**(单线程 select 多路复用;数据传输时该会话独占;第 4 个连接返回 421)
- 传输类型:TYPE I(二进制,默认)/ TYPE A(ASCII,自动 CR/LF 转换)
- 空闲超时:**120 秒**(无命令自动断开)
- 命令:USER PASS SYST FEAT TYPE PWD CWD CDUP PASV LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE REST NOOP QUIT
  - LIST 返回标准 Unix `ls -l` 格式(GUI 客户端兼容)
  - REST 支持 RETR/STOR 断点续传
- 用途:下载历史文件(`data_MMDD_HHMMSS.raw`)、上传/删除/重命名文件、目录管理

示例:`ftp 192.168.12.101`,登录后 `ls` / `get data_0809_1200.raw`。

---

## 7. 固件升级

支持 UDP 与 CAN 双通道,镜像需用本应用 `boards/io_edge_f407vet6.pem` 签名(RSA-2048)。

- **UDP**:上位机发 FW_START(含镜像大小 + 密钥哈希)→ FW_DATA 分块 → FW_END(CRC16-CCITT 校验)→ 重启,MCUboot SWAP 升级(失败可回滚)
- **CAN**:0x104 发密钥哈希 → 0x101 START → 0x103 数据(8B/帧,每 64B 应答)→ 0x101 CONFIRM → 重启

升级前校验签名密钥 SHA-256(防错误密钥固件刷入)。

---

## 8. 默认参数

| 参数 | 默认值 |
|------|--------|
| IP | 192.168.12.101 / 255.255.255.0 / 网关 192.168.12.1 |
| Modbus TCP 端口 | 502 |
| Modbus RTU | Slave ID 1, 9600bps |
| DI/AI 采样间隔 | 200ms |
| DI/AI 使能 | 全使能 |
| CAN | ID 0x0111, 250kbps |
| FTP | admin/admin, 端口 21 |
| UDP 配置端口 | 8600 |
| 历史记录 | 关闭 |
| TCP Keepalive | 空闲 30s / 探测 5s / 3 次 (检测主站掉线) |

恢复出厂:UDP 发 0x19,或擦除外部 Flash storage 分区(0x0E0000, 64KB)。

---

## 9. 历史数据文件

历史使能(holding 0x05 或 UDP 0x1A)后,DI/AI 采样数据写入 LittleFS。**单文件最大 1MB**(超过则新建 `data_MMDD_HHMMSS.raw`),保留至多 10 个;采样数据经 msgq 累积,系统工作队列批量写(减少 Flash 写次数)。

文件为连续记录流(小端):

```
[type(2B)][timestamp(4B, Unix)][payload]
  DI 记录 (10B): type=1 | di_en(2B) | di_value(2B)
  AI 记录 (16B): type=2 | ai_en(2B) | ai_value[4](8B)
```

与 RT-Thread PC 端解析工具兼容。
