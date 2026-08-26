# canopen-io 用户指南

io_edge_f407vet6 上的纯 CANopen IO 计测节点（io-edge-hub 的 CANopen 版）。
默认节点号 10（`CONFIG_CANOPEN_NODE_ID`），CAN1 250 kbit/s。

## 通信对象（预连接 COB-ID = 基址 + 节点号）

| 通道 | COB-ID | 说明 |
|---|---|---|
| NMT | 0x000 | 主站控制（01 启动 / 02 停止 / 81 复位节点 / 82 复位通信） |
| 心跳 | 0x70A | 1s 周期，byte0=NMT 状态（05=Operational） |
| SDO | 0x60A / 0x58A | 支持 expedited/分段/块传输（900B 缓冲） |
| EMCY | 0x80A | 紧急报文 |
| TPDO1 | 0x18A | AI1-4（8B），每次 AI 采样触发（默认 100ms） |
| TPDO2 | 0x28A | DI 位图 + DO 回读（4B），变更触发（20ms inhibit）+ 1s 兜底 |
| RPDO1 | 0x20A | DO 控制（2B），收到即写 |

## 对象字典（厂家区）

| Index:Sub | 名称 | 类型 | 访问 | 默认 | 语义 |
|---|---|---|---|---|---|
| 0x100A | 软件版本 | string | RO | v0.1.0_xxxxxx | v主.次.补丁_git |
| 0x1017 | 心跳时间 | u16 | RW | 1000 | ms；写 0 关闭；可持久化 |
| 0x2000:1-4 | AI1-AI4 | i16 | RO | - | 0.01mA（AI1/2）/ 0.01V（AI3/4） |
| 0x2001 | DI 位图 | u16 | RO | - | DI1-16 |
| 0x2002 | DO 控制 | u16 | RW | 0 | 写即联动 DO GPIO + LED；TPDO2 回读 |
| 0x2004:1 | DI 使能 | u16 | RW | 0xFFFF | 位图 |
| 0x2004:2 | AI 使能 | u16 | RW | 0x000F | 低 4 位 |
| 0x2004:3 | DI 采样间隔 | u16 | RW | 100 | ms，钳位 10-5000 |
| 0x2004:4 | AI 采样间隔 | u16 | RW | 100 | ms，钳位 10-5000 |
| 0x2004:5 | 保存触发 | u16 | RW | 0 | 写 1 → 持久化全部参数（回读 0） |
| 0x2004:6 | 重启触发 | u16 | RW | 0 | 写 1 → 延迟重启（回读 0） |
| 0x1F50:1 | 固件数据 | DOMAIN | WO | - | SDO 写流式入 slot1 |
| 0x1F51:1 | 固件控制 | u32 | RW | 0 | 写 0 复位/进入下载；写 1 确认升级；读=状态（0=IDLE 1=READY 2=STREAMING 3=CONFIRMED 4=ERROR） |

## 参数持久化

写 `0x2004:5=1`（或标准 0x1010:1/:2 写 "save"）保存；断电不丢。
0x1011:1/:2 写 "load" 恢复出厂（重启后生效）。存储位于外部 W25Q128
storage_partition（settings/FCB）。**固件下载进行中写 0x2004:5 会被拒绝
（SDO abort 0x08000022）。**

## 固件升级（CiA 302-2）

```shell
python tools/firmware_upgrade/canopen_fw_upgrade.py version  -c can0
python tools/firmware_upgrade/canopen_fw_upgrade.py upgrade  -c can0 -f zephyr.signed.bin
```

镜像必须是本应用密钥（`boards/io_edge_f407vet6.pem`）签名的 MCUboot 格式
（`--sysbuild` 构建自动签名）。升级流程 0x1F51=0 → SDO 块下载 0x1F50 →
0x1F51=1 → 设备延迟 500ms 重启 → MCUboot SWAP_SCRATCH 换固件并验签
（约 10-30s，期间 LED 快闪）→ 新版本上线。验签失败 MCUboot 自动回滚旧版本。
传输中断后重跑 `upgrade` 即可恢复。**注意：升级只发生在应用域，若应用损坏
需 SWD 重刷。**

## 故障排查

| 现象 | 排查 |
|---|---|
| 总线无心跳 | 波特率/终端电阻；确认烧录的是本应用（串口日志 `canopen-io v...`） |
| SDO 块下载 abort 0x08000020 | flash 写失败：查 0x1F51 状态（4=ERROR），重写 0x1F51=0 复位 |
| SDO 写 0x2004:5 被拒 | 固件下载进行中，等升级结束或复位后再保存 |
| 升级后版本未变 | 镜像未用本应用 pem 签名（MCUboot 拒绝并回滚） |
