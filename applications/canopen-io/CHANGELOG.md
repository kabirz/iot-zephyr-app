# Changelog

## 0.1.0 (unreleased)

- 初版：纯 CANopen 从站节点（16DI/8DO/4AI 计测器读写，SDO + PDO）
- CiA 302-2 固件下载（OD 0x1F50/0x1F51 -> MCUboot slot1）
- 心跳生产者 0x1017（默认 1000ms）+ CiA 303-3 LED
- OD 持久化（0x1010/0x1011，settings/FCB 后端）

## Unreleased

- 移植 io-edge-hub 全部非 CAN 功能：
  - 寄存器模型统一：`holding_reg[]/input_reg[]` 为唯一参数/数据源，
    OD 0x2002（DO）与 0x2004（配置）桥接到寄存器；DO 经 RPDO1/SDO 写入
    同步到 Modbus 寄存器，Modbus/Web 写 DO 反向镜像回 OD 并触发 TPDO
  - Modbus TCP Server（端口 502, RAW ADU + select 多客户端 + Keepalive）
    与 Modbus RTU Slave（RS485 USART2, 8N1）
  - 参数持久化：settings/FCB `modbus/` 命名空间直接映射寄存器
    （IP/RS485/slave_id/采样使能与间隔/历史开关）；0x2004:5 触发全量保存；
    0x1010 持久化条目收敛为通信参数（:1）
  - Web 管理：HTTP SPA 页面 + REST API (/api/*) + WebSocket 实时推送 (/ws) +
    WebSocket 固件升级（keyhash/CRC 校验）；页面去除 CAN 配置项并适配新寄存器布局
  - FTP Server（端口 21, PASV/EPSV, admin/admin + 匿名只读）
  - 历史记录：LittleFS (/lfs1) DI/AI 采样批量落盘、1MB 轮转保留 10 个文件
  - UDP 配置命令（端口 8600）：SET/GET_IP、SET/GET_MODBUS、SET_TIME、
    FACTORY_RESET（两步确认）；接入 libs/udp_fw_upgrade 固件升级
  - RTC 时间管理（LSE/LSI）、IWDG 看门狗（30s, housekeeping 线程喂狗）、
    栈溢出保护自动重启
  - 调试 shell（`io` 根命令）与临时 TCP 带宽测试端点
    （`CONFIG_CANOPEN_IO_BW_TEST`, 默认关）
- 升级通道互斥：UDP / WebSocket / CANopen SDO 三通道共用 fw_upgrade_state 锁
