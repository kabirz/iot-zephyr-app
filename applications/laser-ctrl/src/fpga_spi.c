#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>
#include <laser-common.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_spi, LOG_LEVEL_INF);

enum {
	VERSION_GET_REG = 0,
	TIMESTAMP_REG,
	ENCODE1_GET_REG,
	ENCODE2_GET_REG,
	FPGA_RESET = 0xA5,
};

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DT_NODELABEL(spi_laser_fpga), SPI_WORD_SET(8));

struct encode_msg {
	uint8_t reg;
  	uint64_t timecount:31;
  	uint64_t single_num:17;
  	int64_t mutli_num:16;
} __PACKED;

static int spi_tranfer(void *tx_buffer, void *rx_buffer, size_t size)
{
	const struct spi_buf tx_buffers = {.buf = tx_buffer, .len = size};
	const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = size};

	const struct spi_buf_set tx_buf_set = {.buffers = &tx_buffers, .count = 1};
	const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};

	return spi_transceive_dt(&spi_spec, &tx_buf_set, &rx_buf_set);
}

uint32_t fpga_uint32_version(void)
{
	uint8_t rx_buf[5] = {0}, tx_buf[5] = {0};

	tx_buf[0] = VERSION_GET_REG;
	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}

	uint32_t version;
	memcpy(&version, &rx_buf[1], sizeof(version));
	return version;
}

uint32_t fpga_uint32_set(uint8_t reg, uint32_t val)
{
	uint8_t rx_buf[5] = {0}, tx_buf[5] = {0};

	tx_buf[0] = reg;
	memcpy(&tx_buf[1], &val, sizeof(val));

	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}

	return 0;
}

static int encode_data_get(uint8_t reg, struct encode_msg *rx)
{
	struct encode_msg tx_buf;

	tx_buf.reg = reg;
	if (spi_tranfer(&tx_buf, rx, sizeof(tx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}
	return 0;
}

int laser_get_encode_data(struct laser_encode_data *encode)
{
	static uint64_t local_previous_time = 0;
	static uint32_t fpga_previous_time1 = 0;
	static uint32_t fpga_previous_time2 = 0;
	uint32_t diff_l, diff_f;
	struct encode_msg msg1, msg2;
	int postion;
	uint64_t timestamp = k_uptime_get();

	/* get encode data */
	encode_data_get(ENCODE1_GET_REG, &msg1);
	encode_data_get(ENCODE2_GET_REG, &msg2);

	/* local time delta since last call */
	if (local_previous_time == 0) {
		diff_l = 0;
	} else if (timestamp < local_previous_time) {
		diff_l = timestamp + (UINT64_MAX - local_previous_time) + 1;
	} else {
		diff_l = timestamp - local_previous_time;
	}

	/* encode1: compare against msg1's own timestamp */
	if (fpga_previous_time1 == 0) {
		diff_f = 0;
	} else if (msg1.timecount < fpga_previous_time1) {
		diff_f = msg1.timecount + (BIT(31) - fpga_previous_time1);
	} else {
		diff_f = msg1.timecount - fpga_previous_time1;
	}

	postion = msg1.mutli_num * BIT(17) + msg1.single_num;

	if (diff_l > diff_f && (diff_l - diff_f) > 1000)
		encode->encode1 = 0;
	else {
		encode->encode1 = postion;
	}

	/* encode2: compare against msg2's own timestamp */
	if (fpga_previous_time2 == 0) {
		diff_f = 0;
	} else if (msg2.timecount < fpga_previous_time2) {
		diff_f = msg2.timecount + (BIT(31) - fpga_previous_time2);
	} else {
		diff_f = msg2.timecount - fpga_previous_time2;
	}

	postion = msg2.mutli_num * BIT(17) + msg2.single_num;

	if (diff_l > diff_f && (diff_l - diff_f) > 1000)
		encode->encode2 = 0;
	else
		encode->encode2 = postion;

	fpga_previous_time1 = msg1.timecount;
	fpga_previous_time2 = msg2.timecount;
	local_previous_time = timestamp;

	return 0;
}

static int fpga_spi_init(void)
{
	LOG_INF("fpga version: 0x%x", fpga_uint32_version());

	return 0;
}

SYS_INIT(fpga_spi_init, APPLICATION, 10);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <stdlib.h>
static int shell_version_get(const struct shell *ctx, size_t argc, char **argv)
{
	shell_print(ctx, "fpga version: 0x%x", fpga_uint32_version());

	return 0;
}

static int shell_encode_get(const struct shell *ctx, size_t argc, char **argv)
{
	struct encode_msg rx_msg;
	uint8_t reg;

	switch (strtol(argv[1], NULL, 0)) {
	case 1:
		reg = ENCODE1_GET_REG;
		break;
	case 2:
		reg = ENCODE2_GET_REG;
		break;
	default:
		shell_print(ctx, "Usage: %s <1/2>", argv[0]);
		return -1;
	}
	if (encode_data_get(reg, &rx_msg) == 0) {
		shell_print(ctx, "timestamp: %d ms, single num: %d, mutil num: %d, postion: %ld",
	      rx_msg.timecount, rx_msg.single_num, rx_msg.mutli_num, rx_msg.mutli_num * BIT(17) + rx_msg.single_num);
		shell_print(ctx, "RX:");
	      	shell_hexdump(ctx, (void *)&rx_msg, sizeof(rx_msg));
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_fpga_cmds,
			       SHELL_CMD_ARG(version, NULL,
					     "get fpga version\n"
					     "Usage: version",
					     shell_version_get, 1, 0),
			       SHELL_CMD_ARG(encode, NULL,
					     "get encode data\n"
					     "Usage: encode <1/2>",
					     shell_encode_get, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fpga_encode, &sub_fpga_cmds, "fpga encode commands", NULL);
#endif
