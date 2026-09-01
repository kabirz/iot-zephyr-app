/* W5500 hardware TCP/IP offload (TOE) socket driver
 *
 * Uses the W5500's on-chip protocol engine (8 hardware sockets, ARP/ICMP/
 * TCP/UDP in silicon) through Zephyr's socket-offload framework instead of
 * the in-tree MACRAW driver (drivers/ethernet/eth_w5500.c) which feeds raw
 * Ethernet frames into the native IP stack. The two drivers are mutually
 * exclusive (Kconfig + separate compatibles) and drive the same chip in
 * fundamentally different ways.
 *
 * Design notes:
 * - Closing is always Sn_CR=CLOSE (RST semantics, instant SOCK_CLOSED).
 *   DISCON is never used: as the active closer the W5500 parks the socket
 *   in FIN_WAIT/TIME_WAIT and a connect/disconnect storm exhausts all 8
 *   hardware sockets ("not closed" allocation failures).
 * - accept(): a W5500 socket transitions LISTEN -> ESTABLISHED in place, so
 *   the established hardware socket is handed to the accepted connection
 *   and the listener is re-armed on a fresh hardware socket.
 * - Blocking operations poll in bounded slices; a delayed-work notifier
 *   (CONFIG_W5500_TOE_POLL_PERIOD_MS) raises k_poll_signals for poll() and
 *   gives semaphores to wake blocked threads early. Correctness does not
 *   depend on the notifier (slice expiry re-checks the hardware).
 * - Socket buffer sizes keep the chip defaults (2 KiB TX + 2 KiB RX per
 *   socket = exactly the 16 KiB + 16 KiB on-chip pools).
 * - Single instance (like the in-tree eth_w5500): the socket-offload entry
 *   points are global, so instance 0 owns them.
 *
 * Limitations (inherent to the W5500 TOE):
 * - IPv4 only, no TLS, 8 sockets total, one pending connection per
 *   listening hardware socket, static IP from devicetree/Kconfig.
 */

#define DT_DRV_COMPAT wiznet_w5500_toe

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(w5500_toe, CONFIG_W5500_TOE_LOG_LEVEL);

/* ---------------- W5500 register map ---------------- */

/* 32-bit packed address: [20:16] block select, [15:0] register offset */
#define W5500_BSB(bsb, reg)		(((uint32_t)(bsb) << 16) | (reg))
#define BSB_COMMON			0x00
#define BSB_SOCK(n)			(0x01 + (n))
#define BSB_TXBUF(n)			(0x09 + (n))
#define BSB_RXBUF(n)			(0x11 + (n))

#define W5500_MR				0x0000
#define W5500_MR_RST				0x80 /* soft reset */
#define W5500_GAR				0x0001 /* gateway, 4B */
#define W5500_SUBR				0x0005 /* subnet mask, 4B */
#define W5500_SHAR				0x0009 /* MAC, 6B */
#define W5500_SIPR				0x000F /* source IP, 4B */
#define W5500_VERSIONR				0x001F /* reads 0x04 */
#define W5500_PHYCFGR				0x002E
#define W5500_PHYCFGR_LNK			BIT(7)

#define W5500_Sn_MR				0x0000
#define W5500_Sn_CR				0x0001
#define W5500_Sn_IR				0x0002
#define W5500_Sn_SR				0x0003
#define W5500_Sn_PORT				0x0004 /* 2B */
#define W5500_Sn_DIPR				0x000C /* 4B */
#define W5500_Sn_DPORT				0x0010 /* 2B */
#define W5500_Sn_TX_FSR				0x0020 /* 2B */
#define W5500_Sn_TX_WR				0x0024 /* 2B */
#define W5500_Sn_RX_RSR				0x0026 /* 2B */
#define W5500_Sn_RX_RD				0x0028 /* 2B */

#define Sn_MR_TCP_ND				0x21 /* TCP + no-delayed-ACK */
#define Sn_MR_UDP				0x02

#define Sn_CR_OPEN				0x01
#define Sn_CR_LISTEN				0x02
#define Sn_CR_CONNECT				0x04
#define Sn_CR_CLOSE				0x10
#define Sn_CR_SEND				0x20
#define Sn_CR_RECV				0x40

#define SR_CLOSED				0x00
#define SR_INIT					0x13
#define SR_LISTEN				0x14
#define SR_ESTABLISHED				0x17
#define SR_CLOSE_WAIT				0x1C
#define SR_UDP					0x22

#define W5500_NUM_SOCKETS			8
#define W5500_SOCK_BUF_SIZE			2048 /* default per-socket TX/RX */

/* ---------------- driver data ---------------- */

struct w5500_toe_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset;
	const char *local_ip;
	const char *netmask;
	const char *gateway;
};

struct w5500_toe_data;

struct w5500_sock {
	struct w5500_toe_data *data; /* back pointer, owner of the table */
	bool in_use;		/* net-side object allocated (fd alive) */
	int8_t hw;		/* attached hardware socket, -1 = none */
	int type;		/* SOCK_STREAM / SOCK_DGRAM */
	int proto;		/* IPPROTO_TCP / IPPROTO_UDP */
	bool nonblock;
	int recv_to_ms;		/* -1 = forever, 0 = immediate */
	int send_to_ms;
	bool shutdown_rd;
	bool shutdown_wr;
	int err;		/* pending errno for SO_ERROR */
	uint16_t bind_port;	/* port from bind() (host order), 0 = none */
	/* UDP default destination from connect() (host order) */
	bool peer_set;
	uint8_t peer_ip[4];
	uint16_t peer_port;

	/* readiness notification */
	struct k_sem sem_rd;	/* max count 1 */
	struct k_poll_signal sig_rd;
	bool notified;
};

struct w5500_toe_data {
	const struct device *dev;
	struct k_mutex lock;	/* guards chip access + socket table */
	struct net_if *iface;
	struct w5500_sock socks[W5500_NUM_SOCKETS];
	uint16_t next_ephemeral;
	struct k_work_delayable poll_work;
	bool worker_on;
	bool link_up;
	uint8_t mac[6];
};

/* ---------------- SPI register access (data->lock held) ---------------- */

static int w5500_read(const struct device *dev, uint32_t addr, uint8_t *data, size_t len)
{
	const struct w5500_toe_config *cfg = dev->config;
	uint8_t cmd[3] = {
		addr >> 8,
		addr,
		(addr >> 16) << 3, /* BSB, read (RWB=0), variable length */
	};
	const struct spi_buf tx_buf = { .buf = cmd, .len = sizeof(cmd) };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf rx_buf[2] = {
		{ .buf = NULL, .len = sizeof(cmd) },
		{ .buf = data, .len = len },
	};
	const struct spi_buf_set rx = { .buffers = rx_buf, .count = 2 };

	return spi_transceive_dt(&cfg->spi, &tx, &rx);
}

static int w5500_write(const struct device *dev, uint32_t addr, const uint8_t *data, size_t len)
{
	const struct w5500_toe_config *cfg = dev->config;
	uint8_t cmd[3] = {
		addr >> 8,
		addr,
		((addr >> 16) << 3) | BIT(2), /* BSB, write */
	};
	const struct spi_buf tx_buf[2] = {
		{ .buf = cmd, .len = sizeof(cmd) },
		{ .buf = (void *)data, .len = len },
	};
	const struct spi_buf_set tx = { .buffers = tx_buf, .count = 2 };

	return spi_write_dt(&cfg->spi, &tx);
}

static uint8_t w5500_rd8(const struct device *dev, uint32_t addr)
{
	uint8_t v = 0;

	w5500_read(dev, addr, &v, 1);
	return v;
}

static uint16_t w5500_rd16(const struct device *dev, uint32_t addr)
{
	uint8_t b[2] = { 0 };

	w5500_read(dev, addr, b, sizeof(b));
	return (uint16_t)((b[0] << 8) | b[1]);
}

static void w5500_wr8(const struct device *dev, uint32_t addr, uint8_t v)
{
	w5500_write(dev, addr, &v, 1);
}

static void w5500_wr16(const struct device *dev, uint32_t addr, uint16_t v)
{
	uint8_t b[2] = { v >> 8, v };

	w5500_write(dev, addr, b, sizeof(b));
}

static uint32_t sock_reg(int8_t hw, uint16_t reg)
{
	return W5500_BSB(BSB_SOCK(hw), reg);
}

static int w5500_cmd(const struct device *dev, int8_t hw, uint8_t cmd)
{
	w5500_wr8(dev, sock_reg(hw, W5500_Sn_CR), cmd);
	for (int i = 0; i < 100; i++) {
		if (w5500_rd8(dev, sock_reg(hw, W5500_Sn_CR)) == 0) {
			return 0;
		}
		k_busy_wait(10);
	}
	return -EIO;
}

static uint8_t sock_sr(const struct device *dev, int8_t hw)
{
	return w5500_rd8(dev, sock_reg(hw, W5500_Sn_SR));
}

static uint16_t sock_rsr(const struct device *dev, int8_t hw)
{
	return w5500_rd16(dev, sock_reg(hw, W5500_Sn_RX_RSR));
}

static uint16_t sock_fsr(const struct device *dev, int8_t hw)
{
	return w5500_rd16(dev, sock_reg(hw, W5500_Sn_TX_FSR));
}

/* Read/Write socket buffers splitting at the 2 KiB wrap. */
static int sock_buf_read(const struct device *dev, int8_t hw, bool tx,
			 uint16_t ptr, uint8_t *buf, size_t len)
{
	uint32_t base = W5500_BSB(tx ? BSB_TXBUF(hw) : BSB_RXBUF(hw), 0);
	uint16_t mask = W5500_SOCK_BUF_SIZE - 1;
	size_t first = MIN(len, (size_t)(mask + 1) - (ptr & mask));
	int ret;

	ret = w5500_read(dev, base + (ptr & mask), buf, first);
	if (ret < 0 || first == len) {
		return ret;
	}
	return w5500_read(dev, base, buf + first, len - first);
}

static int sock_buf_write(const struct device *dev, int8_t hw, uint16_t ptr,
			  const uint8_t *buf, size_t len)
{
	uint32_t base = W5500_BSB(BSB_TXBUF(hw), 0);
	uint16_t mask = W5500_SOCK_BUF_SIZE - 1;
	size_t first = MIN(len, (size_t)(mask + 1) - (ptr & mask));
	int ret;

	ret = w5500_write(dev, base + (ptr & mask), buf, first);
	if (ret < 0 || first == len) {
		return ret;
	}
	return w5500_write(dev, base, buf + first, len - first);
}

/* ---------------- socket table helpers (data->lock held) ---------------- */

static struct w5500_sock *sock_slot_alloc(struct w5500_toe_data *data)
{
	for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
		if (!data->socks[i].in_use) {
			struct w5500_sock *s = &data->socks[i];

			memset(s, 0, sizeof(*s));
			s->data = data;
			s->in_use = true;
			s->hw = -1;
			s->recv_to_ms = -1;
			s->send_to_ms = -1;
			return s;
		}
	}
	return NULL;
}

static int8_t hw_alloc(struct w5500_toe_data *data)
{
	bool taken[W5500_NUM_SOCKETS] = { 0 };

	for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
		struct w5500_sock *s = &data->socks[i];

		if (s->in_use && s->hw >= 0) {
			taken[s->hw] = true;
		}
	}
	for (int8_t i = 0; i < W5500_NUM_SOCKETS; i++) {
		if (!taken[i]) {
			return i;
		}
	}
	return -1;
}

static uint16_t alloc_ephemeral(struct w5500_toe_data *data)
{
	uint16_t p = data->next_ephemeral++;

	if (p < 49152) {
		data->next_ephemeral = 49152;
		p = 49152;
	}
	return p;
}

/* Hard close: Sn_CR=CLOSE is instant (RST when data pending) and never
 * parks the socket in TIME_WAIT — see file header. */
static void hw_close(const struct device *dev, int8_t hw)
{
	w5500_wr8(dev, sock_reg(hw, W5500_Sn_IR), 0xFF);
	(void)w5500_cmd(dev, hw, Sn_CR_CLOSE);
}

static int tcp_open_listen(const struct device *dev, struct w5500_sock *s)
{
	w5500_wr8(dev, sock_reg(s->hw, W5500_Sn_MR), Sn_MR_TCP_ND);
	w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_PORT), s->bind_port);
	if (w5500_cmd(dev, s->hw, Sn_CR_OPEN) < 0) {
		return -EIO;
	}
	return w5500_cmd(dev, s->hw, Sn_CR_LISTEN);
}

static int udp_open(const struct device *dev, struct w5500_sock *s)
{
	uint16_t port = s->bind_port ? s->bind_port
				     : alloc_ephemeral(s->data);

	w5500_wr8(dev, sock_reg(s->hw, W5500_Sn_MR), Sn_MR_UDP);
	w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_PORT), port);
	if (w5500_cmd(dev, s->hw, Sn_CR_OPEN) < 0) {
		return -EIO;
	}
	s->bind_port = port;
	return 0;
}

/* ---------------- readiness notification ---------------- */

/* data->lock held. */
static bool sock_readable(const struct device *dev, struct w5500_sock *s)
{
	if (s->hw < 0) {
		return false;
	}
	if (sock_rsr(dev, s->hw) > 0) {
		return true;
	}
	if (s->type == SOCK_STREAM) {
		uint8_t sr = sock_sr(dev, s->hw);

		if (sr == SR_CLOSED || sr == SR_CLOSE_WAIT) {
			return true; /* EOF: blocked recv() must return 0 */
		}
	}
	return false;
}

static void sock_notify(struct w5500_sock *s)
{
	if (s->notified) {
		return;
	}
	s->notified = true;
	k_poll_signal_raise(&s->sig_rd, 0);
	k_sem_give(&s->sem_rd);
}

static void sock_notify_drain(struct w5500_sock *s)
{
	s->notified = false;
	k_poll_signal_reset(&s->sig_rd);
}

/* ---------------- offload socket API ---------------- */

static const struct socket_op_vtable w5500_toe_fd_op_vtable;

static int w5500_toe_socket_create(int family, int type, int proto)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct w5500_toe_data *data = dev->data;
	struct w5500_sock *s;
	int fd;

	if (family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (proto != 0 && proto != IPPROTO_TCP && proto != IPPROTO_UDP) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	if (type == SOCK_STREAM) {
		proto = IPPROTO_TCP;
	} else if (type == SOCK_DGRAM) {
		proto = IPPROTO_UDP;
	} else {
		errno = ESOCKTNOSUPPORT;
		return -1;
	}

	fd = zvfs_reserve_fd();
	if (fd < 0) {
		return -1;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	s = sock_slot_alloc(data);
	if (s == NULL) {
		k_mutex_unlock(&data->lock);
		zvfs_free_fd(fd);
		errno = ENOBUFS;
		return -1;
	}
	s->type = type;
	s->proto = proto;
	k_sem_init(&s->sem_rd, 0, 1);
	k_poll_signal_init(&s->sig_rd);
	k_mutex_unlock(&data->lock);

	zvfs_finalize_typed_fd(fd, s, &w5500_toe_fd_op_vtable.fd_vtable,
			       ZVFS_MODE_IFSOCK);
	return fd;
}

static bool w5500_toe_is_supported(int family, int type, int proto)
{
	if (family != AF_INET && family != AF_UNSPEC) {
		return false;
	}
	return type == SOCK_STREAM || type == SOCK_DGRAM;
}

/* Blocking wait in bounded slices; returns false when the user timeout
 * expired. Waking early (notifier semaphore) or a slice expiry both mean
 * "re-check the hardware and come back". */
static bool wait_slice(struct w5500_sock *s, int timeout_ms, int64_t deadline)
{
	k_timeout_t slice;

	if (timeout_ms == 0) {
		return false;
	}
	if (timeout_ms < 0) {
		slice = K_MSEC(10);
	} else {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0) {
			return false;
		}
		slice = K_MSEC(MIN(remaining, 10));
	}
	(void)k_sem_take(&s->sem_rd, slice);
	return true;
}

static int64_t deadline_from(int timeout_ms)
{
	return k_uptime_get() + MAX(timeout_ms, 0);
}

static void fill_addr(struct net_sockaddr_in *sin, const uint8_t ip[4], uint16_t port)
{
	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);
	memcpy(&sin->sin_addr.s4_addr, ip, 4);
}

static int w5500_bind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	struct net_sockaddr_in *sin = (void *)addr;
	uint16_t port;
	int ret = 0;

	if (addr == NULL || addrlen < sizeof(*sin) || sin->sin_family != AF_INET) {
		errno = EINVAL;
		return -1;
	}
	port = ntohs(sin->sin_port);

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (s->type == SOCK_STREAM) {
		s->bind_port = port; /* applied at listen()/connect() time */
	} else {
		if (s->hw >= 0 && port != s->bind_port) {
			hw_close(dev, s->hw);
			s->hw = -1;
		}
		if (s->hw < 0) {
			s->hw = hw_alloc(s->data);
			if (s->hw < 0) {
				ret = -1;
				errno = ENOBUFS;
				goto out;
			}
			s->bind_port = port;
			if (udp_open(dev, s) < 0) {
				s->hw = -1;
				ret = -1;
				errno = EIO;
			}
		}
	}
out:
	k_mutex_unlock(&s->data->lock);
	return ret;
}

static int w5500_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	struct net_sockaddr_in *sin = (void *)addr;
	int ret = -1;

	if (addr == NULL || addrlen < sizeof(*sin) || sin->sin_family != AF_INET) {
		errno = EINVAL;
		return -1;
	}

	k_mutex_lock(&s->data->lock, K_FOREVER);

	if (s->type == SOCK_DGRAM) {
		/* UDP connect = default destination for send()/recv() */
		memcpy(s->peer_ip, sin->sin_addr.s4_addr, 4);
		s->peer_port = ntohs(sin->sin_port);
		s->peer_set = true;
		if (s->hw >= 0) {
			w5500_write(dev, sock_reg(s->hw, W5500_Sn_DIPR), s->peer_ip, 4);
			w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_DPORT), s->peer_port);
		}
		ret = 0;
		goto out;
	}

	if (s->hw >= 0) {
		errno = EISCONN;
		goto out;
	}
	s->hw = hw_alloc(s->data);
	if (s->hw < 0) {
		errno = ENOBUFS;
		goto out;
	}
	w5500_wr8(dev, sock_reg(s->hw, W5500_Sn_MR), Sn_MR_TCP_ND);
	w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_PORT), s->bind_port);
	if (w5500_cmd(dev, s->hw, Sn_CR_OPEN) < 0) {
		s->hw = -1;
		errno = EIO;
		goto out;
	}
	w5500_write(dev, sock_reg(s->hw, W5500_Sn_DIPR), sin->sin_addr.s4_addr, 4);
	w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_DPORT), ntohs(sin->sin_port));
	if (w5500_cmd(dev, s->hw, Sn_CR_CONNECT) < 0) {
		hw_close(dev, s->hw);
		s->hw = -1;
		errno = EIO;
		goto out;
	}

	int64_t deadline = deadline_from(s->send_to_ms);

	while (true) {
		uint8_t sr = sock_sr(dev, s->hw);

		if (sr == SR_ESTABLISHED) {
			ret = 0;
			goto out;
		}
		if (sr == SR_CLOSED) {
			/* SYN retries exhausted or refused */
			s->hw = -1;
			s->err = ECONNREFUSED;
			errno = ECONNREFUSED;
			goto out;
		}
		if (s->nonblock) {
			errno = EINPROGRESS;
			goto out;
		}
		if (!wait_slice(s, s->send_to_ms, deadline)) {
			s->err = ETIMEDOUT;
			errno = ETIMEDOUT;
			goto out;
		}
	}
out:
	k_mutex_unlock(&s->data->lock);
	return ret;
}

static int w5500_listen(void *obj, int backlog)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	int ret = -1;

	ARG_UNUSED(backlog); /* one pending connection per hardware socket */

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (s->type != SOCK_STREAM) {
		errno = EOPNOTSUPP;
		goto out;
	}
	if (s->hw >= 0) {
		uint8_t sr = sock_sr(dev, s->hw);

		if (sr != SR_LISTEN && sr != SR_INIT) {
			errno = EINVAL;
			goto out;
		}
	}
	if (s->hw < 0) {
		s->hw = hw_alloc(s->data);
		if (s->hw < 0) {
			errno = ENOBUFS;
			goto out;
		}
	}
	if (tcp_open_listen(dev, s) < 0) {
		s->hw = -1;
		errno = EIO;
		goto out;
	}
	ret = 0;
out:
	k_mutex_unlock(&s->data->lock);
	return ret;
}

static int sock_fd_finalize(struct w5500_sock *s)
{
	int fd = zvfs_reserve_fd();

	if (fd < 0) {
		return -1;
	}
	zvfs_finalize_typed_fd(fd, s, &w5500_toe_fd_op_vtable.fd_vtable,
			       ZVFS_MODE_IFSOCK);
	return fd;
}

static int w5500_accept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct w5500_sock *ls = obj;
	struct w5500_toe_data *data = ls->data;
	const struct device *dev = data->dev;
	struct w5500_sock *cs;
	int fd = -1;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (ls->type != SOCK_STREAM) {
		errno = EOPNOTSUPP;
		goto out;
	}
	int64_t deadline = deadline_from(ls->recv_to_ms);

	while (true) {
		if (ls->hw < 0) {
			/* stalled listener (pool was full): try to re-arm */
			ls->hw = hw_alloc(data);
			if (ls->hw >= 0 && tcp_open_listen(dev, ls) < 0) {
				ls->hw = -1;
			}
		}
		if (ls->hw >= 0) {
			uint8_t sr = sock_sr(dev, ls->hw);

			if (sr == SR_ESTABLISHED) {
				break;
			}
			if (sr != SR_LISTEN && sr != SR_INIT) {
				/* pending client died: re-arm and keep waiting */
				hw_close(dev, ls->hw);
				(void)tcp_open_listen(dev, ls);
			}
		}
		if (ls->nonblock) {
			errno = EAGAIN;
			goto out;
		}
		if (!wait_slice(ls, ls->recv_to_ms, deadline)) {
			errno = EAGAIN;
			goto out;
		}
	}

	/* The established hardware socket becomes the accepted connection;
	 * the listener is re-armed on a fresh hardware socket (may stall if
	 * the pool is exhausted; retried on the next accept). */
	cs = sock_slot_alloc(data);
	if (cs == NULL) {
		errno = ENOBUFS;
		goto out;
	}
	cs->hw = ls->hw;
	cs->type = ls->type;
	cs->proto = ls->proto;
	cs->recv_to_ms = ls->recv_to_ms;
	cs->send_to_ms = ls->send_to_ms;
	k_sem_init(&cs->sem_rd, 0, 1);
	k_poll_signal_init(&cs->sig_rd);

	ls->hw = hw_alloc(data);
	if (ls->hw >= 0 && tcp_open_listen(dev, ls) < 0) {
		ls->hw = -1;
	}

	fd = sock_fd_finalize(cs);
	if (fd < 0) {
		hw_close(dev, cs->hw);
		cs->in_use = false;
		goto out;
	}

	if (addr != NULL && addrlen != NULL) {
		struct net_sockaddr_in sin = { 0 };
		uint8_t ip[4];

		w5500_read(dev, sock_reg(cs->hw, W5500_Sn_DIPR), ip, 4);
		fill_addr(&sin, ip, w5500_rd16(dev, sock_reg(cs->hw, W5500_Sn_DPORT)));
		memcpy(addr, &sin, MIN(*addrlen, sizeof(sin)));
		*addrlen = sizeof(sin);
	}
out:
	k_mutex_unlock(&data->lock);
	return fd;
}

static ssize_t w5500_sendto(void *obj, const void *buf, size_t len, int flags,
			    const struct net_sockaddr *dest_addr, net_socklen_t addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	const uint8_t *src = buf;
	uint8_t udp_peer[4];
	uint16_t udp_port = 0;
	ssize_t sent = -1;
	bool nonblock = s->nonblock || (flags & ZSOCK_MSG_DONTWAIT);

	if (len == 0) {
		return 0;
	}

	k_mutex_lock(&s->data->lock, K_FOREVER);

	if (s->shutdown_wr) {
		errno = EPIPE;
		goto out;
	}

	if (s->type == SOCK_DGRAM) {
		if (dest_addr != NULL) {
			struct net_sockaddr_in *sin = (void *)dest_addr;

			if (addrlen < sizeof(*sin) || sin->sin_family != AF_INET) {
				errno = EINVAL;
				goto out;
			}
			memcpy(udp_peer, sin->sin_addr.s4_addr, 4);
			udp_port = ntohs(sin->sin_port);
		} else if (s->peer_set) {
			memcpy(udp_peer, s->peer_ip, 4);
			udp_port = s->peer_port;
		} else {
			errno = EDESTADDRREQ;
			goto out;
		}
		if (len > W5500_SOCK_BUF_SIZE - 8) {
			errno = EMSGSIZE;
			goto out;
		}
	}

	/* Open UDP lazily (socket() + sendto() without bind()) */
	if (s->type == SOCK_DGRAM && s->hw < 0) {
		s->hw = hw_alloc(s->data);
		if (s->hw < 0) {
			errno = ENOBUFS;
			goto out;
		}
		if (udp_open(dev, s) < 0) {
			s->hw = -1;
			errno = EIO;
			goto out;
		}
	}

	if (s->hw < 0) {
		errno = ENOTCONN;
		goto out;
	}

	int64_t deadline = deadline_from(s->send_to_ms);
	size_t off = 0;

	while (off < len) {
		uint8_t sr = sock_sr(dev, s->hw);
		size_t chunk = len - off;
		uint16_t fsr;
		uint16_t wr;

		if (s->type == SOCK_STREAM) {
			if (sr != SR_ESTABLISHED && sr != SR_CLOSE_WAIT) {
				s->err = EPIPE;
				errno = EPIPE;
				goto out_partial;
			}
			chunk = MIN(chunk, W5500_SOCK_BUF_SIZE);
		} else {
			if (sr != SR_UDP) {
				s->err = EPIPE;
				errno = EPIPE;
				goto out_partial;
			}
		}

		while ((fsr = sock_fsr(dev, s->hw)) < chunk) {
			if (nonblock) {
				errno = EAGAIN;
				goto out_partial;
			}
			if (!wait_slice(s, s->send_to_ms, deadline)) {
				s->err = ETIMEDOUT;
				errno = ETIMEDOUT;
				goto out_partial;
			}
		}

		if (s->type == SOCK_DGRAM) {
			w5500_write(dev, sock_reg(s->hw, W5500_Sn_DIPR), udp_peer, 4);
			w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_DPORT), udp_port);
		}

		wr = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_TX_WR));
		if (sock_buf_write(dev, s->hw, wr, src + off, chunk) < 0) {
			errno = EIO;
			goto out_partial;
		}
		w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_TX_WR), wr + (uint16_t)chunk);
		if (w5500_cmd(dev, s->hw, Sn_CR_SEND) < 0) {
			errno = EIO;
			goto out_partial;
		}
		off += chunk;
	}

	sent = len;
out:
	k_mutex_unlock(&s->data->lock);
	return sent;

out_partial:
	if (off > 0) {
		sent = off; /* POSIX allows partial success on stream sockets */
	}
	k_mutex_unlock(&s->data->lock);
	return sent;
}

static ssize_t w5500_recvfrom(void *obj, void *buf, size_t max_len, int flags,
			      struct net_sockaddr *src_addr, net_socklen_t *addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	ssize_t recvd = -1;
	bool nonblock = s->nonblock || (flags & ZSOCK_MSG_DONTWAIT);

	if (max_len == 0) {
		return 0;
	}

	k_mutex_lock(&s->data->lock, K_FOREVER);

	if (s->hw < 0) {
		errno = ENOTCONN;
		goto out;
	}
	if (s->shutdown_rd) {
		recvd = 0;
		goto out;
	}

	int64_t deadline = deadline_from(s->recv_to_ms);

	while (true) {
		uint16_t rsr = sock_rsr(dev, s->hw);

		if (s->type == SOCK_DGRAM) {
			if (rsr > 0) {
				uint8_t hdr[8];
				uint8_t ip[4];
				uint16_t rd;
				uint16_t dlen;
				uint16_t port;
				size_t take;

				rd = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_RX_RD));
				sock_buf_read(dev, s->hw, false, rd, hdr, sizeof(hdr));
				memcpy(ip, hdr, 4);
				port = (uint16_t)((hdr[4] << 8) | hdr[5]);
				dlen = (uint16_t)((hdr[6] << 8) | hdr[7]);
				take = MIN(max_len, dlen);
				sock_buf_read(dev, s->hw, false, rd + 8, buf, take);
				/* consume the whole datagram even if truncated */
				w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_RX_RD),
					   rd + 8 + dlen);
				(void)w5500_cmd(dev, s->hw, Sn_CR_RECV);
				if (sock_rsr(dev, s->hw) > 0) {
					sock_notify(s);
				} else {
					sock_notify_drain(s);
				}
				if (src_addr != NULL && addrlen != NULL) {
					struct net_sockaddr_in sin = { 0 };

					fill_addr(&sin, ip, port);
					memcpy(src_addr, &sin, MIN(*addrlen, sizeof(sin)));
					*addrlen = sizeof(sin);
				}
				recvd = take;
				goto out;
			}
		} else {
			if (rsr > 0) {
				uint16_t rd = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_RX_RD));
				size_t take = MIN(max_len, rsr);

				sock_buf_read(dev, s->hw, false, rd, buf, take);
				w5500_wr16(dev, sock_reg(s->hw, W5500_Sn_RX_RD),
					   rd + (uint16_t)take);
				(void)w5500_cmd(dev, s->hw, Sn_CR_RECV);
				if (sock_rsr(dev, s->hw) == 0) {
					sock_notify_drain(s);
				}
				if (src_addr != NULL && addrlen != NULL) {
					struct net_sockaddr_in sin = { 0 };
					uint8_t ip[4];

					w5500_read(dev, sock_reg(s->hw, W5500_Sn_DIPR), ip, 4);
					fill_addr(&sin, ip,
						  w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_DPORT)));
					memcpy(src_addr, &sin, MIN(*addrlen, sizeof(sin)));
					*addrlen = sizeof(sin);
				}
				recvd = take;
				goto out;
			}
			uint8_t sr = sock_sr(dev, s->hw);

			if (sr == SR_CLOSE_WAIT) {
				sock_notify_drain(s);
				recvd = 0; /* orderly EOF */
				goto out;
			}
			if (sr == SR_CLOSED) {
				sock_notify_drain(s);
				s->err = ECONNRESET;
				errno = ECONNRESET;
				goto out;
			}
		}

		if (nonblock) {
			errno = EAGAIN;
			goto out;
		}
		if (!wait_slice(s, s->recv_to_ms, deadline)) {
			errno = EAGAIN;
			goto out;
		}
	}
out:
	k_mutex_unlock(&s->data->lock);
	return recvd;
}

static int w5500_close(void *obj)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (s->hw >= 0) {
		hw_close(dev, s->hw); /* hard close: no TIME_WAIT, ever */
		s->hw = -1;
	}
	sock_notify_drain(s);
	s->in_use = false;
	k_mutex_unlock(&s->data->lock);
	return 0;
}

static int w5500_shutdown(void *obj, int how)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (how == ZSOCK_SHUT_RD || how == ZSOCK_SHUT_RDWR) {
		s->shutdown_rd = true;
	}
	if (how == ZSOCK_SHUT_WR || how == ZSOCK_SHUT_RDWR) {
		s->shutdown_wr = true;
	}
	if (how != ZSOCK_SHUT_RD && s->hw >= 0) {
		/* abort-style: the fd stays valid, transport is torn down */
		hw_close(dev, s->hw);
		s->hw = -1;
	}
	sock_notify(s);
	k_mutex_unlock(&s->data->lock);
	return 0;
}

static int w5500_getsockopt(void *obj, int level, int optname,
			    void *optval, net_socklen_t *optlen)
{
	struct w5500_sock *s = obj;

	if (level != SOL_SOCKET) {
		errno = ENOPROTOOPT;
		return -1;
	}

	switch (optname) {
	case SO_TYPE:
		if (*optlen < sizeof(int)) {
			errno = EINVAL;
			return -1;
		}
		*(int *)optval = s->type;
		*optlen = sizeof(int);
		return 0;
	case SO_ERROR:
		if (*optlen < sizeof(int)) {
			errno = EINVAL;
			return -1;
		}
		*(int *)optval = s->err;
		*optlen = sizeof(int);
		s->err = 0;
		return 0;
	case SO_RCVTIMEO:
	case SO_SNDTIMEO: {
		struct zsock_timeval *tv = optval;
		int ms = (optname == SO_RCVTIMEO) ? s->recv_to_ms : s->send_to_ms;

		if (*optlen < sizeof(*tv)) {
			errno = EINVAL;
			return -1;
		}
		tv->tv_sec = MAX(ms, 0) / 1000;
		tv->tv_usec = (MAX(ms, 0) % 1000) * 1000;
		*optlen = sizeof(*tv);
		return 0;
	}
	default:
		errno = ENOPROTOOPT;
		return -1;
	}
}

static int w5500_setsockopt(void *obj, int level, int optname,
			    const void *optval, net_socklen_t optlen)
{
	struct w5500_sock *s = obj;

	if (level != SOL_SOCKET) {
		/* TCP_* etc. accepted silently: TCP_NODELAY is baked into
		 * Sn_MR (ND bit) at open time. */
		return 0;
	}

	switch (optname) {
	case SO_RCVTIMEO:
	case SO_SNDTIMEO: {
		const struct zsock_timeval *tv = optval;
		int ms;

		if (optlen < sizeof(*tv)) {
			errno = EINVAL;
			return -1;
		}
		ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
		if (optname == SO_RCVTIMEO) {
			s->recv_to_ms = ms;
		} else {
			s->send_to_ms = ms;
		}
		return 0;
	}
	default:
		return 0;
	}
}

static int w5500_getsockname(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	struct net_sockaddr_in sin = { 0 };
	uint8_t ip[4] = { 0 };
	uint16_t port = 0;

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (s->hw >= 0) {
		port = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_PORT));
	}
	k_mutex_unlock(&s->data->lock);

	w5500_read(dev, W5500_BSB(BSB_COMMON, W5500_SIPR), ip, 4);
	fill_addr(&sin, ip, port);
	memcpy(addr, &sin, MIN(*addrlen, sizeof(sin)));
	*addrlen = sizeof(sin);
	return 0;
}

static int w5500_getpeername(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	struct net_sockaddr_in sin = { 0 };
	uint8_t ip[4];
	uint16_t port;

	k_mutex_lock(&s->data->lock, K_FOREVER);
	if (s->hw < 0) {
		k_mutex_unlock(&s->data->lock);
		errno = ENOTCONN;
		return -1;
	}
	w5500_read(dev, sock_reg(s->hw, W5500_Sn_DIPR), ip, 4);
	port = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_DPORT));
	k_mutex_unlock(&s->data->lock);

	fill_addr(&sin, ip, port);
	memcpy(addr, &sin, MIN(*addrlen, sizeof(sin)));
	*addrlen = sizeof(sin);
	return 0;
}

/* ---------------- fd vtable ---------------- */

static ssize_t w5500_read_vmeth(void *obj, void *buffer, size_t count)
{
	return w5500_recvfrom(obj, buffer, count, 0, NULL, NULL);
}

static ssize_t w5500_write_vmeth(void *obj, const void *buffer, size_t count)
{
	return w5500_sendto(obj, buffer, count, 0, NULL, 0);
}

static int w5500_ioctl_vmeth(void *obj, unsigned int request, va_list args)
{
	struct w5500_sock *s = obj;
	const struct device *dev = s->data->dev;
	uint16_t rsr;
	int n = 0;

	switch (request) {
	case ZFD_IOCTL_POLL_PREPARE: {
		struct zsock_pollfd *pfd;
		struct k_poll_event **pev;
		struct k_poll_event *pev_end;

		pfd = va_arg(args, struct zsock_pollfd *);
		pev = va_arg(args, struct k_poll_event **);
		pev_end = va_arg(args, struct k_poll_event *);

		if (pfd->events & ZSOCK_POLLIN) {
			if (*pev == pev_end) {
				errno = ENOMEM;
				return -1;
			}
			k_poll_event_init(*pev, K_POLL_TYPE_SIGNAL,
					  K_POLL_MODE_NOTIFY_ONLY, &s->sig_rd);
			(*pev)++;
		}
		return 0;
	}
	case ZFD_IOCTL_POLL_UPDATE: {
		struct zsock_pollfd *pfd;
		struct k_poll_event **pev;

		pfd = va_arg(args, struct zsock_pollfd *);
		pev = va_arg(args, struct k_poll_event **);

		k_mutex_lock(&s->data->lock, K_FOREVER);
		if (pfd->events & ZSOCK_POLLIN) {
			if ((*pev)->state != K_POLL_STATE_NOT_READY ||
			    sock_readable(dev, s)) {
				pfd->revents |= ZSOCK_POLLIN;
			}
			(*pev)++;
		}
		if (pfd->events & ZSOCK_POLLOUT) {
			if (s->shutdown_wr || s->err) {
				pfd->revents |= ZSOCK_POLLERR;
			} else {
				/* effectively always writable (2 KiB buffer) */
				pfd->revents |= ZSOCK_POLLOUT;
			}
		}
		if (s->hw >= 0 && s->type == SOCK_STREAM) {
			uint8_t sr = sock_sr(dev, s->hw);

			if ((sr == SR_CLOSED || sr == SR_CLOSE_WAIT) &&
			    sock_rsr(dev, s->hw) == 0) {
				pfd->revents |= ZSOCK_POLLHUP;
			}
		}
		if (s->err) {
			pfd->revents |= ZSOCK_POLLERR;
		}
		k_mutex_unlock(&s->data->lock);
		return 0;
	}
	case ZFD_IOCTL_FIONREAD: {
		int *avail = va_arg(args, int *);

		k_mutex_lock(&s->data->lock, K_FOREVER);
		if (s->hw >= 0) {
			rsr = sock_rsr(dev, s->hw);
			if (s->type == SOCK_DGRAM && rsr >= 8) {
				uint8_t hdr[8];
				uint16_t rd = w5500_rd16(dev, sock_reg(s->hw, W5500_Sn_RX_RD));

				/* peek the datagram length header */
				sock_buf_read(dev, s->hw, false, rd, hdr, sizeof(hdr));
				n = (hdr[6] << 8) | hdr[7];
			} else {
				n = rsr;
			}
		}
		k_mutex_unlock(&s->data->lock);
		*avail = n;
		return 0;
	}
	case ZVFS_F_GETFL:
		return s->nonblock ? ZVFS_O_NONBLOCK : 0;
	case ZVFS_F_SETFL: {
		int flags = va_arg(args, int);

		s->nonblock = (flags & ZVFS_O_NONBLOCK) != 0;
		return 0;
	}
	default:
		errno = EINVAL;
		return -1;
	}
}

static const struct socket_op_vtable w5500_toe_fd_op_vtable = {
	.fd_vtable = {
		.read = w5500_read_vmeth,
		.write = w5500_write_vmeth,
		.close = w5500_close,
		.ioctl = w5500_ioctl_vmeth,
	},
	.shutdown = w5500_shutdown,
	.bind = w5500_bind,
	.connect = w5500_connect,
	.listen = w5500_listen,
	.accept = w5500_accept,
	.sendto = w5500_sendto,
	.sendmsg = NULL,
	.recvfrom = w5500_recvfrom,
	.recvmsg = NULL,
	.getsockopt = w5500_getsockopt,
	.setsockopt = w5500_setsockopt,
	.getpeername = w5500_getpeername,
	.getsockname = w5500_getsockname,
};

NET_SOCKET_OFFLOAD_REGISTER(w5500_toe, CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY,
			    NET_AF_UNSPEC, w5500_toe_is_supported,
			    w5500_toe_socket_create);

/* ---------------- readiness worker ---------------- */

static void w5500_toe_poll_fn(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct w5500_toe_data *data =
		CONTAINER_OF(dwork, struct w5500_toe_data, poll_work);
	const struct device *dev = data->dev;
	bool link;

	k_mutex_lock(&data->lock, K_FOREVER);

	link = (w5500_rd8(dev, W5500_BSB(BSB_COMMON, W5500_PHYCFGR)) &
		W5500_PHYCFGR_LNK) != 0;
	if (link != data->link_up) {
		data->link_up = link;
		if (link) {
			net_if_carrier_on(data->iface);
		} else {
			net_if_carrier_off(data->iface);
		}
	}

	for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
		struct w5500_sock *s = &data->socks[i];

		if (s->in_use && sock_readable(dev, s)) {
			sock_notify(s);
		}
	}
	k_mutex_unlock(&data->lock);

	if (data->worker_on) {
		k_work_reschedule(&data->poll_work,
				  K_MSEC(CONFIG_W5500_TOE_POLL_PERIOD_MS));
	}
}

/* ---------------- net iface ---------------- */

static void w5500_toe_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct w5500_toe_data *data = dev->data;

	data->iface = iface;
	net_if_socket_offload_set(iface, w5500_toe_socket_create);
	net_if_set_link_addr(iface, data->mac, sizeof(data->mac),
			     NET_LINK_ETHERNET);

#if defined(CONFIG_NET_IPV4)
	{
		const struct w5500_toe_config *cfg = dev->config;
		struct in_addr addr;
		struct in_addr mask;

		if (net_addr_pton(AF_INET, cfg->local_ip, &addr) == 0 &&
		    net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
			if (net_addr_pton(AF_INET, cfg->netmask, &mask) == 0) {
				net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
			}
		}
		if (net_addr_pton(AF_INET, cfg->gateway, &addr) == 0) {
			net_if_ipv4_set_gw(iface, &addr);
		}
	}
#endif

	net_if_carrier_off(iface);
}

static struct offloaded_if_api w5500_toe_offload_api = {
	.iface_api.init = w5500_toe_iface_init,
};

/* ---------------- device init ---------------- */

static int w5500_toe_init(const struct device *dev)
{
	const struct w5500_toe_config *cfg = dev->config;
	struct w5500_toe_data *data = dev->data;
	struct in_addr addr;
	uint8_t version;
	int ret;

	k_mutex_init(&data->lock);
	data->dev = dev;
	data->link_up = false;
	for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
		data->socks[i].hw = -1;
	}
	data->next_ephemeral = 49152;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("SPI device not ready");
		return -ENODEV;
	}

	if (cfg->reset.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("reset gpio config failed (%d)", ret);
			return ret;
		}
		k_msleep(2); /* RST low >= 500 us */
		gpio_pin_set_dt(&cfg->reset, 0);
		k_msleep(50); /* power/PHY settle */
	} else {
		/* soft reset */
		w5500_wr8(dev, W5500_BSB(BSB_COMMON, W5500_MR), W5500_MR_RST);
		k_msleep(10);
		w5500_wr8(dev, W5500_BSB(BSB_COMMON, W5500_MR), 0x00);
		k_msleep(50);
	}

	version = w5500_rd8(dev, W5500_BSB(BSB_COMMON, W5500_VERSIONR));
	if (version != 0x04) {
		LOG_ERR("W5500 not detected (VERSIONR=0x%02x)", version);
		return -ENODEV;
	}

	/* MAC: devicetree property or Wiznet OUI + random node id */
	if (DT_INST_NODE_HAS_PROP(0, local_mac_address)) {
		static const uint8_t mac[] = DT_INST_PROP(0, local_mac_address);

		memcpy(data->mac, mac, sizeof(data->mac));
	} else {
		sys_rand_get(&data->mac[3], 3);
		data->mac[0] = 0x00;
		data->mac[1] = 0x08;
		data->mac[2] = 0xdc;
	}
	w5500_write(dev, W5500_BSB(BSB_COMMON, W5500_SHAR), data->mac, 6);

	/* static IPv4 configuration into the chip's own registers */
	if (cfg->local_ip == NULL) {
		LOG_ERR("devicetree node needs a local-ip property");
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, cfg->local_ip, &addr) == 0) {
		w5500_write(dev, W5500_BSB(BSB_COMMON, W5500_SIPR),
			    (const uint8_t *)&addr, 4);
	} else {
		LOG_ERR("bad local-ip \"%s\"", cfg->local_ip);
		return -EINVAL;
	}
	if (cfg->netmask != NULL &&
	    net_addr_pton(AF_INET, cfg->netmask, &addr) == 0) {
		w5500_write(dev, W5500_BSB(BSB_COMMON, W5500_SUBR),
			    (const uint8_t *)&addr, 4);
	}
	if (cfg->gateway != NULL &&
	    net_addr_pton(AF_INET, cfg->gateway, &addr) == 0) {
		w5500_write(dev, W5500_BSB(BSB_COMMON, W5500_GAR),
			    (const uint8_t *)&addr, 4);
	}

	k_work_init_delayable(&data->poll_work, w5500_toe_poll_fn);
	data->worker_on = true;
	k_work_reschedule(&data->poll_work,
			  K_MSEC(CONFIG_W5500_TOE_POLL_PERIOD_MS));

	LOG_INF("W5500 TOE up: ip %s mac %02x:%02x:%02x:%02x:%02x:%02x",
		cfg->local_ip, data->mac[0], data->mac[1], data->mac[2],
		data->mac[3], data->mac[4], data->mac[5]);
	return 0;
}

/* Single instance (like the in-tree eth_w5500): the socket-offload entry
 * points are global, so instance 0 owns them. */
static struct w5500_toe_data w5500_toe_0_data = {
	.socks = {
		{ .hw = -1 }, { .hw = -1 }, { .hw = -1 }, { .hw = -1 },
		{ .hw = -1 }, { .hw = -1 }, { .hw = -1 }, { .hw = -1 },
	},
};

static const struct w5500_toe_config w5500_toe_0_config = {
	.spi = SPI_DT_SPEC_INST_GET(0, SPI_WORD_SET(8) | SPI_TRANSFER_MSB),
	.reset = GPIO_DT_SPEC_INST_GET_OR(0, reset_gpios, { 0 }),
	.local_ip = COND_CODE_1(DT_INST_NODE_HAS_PROP(0, local_ip),
				(DT_INST_PROP(0, local_ip)), (NULL)),
	.netmask = COND_CODE_1(DT_INST_NODE_HAS_PROP(0, netmask),
			       (DT_INST_PROP(0, netmask)), (NULL)),
	.gateway = COND_CODE_1(DT_INST_NODE_HAS_PROP(0, gateway),
			       (DT_INST_PROP(0, gateway)), (NULL)),
};

NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, w5500_toe_init,
				  NULL, &w5500_toe_0_data, &w5500_toe_0_config,
				  CONFIG_W5500_TOE_INIT_PRIORITY,
				  &w5500_toe_offload_api, NET_ETH_MTU);
