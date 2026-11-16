#include "sprd_sensor.h"

static int sensor_error_gc5035(const char *what)
{
    int error = errno;
    perror(what);
    errno = error;
    return -1;
}

static void sensor_delay_gc5035(unsigned int usec)
{
    struct timespec delay = {.tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static int sensor_u32_gc5035(int fd, unsigned long op, uint32_t value)
{
    if (ioctl(fd, op, &value) < 0) return sensor_error_gc5035("sensor ioctl");
    return 0;
}

static int sensor_write_gc5035(int fd, uint16_t addr, uint8_t value)
{
    struct sensor_reg_bits_tag reg = {.reg_addr = addr, .reg_value = value, .reg_bits = 0};
    if (ioctl(fd, SENSOR_IO_I2C_WRITE, &reg) < 0) return sensor_error_gc5035("sensor write");
    return 0;
}

static int sensor_read_gc5035(int fd, uint16_t addr)
{
    struct sensor_reg_bits_tag reg = {.reg_addr = addr, .reg_bits = 0};
    if (ioctl(fd, SENSOR_IO_I2C_READ, &reg) < 0) return sensor_error_gc5035("sensor read");
    return reg.reg_value;
}

static int sensor_prepare_gc5035(int fd)
{
    uint16_t address = 0x37;
    if (sensor_u32_gc5035(fd, SENSOR_IO_PRI_KEY, SENSOR_IOC_PRIVATE_KEY) ||
        sensor_u32_gc5035(fd, SENSOR_IO_SET_ID, 0) ||
        ioctl(fd, SENSOR_IO_I2C_ADDR, &address) < 0) return sensor_error_gc5035("sensor address");
    if (sensor_u32_gc5035(fd, SENSOR_IO_PD, 0) || sensor_u32_gc5035(fd, SENSOR_IO_RST_LEVEL, 0) ||
        sensor_u32_gc5035(fd, SENSOR_IO_SET_MCLK, 0) || sensor_u32_gc5035(fd, SENSOR_IO_SET_AVDD, 13) ||
        sensor_u32_gc5035(fd, SENSOR_IO_SET_DVDD, 13) || sensor_u32_gc5035(fd, SENSOR_IO_SET_IOVDD, 13)) return -1;
    sensor_delay_gc5035(12000);
    sensor_delay_gc5035(1000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_SET_IOVDD, 7)) return -1;
    sensor_delay_gc5035(1000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_SET_DVDD, 10)) return -1;
    sensor_delay_gc5035(1000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_SET_AVDD, 3)) return -1;
    sensor_delay_gc5035(1000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_SET_MCLK, 24)) return -1;
    sensor_delay_gc5035(10000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_PD, 1)) return -1;
    sensor_delay_gc5035(1000);
    if (sensor_u32_gc5035(fd, SENSOR_IO_RST_LEVEL, 1)) return -1;
    sensor_delay_gc5035(6000);
    int high = sensor_read_gc5035(fd, 0xf0), low = sensor_read_gc5035(fd, 0xf1);
    if (high < 0 || low < 0) return -1;
    printf("sensor GC5035 ID: %02x%02x\n", high, low);
    if (high != 0x50 || low != 0x35) { errno = ENODEV; return -1; }
    static const struct { uint16_t addr; uint8_t value; } mode[] = {
        {0xfe, 0x00},
        {0x3e, 0x01},
        {0xfc, 0x01},
        {0xf4, 0x40},
        {0xf5, 0xe9},
        {0xf6, 0x14},
        {0xf8, 0x49},
        {0xf9, 0x82},
        {0xfa, 0x00},
        {0xfc, 0x81},
        {0xfe, 0x00},
        {0x36, 0x01},
        {0xd3, 0x87},
        {0x36, 0x00},
        {0x33, 0x00},
        {0xfe, 0x03},
        {0x01, 0xe7},
        {0xf7, 0x01},
        {0xfc, 0x8f},
        {0xfc, 0x8f},
        {0xfc, 0x8e},
        {0xfe, 0x00},
        {0xee, 0x30},
        {0x87, 0x18},
        {0xfe, 0x01},
        {0x8c, 0x90},
        {0xfe, 0x00},
        {0xfe, 0x00},
        {0x05, 0x02},
        {0x06, 0xda},
        {0x9d, 0x0c},
        {0x09, 0x00},
        {0x0a, 0x04},
        {0x0b, 0x00},
        {0x0c, 0x03},
        {0x0d, 0x07},
        {0x0e, 0xa8},
        {0x0f, 0x0a},
        {0x10, 0x30},
        {0x21, 0x48},
        {0x29, 0x58},
        {0x44, 0x20},
        {0x4e, 0x1a},
        {0x8c, 0x1a},
        {0x91, 0x80},
        {0x92, 0x28},
        {0x93, 0x20},
        {0x95, 0xa0},
        {0x96, 0xe0},
        {0xd5, 0xfc},
        {0x97, 0x28},
        {0x1f, 0x11},
        {0xce, 0x18},
        {0xd0, 0xb2},
        {0xfe, 0x02},
        {0x14, 0x01},
        {0x15, 0x02},
        {0xfe, 0x00},
        {0xfc, 0x88},
        {0xfe, 0x10},
        {0xfe, 0x00},
        {0xfc, 0x8e},
        {0xfe, 0x00},
        {0xfe, 0x00},
        {0xfe, 0x00},
        {0xfc, 0x88},
        {0xfe, 0x10},
        {0xfe, 0x00},
        {0xfc, 0x8e},
        {0xfe, 0x01},
        {0x49, 0x03},
        {0x4a, 0xff},
        {0x4b, 0xc0},
        {0xfe, 0x01},
        {0x4e, 0x3c},
        {0x44, 0x08},
        {0xfe, 0x01},
        {0x91, 0x00},
        {0x92, 0x08},
        {0x93, 0x00},
        {0x94, 0x07},
        {0x95, 0x07},
        {0x96, 0x98},
        {0x97, 0x0a},
        {0x98, 0x20},
        {0x99, 0x00},
        {0xfe, 0x03},
        {0x02, 0x57},
        {0x22, 0x06},
        {0x26, 0x08},
        {0x29, 0x06},
        {0x2b, 0x08},
        {0xfe, 0x01},
        {0x8c, 0x10},
        {0xfe, 0x00},
        {0x3e, 0x91},
    };
    for (size_t i = 0; i < sizeof mode / sizeof *mode; i++)
        if (sensor_write_gc5035(fd, mode[i].addr, mode[i].value)) return -1;
    return 0;
}
