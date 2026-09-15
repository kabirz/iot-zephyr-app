# titan-mini-rpmsg — Titan Mini RA8P1 双核 RPMsg Echo

CM85（主核）+ CM33（副核）核间通信验证例程。

- **CM33 副核**（`remote/`）：RPMsg echo 服务器——收到任意消息原样回发；endpoint 绑定成功后先发一条开机问候 `hello from cm33`。
- **CM85 主核**（根目录）：拉起 CM33，注册同名 endpoint `titan`；通过 shell 命令 `cm33 send <msg>` 发消息给副核，收到回包后以 `CM33 says: ...` 打印。

## 硬件资源

| 资源 | 配置 |
|---|---|
| CM33 代码 | `code_mram_cm33` @ 0x20C0000，256 KB（与 CM85 镜像 0x02000000 互不冲突） |
| CM33 RAM | `sram1` @ 0x22100000，640 KB（CM33 专用） |
| IPC 共享内存 | sram1 顶部 4 KB：TX 0x2219F000（2 KB）+ RX 0x2219F800（2 KB），CM33 侧 sram1 相应缩为 636 KB |
| 核间通知 | 硬件邮箱 `mbox0`（`renesas,ra-ipc-mbox` 驱动），NVIC 槽位 94 |
| IPC 协议 | Zephyr IPC service，icmsg 后端 |
| CM33 调试控制台 | uart2（SCI2，H1 排针 3=TXD2/P801、2=RXD2/P802），115200-8N1；CM85 例程侧已禁用 sci2 让出 |

## 编译

需要 titan_mini board 的 **cm33 变体**（本仓库 `boards/titan_mini/`，随本例程一起提交）。

**一键双镜像（推荐，sysbuild）：**

```shell
cd apps
west build -p -b titan_mini/r7ka8p1kflcac/cm85 \
    examples/titan-mini-rpmsg -d build/titan_rpmsg --sysbuild
```

sysbuild 自动完成：

1. 把 `examples/titan-mini-rpmsg` 编成 **CM85 主镜像**（含 `CONFIG_SOC_RA_ENABLE_START_SECOND_CORE=y`，开机自动拉起 CM33）
2. 把 `examples/titan-mini-rpmsg/remote` 编成 **CM33 副镜像**（`remote/` 目录，板型由 `Kconfig.sysbuild` 自动选为 `titan_mini/r7ka8p1kflcac/cm33`）

只重编其中一个域：

```shell
west build -d build/titan_rpmsg --domain titan-mini-rpmsg   # 主核
west build -d build/titan_rpmsg --domain remote              # 副核
```

## 烧录

两个镜像独立烧录（各占一块 MRAM，互不覆盖）：

```shell
# 副核镜像（cm33, 0x20C0000 区域）
west flash -d build/titan_rpmsg --domain remote

# 主核镜像（cm85, 0x0200000 区域）
west flash -d build/titan_rpmsg --domain titan-mini-rpmsg
```

> 注意：烧录顺序**先 remote 再主核**——pyocd 按域擦写，两个域的擦除范围不重叠，顺序其实都行；但先烧副核可避免主核起来后（若 CM33 区域还是空/旧代码）产生一次无效启动。

烧完按复位（或 `pyocd reset`），两路串口各自输出：

- **CM85**：U18 排针 TXD1=P707 / RXD1=P706（本例程 `/dev/ttyUSB0`）
- **CM33**：H1 排针 1=GND、2=RXD2/P802、3=TXD2/P801（本例程 `/dev/ttyACM1`，经 ST-Link VCP）

## 运行与验证

**1. 开机自动握手**

上电后 CM85 拉起 CM33，endpoint 绑定成功时打印：

```
[00:00:01.xxx,xxx] <inf> titan_rpmsg: CM33 endpoint bound
[00:00:01.xxx,xxx] <inf> titan_rpmsg: RPMSG ready, try: cm33 send ping
[00:00:01.xxx,xxx] <inf> titan_rpmsg: CM33 says: hello from cm33
```

最后这行 `hello from cm33` 即 CM33 主动发来的开机问候——**看到它就说明双核链路已通**。

**2. shell 交互 echo**

```
uart:~$ cm33 send ping
sent 4 bytes to CM33
[00:01:10.618,000] <inf> titan_rpmsg: CM33 says: ping

uart:~$ cm33 send hello
sent 5 bytes to CM33
[00:01:32.618,000] <inf> titan_rpmsg: CM33 says: hello
```

**3. 排查**

- 看不到 `endpoint bound`：副核镜像没烧（`--domain remote`），或复位没生效（板卡 pyocd 软复位偶发失灵，按板上复位键）
- `ipc0 not ready`：overlay 未生效，确认用 `--sysbuild` 编译（remote/app.overlay 才会被应用）
- 收不到 echo 但能 send：检查两域镜像是否为同一次编译的产物（共享内存布局必须一致）

## 文件结构

```
titan-mini-rpmsg/
├── CMakeLists.txt          # CM85 主核 app
├── prj.conf                # 主核: ENABLE_START_SECOND_CORE + IPC + shell
├── app.overlay             # 主核: ipc0 节点 + 共享内存 + mbox0
├── src/main.c              # 主核: endpoint + 'cm33 send' shell 命令
├── Kconfig.sysbuild        # REMOTE_BOARD → titan_mini cm33 变体
├── sysbuild.cmake          # 声明 remote 域
└── remote/                 # CM33 副核 app
    ├── CMakeLists.txt
    ├── prj.conf            # 副核: IPC（无控制台）
    ├── app.overlay         # 副核: ipc0 (tx/rx 与主核镜像) + mbox0
    └── src/main.c          # 副核: echo 服务器 + 开机问候
```

## 参考时钟/内存映射

```
CM85: sram0 0x22000000 (1MB)   代码 0x02000000 (768KB MRAM)
CM33: sram1 0x22100000 (640KB) 代码 0x020C0000 (256KB MRAM)
IPC : 0x2219F000..0x221A0000 (sram1 顶部 4KB)
SDRAM: 0x68000000 (32MB, 双核可见，主核当前用作 LCD 帧缓冲)
```

参考：官方 BSP 的 RPMsg-lite 方案（`Titan_Mini_rpmsg` 工程）使用
0x22174000/64KB 共享内存，本例程改用 Zephyr IPC service + sram1 顶部 4KB，
与官方方案不冲突（如需对接 RT-Thread 侧，可将 tx/rx region 移到 0x22174000）。
