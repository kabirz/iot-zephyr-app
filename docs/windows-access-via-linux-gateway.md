# Windows 通过 Linux 网关访问不同网段设备

> **场景**: io-edge-hub 设备 (192.168.12.101) 直连 Linux 主机, Windows 上位机位于局域网,
> 两者不在同一网段, 需要 Windows 能访问设备 (Modbus TCP / FTP / UDP 配置 / 固件升级)。
> **日期**: 2026-08-16

---

## 目录

1. [网络拓扑](#1-网络拓扑)
2. [原理](#2-原理)
3. [Linux 侧配置](#3-linux-侧配置)
4. [Windows 侧配置](#4-windows-侧配置)
5. [验证](#5-验证)
6. [注意事项](#6-注意事项)

---

## 1. 网络拓扑

```
+--------------------------+          +----------------------+
|   Windows 上位机          |          |    Linux 主机         |
|   (局域网 10.237.61.x)    |  <--->  | wlan0: 10.237.61.190 |
|                          |  局域网  | enp3s0: 192.168.12.100|
+--------------------------+          +----------+-----------+
                                                |
                                                | 直连 (RJ45)
                                                v
                                        +------------------+
                                        | io-edge-hub 设备 |
                                        | 192.168.12.101   |
                                        +------------------+
```

| 角色 | 接口 / 地址 | 说明 |
|---|---|---|
| Windows 上位机 | 10.237.61.x | 局域网, 与 Linux 的 wlan0 同网段 |
| Linux wlan0 | 10.237.61.190/24 | 局域网网卡 (上行, 默认网关在此) |
| Linux enp3s0 | 192.168.12.100/24 | 直连设备的网卡 (静态 IP) |
| io-edge-hub | 192.168.12.101 | 设备默认静态 IP |

> 网卡名与 IP 是**示例**, 实际以 `ip addr` 输出为准。
> `a.sh` 中的 `192.168.1.10` 仅是早期示例, 不代表本环境。

## 2. 原理

设备网段 (192.168.12.0/24) 与 Windows 所在局域网 (10.237.61.0/24) 不同。
通过 Linux 主机做**网关转发**:

1. Linux 开启内核 IP 转发 (`net.ipv4.ip_forward=1`)
2. iptables 允许 `wlan0 <-> enp3s0` 双向转发
3. iptables `MASQUERADE` 把设备回程流量伪装成 Linux 的局域网出口 IP (NAT)
4. Windows 加**静态路由**: 到 192.168.12.101 的流量网关指向 Linux 的 wlan0 IP

这样 Windows 无需改自己的网段, 即可访问 192.168.12.101 上的 Modbus TCP (502)、
FTP (21)、UDP 配置 (8600) 等服务。

## 3. Linux 侧配置

对应仓库根目录的 `a.sh`, 步骤展开如下:

### 3.1 确认网卡与 IP

```bash
ip -4 -br addr show
# 确认: wlan0 = 局域网网卡, enp3s0 = 直连设备的网卡
```

### 3.2 开启内核 IP 转发 (a.sh 未包含, 必须执行)

```bash
# 临时生效
sudo sysctl -w net.ipv4.ip_forward=1

# 永久生效
echo 'net.ipv4.ip_forward=1' | sudo tee /etc/sysctl.d/99-forward.conf
sudo sysctl -p /etc/sysctl.d/99-forward.conf
```

> 不开启 `ip_forward` 时, iptables FORWARD 规则不生效, Windows 无法 ping 通设备。

### 3.3 iptables 转发 + MASQUERADE (即 a.sh 内容)

```bash
# 假设 wlan0 = 局域网网卡, enp3s0 = 连接设备的网卡

# MASQUERADE (自动适配出口 IP, 适合动态 IP 场景)
sudo iptables -t nat -A POSTROUTING -o enp3s0 -j MASQUERADE

# 允许转发流量 (双向)
sudo iptables -A FORWARD -i wlan0 -o enp3s0 -j ACCEPT
sudo iptables -A FORWARD -i enp3s0 -o wlan0 -m state --state ESTABLISHED,RELATED -j ACCEPT
```

### 3.4 (可选) 规则持久化

iptables 规则重启后丢失, 需保存:

```bash
# Arch (iptables-nft)
sudo iptables-save > /etc/iptables/iptables.rules

# 或用 systemd 启用持久化服务 (如 iptables / netfilter-persistent)
```

## 4. Windows 侧配置

以**管理员**身份打开 CMD:

```cmd
:: 添加静态路由: 到 192.168.12.101 的流量走 Linux 局域网 IP (此处 10.237.61.190)
route ADD 192.168.12.101 MASK 255.255.255.255 10.237.61.190 METRIC 1

:: 删除静态路由
route DELETE 192.168.12.101 MASK 255.255.255.255
```

> 网关地址填 **Linux 的局域网 IP** (wlan0 的地址), 不是 a.sh 注释里的示例值。
> 若设备 IP 变化, 相应调整目标地址或改用网段路由:
> `route ADD 192.168.12.0 MASK 255.255.255.0 10.237.61.190 METRIC 1`

## 5. 验证

```bash
# Windows 侧
ping 192.168.12.101

# 或直接用工具访问:
#   Modbus Poll 连接 192.168.12.101:502
#   浏览器/资源管理器 ftp://192.168.12.101/
```

## 6. 注意事项

1. **设备 IP 必须固定**: io-edge-hub 是静态 IP (192.168.12.101), 无需 DHCP。
2. **Linux 双网卡路由**: 确保默认网关在 wlan0 (上行), enp3s0 只有直连设备的 192.168.12.0/24 路由,
   否则 Linux 自身可能因多网卡路由冲突无法访问设备。
3. **防火墙**: 若 Linux 有额外防火墙 (如 nftables), 需放行 FORWARD 与相关端口。
4. **UDP 广播发现**: Windows 通过该 NAT 无法用 UDP 广播 (255.255.255.255) 发现设备,
   需用目标 IP 直接配置; 定向广播 (192.168.12.255) 取决于 MASQUERADE 配置。
5. **替换示例值**: 所有 IP/网卡名均为示例, 按实际环境调整。

---

## 相关文件

- 仓库根目录 `a.sh` — 精简版 iptables 配置
- 本文档为完整步骤 (含 a.sh 缺失的 `ip_forward` 开启与持久化说明)
