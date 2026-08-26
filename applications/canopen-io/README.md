# canopen-io

io_edge_f407vet6 上的纯 CANopen IO 计测节点（io-edge-hub 的 CANopen 版）。
复刻 io-edge-hub 的计测器寄存器模型并全部经 CANopen 暴露：SDO 读写、
TPDO 周期/事件上报、RPDO 控制；固件下载走 CiA 302-2（0x1F50/0x1F51）。

构建:

    west build -b io_edge_f407vet6 applications/canopen-io --sysbuild

详见 USER_GUIDE.md（对象字典、PDO 映射、升级流程）。
