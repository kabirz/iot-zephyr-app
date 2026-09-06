#!/bin/bash
# apollo-v2 中文字库一键安装
#
#   font_upload.sh [board_ip] [font.bin]
#
# 流程: Web 关背光(省电流余量) -> SMP 限速上传到 /QSPI: -> 串口 fontinstall
#       -> 重启 -> 开背光
#
# 注意: 板子 3.3V 余量不足时 QSPI 持续擦写会引发整板掉电 (2026-09-05 实测,
# CH340 随 MCU 同时掉线)。若仍崩溃, 检查供电 (换 USB 口/线, 用带供电的 HUB
# 或 DC 独立供电) 后重试。

set -e
cd "$(dirname "$0")/../../.."   # 工作区根

IP=${1:-192.168.12.200}
FONT=${2:-/tmp/font.bin}
PORT=$(ls /dev/ttyUSB* 2>/dev/null | head -1)

[ -f "$FONT" ] || {
	echo "先生成字库: python apps/applications/apollo-v2/tools/font_pack.py '' '$FONT'"
	echo "  (默认字体 /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc)"
	exit 1
}
[ -n "$PORT" ] || { echo "找不到串口 /dev/ttyUSB*"; exit 1; }

echo "== 1) 关背光 (省电流) =="
curl -s -m 5 -X POST $IP/api/ctl -d '{"bl":0}' || echo "(web 不可达, 跳过)"

echo "== 2) 上传字库到 /QSPI: (限速 60/s) =="
python apps/applications/apollo-v2/tools/smp_udp.py $IP fswrite "$FONT" /QSPI:/font.bin --rate 60

echo "== 3) fontinstall + 重启 (串口 $PORT) =="
python3 - <<EOF
import serial, time, re
s = serial.Serial(); s.port='$PORT'; s.baudrate=115200; s.timeout=1
s.rts=False; s.dtr=False; s.open()
s.reset_input_buffer()
s.write(b'apollo fontinstall\r')
time.sleep(30)
s.write(b'apollo reboot\r')
time.sleep(25)
out = b''
end = time.time() + 30
while time.time() < end:
    d = s.read(4096)
    if d: out += d
t = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out.decode(errors='replace'))
for l in t.splitlines():
    if any(k in l for k in ('字库', 'fw 0.', 'LCD')):
        print(l.strip())
ok = '字库就绪' in t
print('字库安装成功!' if ok else '字库未就绪, 检查上方日志')
exit(0 if ok else 1)
EOF

echo "== 4) 开背光 =="
sleep 15
curl -s -m 5 -X POST $IP/api/ctl -d '{"bl":1}' || true
echo "完成. LCD 文件浏览页现在应能显示任意中文文件名."
