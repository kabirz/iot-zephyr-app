/*
 * LD_PRELOAD: 修复 ~/modbus 的 unit_id 问题
 *
 * main.cpp 用 libmodbus 但没调用 modbus_set_slave(), 导致默认
 * unit_id=0xff, 设备 (slave_id=1) 返回 Server Device Failure.
 * 劫持 modbus_connect: 连接成功后强制 modbus_set_slave(ctx, 1).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <modbus/modbus.h>

int modbus_connect(modbus_t *ctx)
{
	static int (*real_connect)(modbus_t *) = NULL;

	if (!real_connect) {
		real_connect = dlsym(RTLD_NEXT, "modbus_connect");
	}
	int rc = real_connect(ctx);

	if (rc == 0) {
		modbus_set_slave(ctx, 1); /* 设备实际 slave_id=1 */
	}
	return rc;
}
