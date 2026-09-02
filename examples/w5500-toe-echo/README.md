# w5500-toe-echo

Minimal TCP echo server running on the W5500's **hardware TCP/IP stack**
(TOE mode) through the socket-offload driver in `drivers/w5500_toe` — the
native Zephyr IP stack is completely bypassed.

- Board: `io_edge_f407vet6` (W5500 on SPI2, reset PD0, INT PD1)
- IP: static, from the devicetree node (`local-ip`, `netmask`, `gateway`)
- Service: TCP echo on port 4242

## Build & flash

```sh
west build -b io_edge_f407vet6 examples/w5500-toe-echo \
    -d examples/w5500-toe-echo/build
st-flash --connect-under-reset --reset write \
    examples/w5500-toe-echo/build/zephyr/zephyr.bin 0x8000000
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
regression test for the driver. Verified: 3000/3000 connect/close cycles
at ~190 conn/s with periodic echo checks, zero refusals.

Bandwidth (echo, full duplex, 2 KiB chunks): ~0.5 MB/s sustained. The
driver streams buffer-sized SPI transfers with a tight register loop
(the generic per-byte SPI path costs ~1.4 µs/byte and caps the link at
~0.7 MB/s raw). Note the W5500 answers a segment that does not fit the
receive buffer with an RST instead of dropping it, so keep client-side
in-flight data at or below the 2 KiB socket buffer (window depths of
more than one 2 KiB chunk can trip this on a fresh connection).

## Driver notes

- Backlog: a pool of two hardware sockets stays in LISTEN; when one
  completes a handshake it is detached into the pending-connection FIFO
  (a real backlog) and the pool is refilled immediately — armed from the
  INTn interrupt, never only inside accept(), so connect storms never
  hit a port without a listener (the chip would answer RST).
- Readiness is INTn-driven (int-gpios): Sn_IR events wake accept/recv in
  microseconds; no periodic polling runs at all.
- Closing is always Sn_CR=CLOSE (RST semantics, instant SOCK_CLOSED).
  DISCON is never used: as the active closer the W5500 parks the socket
  in FIN_WAIT/TIME_WAIT and a connect/disconnect storm exhausts all 8
  hardware sockets.
- hw_alloc only hands out sockets the chip confirms as SOCK_CLOSED, and
  sockets whose command engine repeatedly ignores OPEN/LISTEN are
  black-listed after three independent failures (a socket just closed
  may transiently ignore OPEN while its RST is still going out — that
  recovers by itself and is not black-listed).
- Presence check uses the RTR reset value (0x07D0): VERSIONR reads 0x04
  only on genuine parts and 0x00 on compatible clones, which work fine.
  The per-socket 2 KiB buffer sizes are written explicitly at init:
  this board's part comes up with the MEM_SIZE registers not at their
  documented reset values.
- `drivers/w5500_toe` and the in-tree MACRAW driver (`CONFIG_ETH_W5500`)
  are mutually exclusive by Kconfig and by compatible string.
