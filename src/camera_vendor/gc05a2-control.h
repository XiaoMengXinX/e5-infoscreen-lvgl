#include "sprd_sensor.h"
#include "gc05a2-mode.h"

static int sensor_error(const char *what)
{
	int error = errno;
	perror(what);
	errno = error;
	return -1;
}

static void sensor_delay(unsigned int usec)
{
	struct timespec delay = {.tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000};
	/* Cancellation signals must not shorten the sensor's power sequence. */
	while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static int sensor_u32(int fd, unsigned long op, uint32_t value)
{
	if (ioctl(fd, op, &value) < 0) return sensor_error("sensor ioctl");
	return 0;
}

static int sensor_write(int fd, uint16_t addr, uint8_t value)
{
	struct sensor_reg_bits_tag reg = { addr, value, 2 };
	if (ioctl(fd, SENSOR_IO_I2C_WRITE, &reg) < 0) {
		int error = errno;
		fprintf(stderr, "sensor write %#x: %s\n", addr, strerror(error));
		errno = error;
		return -1;
	}
	return 0;
}

static int sensor_read(int fd, uint16_t addr)
{
	struct sensor_reg_bits_tag reg = { .reg_addr = addr, .reg_bits = 2 };
	if (ioctl(fd, SENSOR_IO_I2C_READ, &reg) < 0) return sensor_error("sensor read");
	return reg.reg_value;
}

static void sensor_power_off(int fd)
{
	sensor_write(fd, 0x0100, 0);
	sensor_u32(fd, SENSOR_IO_RST_LEVEL, 0);
	sensor_u32(fd, SENSOR_IO_PD, 0);
	sensor_u32(fd, SENSOR_IO_SET_MCLK, 0);
	sensor_u32(fd, SENSOR_IO_SET_AVDD, 13);
	sensor_u32(fd, SENSOR_IO_SET_DVDD, 13);
	sensor_u32(fd, SENSOR_IO_SET_IOVDD, 13);
}

static int sensor_prepare_gc05a2(int fd)
{
	uint16_t address = 0x37;
	int high, low;
	if (sensor_u32(fd, SENSOR_IO_PRI_KEY, SENSOR_IOC_PRIVATE_KEY) ||
	    sensor_u32(fd, SENSOR_IO_SET_ID, 0)) return -1;
	if (ioctl(fd, SENSOR_IO_I2C_ADDR, &address) < 0) return sensor_error("sensor address");
	if (sensor_u32(fd, SENSOR_IO_PD, 0) || sensor_u32(fd, SENSOR_IO_RST_LEVEL, 0) ||
	    sensor_u32(fd, SENSOR_IO_SET_MCLK, 0) || sensor_u32(fd, SENSOR_IO_SET_AVDD, 13) ||
	    sensor_u32(fd, SENSOR_IO_SET_DVDD, 13) || sensor_u32(fd, SENSOR_IO_SET_IOVDD, 13)) return -1;
	sensor_delay(12000);
	if (sensor_u32(fd, SENSOR_IO_SET_IOVDD, 7)) return -1;
	sensor_delay(1000);
	if (sensor_u32(fd, SENSOR_IO_SET_DVDD, 10)) return -1;
	sensor_delay(1000);
	if (sensor_u32(fd, SENSOR_IO_SET_AVDD, 3)) return -1;
	sensor_delay(1000);
	if (sensor_u32(fd, SENSOR_IO_SET_MCLK, 24)) return -1;
	sensor_delay(10000);
	if (sensor_u32(fd, SENSOR_IO_PD, 1)) return -1;
	sensor_delay(1000);
	if (sensor_u32(fd, SENSOR_IO_RST_LEVEL, 1)) return -1;
	sensor_delay(6000);
	high = sensor_read(fd, 0x03f0);
	if (high < 0) return -1;
	low = sensor_read(fd, 0x03f1);
	if (low < 0) return -1;
	printf("sensor ID: %02x%02x\n", high, low);
	if (high != 0x05 || low != 0xa2) { errno = ENODEV; return -1; }
	if (sensor_write(fd, 0x0100, 0)) return -1;
	for (unsigned int i = 0; i < sizeof(gc05a2_mode) / sizeof(gc05a2_mode[0]); i++) {
		unsigned int a = gc05a2_mode[i].addr, v = gc05a2_mode[i].value;
		if (gc05a2_mode[i].bits == 16) {
			if (sensor_write(fd, a, v >> 8) || sensor_write(fd, a + 1, v & 255)) return -1;
		} else if (sensor_write(fd, a, v)) return -1;
	}
	/* Set the controls applied by the upstream driver after mode setup. */
	if (sensor_write(fd, 0x0340, 0x07) || sensor_write(fd, 0x0341, 0xf0) ||
	    sensor_write(fd, 0x0202, 0x07) || sensor_write(fd, 0x0203, 0x32) ||
	    sensor_write(fd, 0x0204, 0x04) || sensor_write(fd, 0x0205, 0x00)) return -1;
	printf("sensor mode configured; output width: %02x%02x\n",
		sensor_read(fd, 0x034c), sensor_read(fd, 0x034d));
	return 0;
}
