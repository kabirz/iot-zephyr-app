"""固件升级全流程: 直接驱动上位机脚本 (需 CANOPEN_FW_BIN 指向签名镜像)."""
import os
import subprocess

import pytest

import config

pytestmark = [
    pytest.mark.skipif(not config.HAS_HW,
                       reason="set CANOPEN_CHANNEL to run HW tests"),
    pytest.mark.skipif(not config.FW_BIN,
                       reason="set CANOPEN_FW_BIN to a signed image"),
]

TOOL = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..",
                    "tools", "firmware_upgrade", "canopen_fw_upgrade.py")


def test_upgrade_roundtrip():
    env = dict(os.environ)
    env.setdefault("CANOPEN_FW_BIN", config.FW_BIN)
    result = subprocess.run(
        [os.sys.executable, os.path.abspath(TOOL), "upgrade",
         "-c", config.CAN_CHANNEL, "--node-id", str(config.NODE_ID),
         "-f", config.FW_BIN],
        capture_output=True, text=True, timeout=300, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
