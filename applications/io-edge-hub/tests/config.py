"""测试全局配置.

设备 IP/端口可通过环境变量或命令行参数覆盖:
  IOEDGE_IP       默认 192.168.12.101
  IOEDGE_MB_PORT  默认 502  (Modbus TCP)
  IOEDGE_UDP_PORT 默认 8600 (UDP 配置/升级端口)
  IOEDGE_UDP_REPLY_PORT 默认 8601 (跨子网广播回复)
"""
import os

DEVICE_IP = os.environ.get("IOEDGE_IP", "192.168.12.101")
MODBUS_TCP_PORT = int(os.environ.get("IOEDGE_MB_PORT", "502"))
UDP_PORT = int(os.environ.get("IOEDGE_UDP_PORT", "8600"))
UDP_REPLY_PORT = int(os.environ.get("IOEDGE_UDP_REPLY_PORT", "8601"))

# Modbus 客户端超时 (秒). 慢速网络可调大.
MODBUS_TIMEOUT = 2.0
# UDP 单条命令等待超时 (秒).
UDP_TIMEOUT = 2.0

# Modbus RTU (RS485 串口). Linux 下 USB-RS485 适配器通常为 /dev/ttyUSB0.
# 设备默认 9600/8N1, slave_id=1 (来自 holding 0x09/0x08, 出厂值).
MODBUS_RTU_PORT     = os.environ.get("IOEDGE_RTU_PORT", "/dev/ttyUSB0")
MODBUS_RTU_BAUDRATE = int(os.environ.get("IOEDGE_RTU_BAUDRATE", "9600"))
MODBUS_RTU_PARITY   = os.environ.get("IOEDGE_RTU_PARITY", "N")
MODBUS_RTU_STOPBITS = int(os.environ.get("IOEDGE_RTU_STOPBITS", "1"))
MODBUS_RTU_BYTESIZE = int(os.environ.get("IOEDGE_RTU_BYTESIZE", "8"))
MODBUS_RTU_TIMEOUT  = float(os.environ.get("IOEDGE_RTU_TIMEOUT", "2.0"))
# 默认 Modbus RTU slave_id (= holding 0x09). 修改后需重启设备生效.
MODBUS_RTU_SLAVE_ID = int(os.environ.get("IOEDGE_RTU_SLAVE_ID", "1"))

# Holding 寄存器布局 (与固件 init.h 一致, v3.4)
HOLDING = {
    "DO":              0x00,
    "DI_ENABLE":       0x01,
    "AI_ENABLE":       0x02,
    "DI_SAMPLE_MS":    0x03,
    "AI_SAMPLE_MS":    0x04,
    "HISTORY_ENABLE":  0x05,
    "CAN_ID":          0x06,
    "CAN_BAUDRATE":    0x07,
    "RS485_BAUDRATE":  0x08,
    "SLAVE_ID":        0x09,
    "IP_OCTET1":       0x0A,
    "IP_OCTET2":       0x0B,
    "IP_OCTET3":       0x0C,
    "IP_OCTET4":       0x0D,
    "TIMESTAMP_HI":    0x0E,
    "TIMESTAMP_LO":    0x0F,
    "CONFIG_SAVE":     0x10,
    "REBOOT":          0x11,
}
HOLDING_COUNT = 18  # 0x00 .. 0x11

# Input 寄存器布局
INPUT = {
    "VER":  0x00,
    "AI0":  0x01,
    "AI1":  0x02,
    "AI2":  0x03,
    "AI3":  0x04,
    "DI":   0x05,
}
INPUT_COUNT = 6

# UDP 应用命令码 (与固件 udp.h 一致, v3.4)
UDP_CMD_SET_IP         = 0x10
UDP_CMD_GET_IP         = 0x11
UDP_CMD_SET_MODBUS     = 0x12
UDP_CMD_GET_MODBUS     = 0x13
UDP_CMD_SET_TIME       = 0x14
UDP_CMD_FACTORY_RESET  = 0x19

# ==================== CAN (Linux SocketCAN) ====================
# 默认使用 socketcan + can0. 用户需先配置:
#   sudo ip link set can0 type can bitrate 250000
#   sudo ip link set can0 up
# 或 vcan (虚拟, 无硬件):
#   sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
CAN_CHANNEL  = os.environ.get("IOEDGE_CAN_CHANNEL", "can0")
CAN_INTERFACE = os.environ.get("IOEDGE_CAN_INTERFACE", "socketcan")
CAN_BITRATE  = 250000  # 设备默认 CONFIG_CAN_FW_UPGRADE_BITRATE

# CAN 帧 ID (固件升级库协议, 与固件 libs/can_fw_upgrade 一致)
CAN_ID_FW_CMD      = 0x101  # 主→设 命令: data_32[0]=cmd, data_32[1]=arg (LE32, DLC=8)
CAN_ID_FW_REPLY    = 0x102  # 设→主 回复: data_32[0]=code, data_32[1]=offset/arg (LE32, DLC=8)
CAN_ID_FW_DATA     = 0x103  # 主→设 固件数据 (≤8B 原始)
CAN_ID_FW_KEYHASH  = 0x104  # 主→设 keyhash 分片: data[0]=seq, data[1..7]=7B
CAN_ID_FW_VERSION  = 0x105  # 设→主 版本分片: data[0]=seq, data[1..7]=ASCII

# 固件升级命令码 (0x101 data_32[0], LE32)
CAN_FW_CMD_START_UPDATE = 0
CAN_FW_CMD_CONFIRM      = 1
CAN_FW_CMD_VERSION      = 2
CAN_FW_CMD_REBOOT       = 3

# 固件升级回复码 (0x102 data_32[0], LE32)
CAN_FW_CODE_OFFSET         = 0
CAN_FW_CODE_UPDATE_SUCCESS = 1
CAN_FW_CODE_VERSION        = 2
CAN_FW_CODE_CONFIRM        = 3
CAN_FW_CODE_FLASH_ERROR    = 4
CAN_FW_CODE_TRANSFER_ERROR = 5
CAN_FW_CODE_KEYHASH_ERROR  = 6

CAN_FW_CONFIRM_MAGIC = 0x55AA55AA  # CONFIRM 成功时的 arg 值

# 业务帧默认 ID (holding 0x06 CAN_ID, 可通过 Modbus 改)
CAN_DEFAULT_BUSINESS_ID = 0x0111

# CAN 单帧等待超时 (秒)
CAN_FRAME_TIMEOUT = 2.0
