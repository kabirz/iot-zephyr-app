# Changelog

## 0.1.0 (unreleased)

- 初版：纯 CANopen 从站节点（16DI/8DO/4AI 计测器读写，SDO + PDO）
- CiA 302-2 固件下载（OD 0x1F50/0x1F51 -> MCUboot slot1）
- 心跳生产者 0x1017（默认 1000ms）+ CiA 303-3 LED
- OD 持久化（0x1010/0x1011，settings/FCB 后端）
