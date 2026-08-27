"""canopen-io 硬件测试配置 (环境变量覆盖).

环境变量:
  CANOPEN_CHANNEL           SocketCAN 通道 (默认 can0); 设置后启用 CANopen 用例
  CANOPEN_NODE_ID           节点号 (默认 10)
  CANOPEN_BITRATE           波特率 (默认 250000)
  CANOPEN_FW_BIN            固件升级测试用的签名镜像路径
  IOEDGE_IP                 设备 IP (默认 192.168.12.101)
  IOEDGE_MB_PORT            Modbus TCP 端口 (默认 502)
  IOEDGE_UDP_PORT           UDP 配置端口 (默认 8600)
  IOEDGE_RTU_PORT           RS485 串口 (默认 /dev/ttyUSB0); 存在才跑 RTU 用例
  CANOPEN_ALLOW_DESTRUCTIVE 设为 1 启用出厂复位等破坏性用例
"""
import os

# ==================== CANopen (SocketCAN) ====================
HAS_HW = bool(os.environ.get("CANOPEN_CHANNEL"))
CAN_CHANNEL = os.environ.get("CANOPEN_CHANNEL", "can0")
NODE_ID = int(os.environ.get("CANOPEN_NODE_ID", "10"))
BITRATE = int(os.environ.get("CANOPEN_BITRATE", "250000"))
FW_BIN = os.environ.get("CANOPEN_FW_BIN", "")  # 固件升级测试用的签名镜像路径

# ==================== 网络 (Modbus TCP / UDP / Web) ====================
DEVICE_IP = os.environ.get("IOEDGE_IP", "192.168.12.101")
MODBUS_TCP_PORT = int(os.environ.get("IOEDGE_MB_PORT", "502"))
UDP_PORT = int(os.environ.get("IOEDGE_UDP_PORT", "8600"))
UDP_REPLY_PORT = int(os.environ.get("IOEDGE_UDP_REPLY_PORT", "8601"))

MODBUS_TIMEOUT = 2.0
UDP_TIMEOUT = 2.0

# ==================== Modbus RTU (RS485) ====================
MODBUS_RTU_PORT     = os.environ.get("IOEDGE_RTU_PORT", "/dev/ttyUSB0")
MODBUS_RTU_BAUDRATE = int(os.environ.get("IOEDGE_RTU_BAUDRATE", "9600"))
MODBUS_RTU_PARITY   = os.environ.get("IOEDGE_RTU_PARITY", "N")
MODBUS_RTU_STOPBITS = int(os.environ.get("IOEDGE_RTU_STOPBITS", "1"))
MODBUS_RTU_BYTESIZE = int(os.environ.get("IOEDGE_RTU_BYTESIZE", "8"))
MODBUS_RTU_TIMEOUT  = float(os.environ.get("IOEDGE_RTU_TIMEOUT", "2.0"))
# 默认 slave_id (= holding 0x07 出厂值). 修改需重启生效.
MODBUS_RTU_SLAVE_ID = int(os.environ.get("IOEDGE_RTU_SLAVE_ID", "1"))

# 串口在位才跑 RTU 用例 (CI 无串口自动跳过)
HAS_RTU = os.path.exists(MODBUS_RTU_PORT)

# ==================== 保持寄存器布局 ====================
# 与固件 include/init.h 合并版一致: 16 个 holding (0x00-0x0F),
# CAN 业务寄存器已删除, IP/时间戳位置相对 io-edge-hub 前移
HOLDING = {
    "DO":              0x00,
    "DI_ENABLE":       0x01,
    "AI_ENABLE":       0x02,
    "DI_SAMPLE_MS":    0x03,
    "AI_SAMPLE_MS":    0x04,
    "HISTORY_ENABLE":  0x05,
    "RS485_BAUDRATE":  0x06,
    "SLAVE_ID":        0x07,
    "IP_OCTET1":       0x08,
    "IP_OCTET2":       0x09,
    "IP_OCTET3":       0x0A,
    "IP_OCTET4":       0x0B,
    "TIMESTAMP_HI":    0x0C,
    "TIMESTAMP_LO":    0x0D,
    "CONFIG_SAVE":     0x0E,
    "REBOOT":          0x0F,
}
HOLDING_COUNT = 16  # 0x00 .. 0x0F

# ==================== 输入寄存器布局 ====================
INPUT = {
    "VER": 0x00,
    "AI0": 0x01,
    "AI1": 0x02,
    "AI2": 0x03,
    "AI3": 0x04,
    "DI":  0x05,
}
INPUT_COUNT = 6

# ==================== CANopen OD 厂家区 (与 objdict/OD.h 一致) ====================
OD_AI_BASE       = 0x2000  # :1-4 AI (i16)
OD_DI            = 0x2001  # DI 位图 (u16)
OD_DO            = 0x2002  # DO 控制/回读 (u16)
OD_CFG_DI_EN     = (0x2004, 1)
OD_CFG_AI_EN     = (0x2004, 2)
OD_CFG_DI_MS     = (0x2004, 3)
OD_CFG_AI_MS     = (0x2004, 4)
OD_CFG_SAVE_TRIG = (0x2004, 5)  # 写 1 触发保存, 回读恒 0
OD_CFG_REBOOT    = (0x2004, 6)  # 写 1 触发延迟重启, 回读恒 0

# ==================== UDP 应用命令码 (与固件 src/udp.h 一致) ====================
UDP_CMD_SET_IP         = 0x10
UDP_CMD_GET_IP         = 0x11
UDP_CMD_SET_MODBUS     = 0x12
UDP_CMD_GET_MODBUS     = 0x13
UDP_CMD_SET_TIME       = 0x14
UDP_CMD_FACTORY_RESET  = 0x19

# 破坏性用例开关 (出厂复位会擦参数并重启)
ALLOW_DESTRUCTIVE = os.environ.get("CANOPEN_ALLOW_DESTRUCTIVE", "") == "1"
