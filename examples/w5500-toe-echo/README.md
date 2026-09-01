# w5500-toe-echo

Minimal TCP echo server running on the W5500's **hardware TCP/IP stack**
(TOE mode) through the socket-offload driver in `drivers/w5500_toe` — the
native Zephyr IP stack is completely bypassed.

- Board: `io_edge_f407vet6` (W5500 on SPI2, reset PD0)
- IP: static, from the devicetree node (`local-ip`, `netmask`, `gateway`)
- Service: TCP echo on port 4242

## Build & flash

```sh
west build -b io_edge_f407vet6 examples/w5500-toe-echo
west flash
```

## Test from the development machine

Direct cable, host at 192.168.12.1/24:

```sh
ping 192.168.12.200          # ICMP answered inside the W5500 chip
nc 192.168.12.200 4242       # everything typed is echoed back
```

Reconnect immediately after killing `nc`: connections close with RST
(`Sn_CR=CLOSE`), so the listener never lingers in FIN/TIME_WAIT and the
next `nc` always connects — this example doubles as a disconnect-storm
regression test for the driver.

## Driver notes

- One client at a time: each W5500 hardware socket is one connection; the
  accepted connection takes the listener's socket and the listener re-arms
  on a free one (8 sockets total).
- `drivers/w5500_toe` and the in-tree MACRAW driver (`CONFIG_ETH_W5500`)
  are mutually exclusive by Kconfig and by compatible string.
