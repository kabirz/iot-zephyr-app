"""canopen-io hardware test configuration (via environment variables)."""
import os

# 未设置 CANOPEN_CHANNEL 时所有硬件测试自动跳过 (CI 无 CAN 接口)
HAS_HW = bool(os.environ.get("CANOPEN_CHANNEL"))
CAN_CHANNEL = os.environ.get("CANOPEN_CHANNEL", "can0")
NODE_ID = int(os.environ.get("CANOPEN_NODE_ID", "10"))
BITRATE = int(os.environ.get("CANOPEN_BITRATE", "250000"))
FW_BIN = os.environ.get("CANOPEN_FW_BIN", "")  # 固件升级测试用的签名镜像路径
