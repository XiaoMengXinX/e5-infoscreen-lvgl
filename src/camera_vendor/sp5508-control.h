/* SP5508 sequences and full-resolution mode from the E5 vendor sensor library. */
static void sensor_power_off_sp5508(int fd)
{
    sensor_u32(fd, SENSOR_IO_RST_LEVEL, 1);
    sensor_u32(fd, SENSOR_IO_SET_MCLK, 0);
    sensor_u32(fd, SENSOR_IO_PD, 1);
    sensor_u32(fd, SENSOR_IO_SET_AVDD, 13);
    sensor_u32(fd, SENSOR_IO_SET_DVDD, 13);
    sensor_u32(fd, SENSOR_IO_SET_IOVDD, 13);
}

static int sensor_identify_sp5508(int fd)
{
    uint16_t address = 0x3c;
    if (sensor_u32(fd, SENSOR_IO_PRI_KEY, SENSOR_IOC_PRIVATE_KEY) ||
        sensor_u32(fd, SENSOR_IO_SET_ID, 0) ||
        ioctl(fd, SENSOR_IO_I2C_ADDR, &address) < 0) return -1;
    if (sensor_u32(fd, SENSOR_IO_PD, 1) || sensor_u32(fd, SENSOR_IO_RST_LEVEL, 1) ||
        sensor_u32(fd, SENSOR_IO_SET_MCLK, 0) || sensor_u32(fd, SENSOR_IO_SET_AVDD, 13) ||
        sensor_u32(fd, SENSOR_IO_SET_IOVDD, 13)) return -1;
    sensor_delay(1000);
    if (sensor_u32(fd, SENSOR_IO_SET_IOVDD, 7) || sensor_u32(fd, SENSOR_IO_SET_AVDD, 3) ||
        sensor_u32(fd, SENSOR_IO_SET_DVDD, 10)) return -1;
    sensor_delay(20000);
    if (sensor_u32(fd, SENSOR_IO_SET_MCLK, 24)) return -1;
    sensor_delay(10000);
    if (sensor_u32(fd, SENSOR_IO_PD, 1)) return -1;
    sensor_delay(20000);
    if (sensor_u32(fd, SENSOR_IO_RST_LEVEL, 1)) return -1;
    sensor_delay(20000);
    if (sensor_write8(fd, 0xfd, 0)) return -1;
    int high = sensor_read8(fd, 0x02);
    if (high < 0) return -1;
    int low = sensor_read8(fd, 0x03);
    if (low < 0) return -1;
    fprintf(stderr, "sensor SP5508 ID: %02x%02x\n", high, low);
    if (high != 0x55 || low != 0x08) { errno = ENODEV; return -1; }
    return 0;
}

static int sensor_configure_sp5508(int fd)
{
    static const struct { uint8_t addr, value; } mode[] = {
        {0xfd, 0x00},
        {0x2e, 0x24},
        {0x2f, 0x01},
        {0xfd, 0x01},
        {0x3f, 0x02},
        {0xa0, 0x00},
        {0x03, 0x06},
        {0x04, 0x36},
        {0x0e, 0x07},
        {0x0f, 0xc8},
        {0x0d, 0x10},
        {0x24, 0xff},
        {0x39, 0x0d},
        {0x31, 0x00},
        {0x01, 0x01},
        {0x11, 0x60},
        {0x33, 0xb0},
        {0x12, 0x03},
        {0x13, 0xd0},
        {0x45, 0x1e},
        {0x16, 0xea},
        {0x19, 0xf7},
        {0x1a, 0x5f},
        {0x1c, 0x0c},
        {0x1d, 0x06},
        {0x1e, 0x09},
        {0x20, 0x07},
        {0x2a, 0x0f},
        {0x2c, 0x10},
        {0x25, 0x0d},
        {0x26, 0x0d},
        {0x27, 0x08},
        {0x29, 0x01},
        {0x2d, 0x06},
        {0x55, 0x14},
        {0x56, 0x00},
        {0x57, 0x17},
        {0x59, 0x01},
        {0x5a, 0x04},
        {0x50, 0x10},
        {0x53, 0x0e},
        {0x6b, 0x10},
        {0x5c, 0x20},
        {0x5d, 0x00},
        {0x5e, 0x06},
        {0x66, 0x38},
        {0x68, 0x30},
        {0x71, 0x3f},
        {0x72, 0x05},
        {0x73, 0x3e},
        {0x81, 0x22},
        {0x8a, 0x66},
        {0x8b, 0x66},
        {0xc0, 0x02},
        {0xc1, 0x02},
        {0xc2, 0x02},
        {0xc3, 0x02},
        {0xc4, 0x82},
        {0xc5, 0x82},
        {0xc6, 0x82},
        {0xc7, 0x82},
        {0xfb, 0x43},
        {0xf0, 0x27},
        {0xf1, 0x27},
        {0xf2, 0x27},
        {0xf3, 0x27},
        {0xb1, 0xad},
        {0xb6, 0x42},
        {0xa1, 0x04},
        {0x8f, 0x20},
        {0x90, 0x07},
        {0x91, 0x98},
        {0xfd, 0x02},
        {0x34, 0xc8},
        {0x60, 0x99},
        {0x93, 0x03},
        {0xa0, 0x00},
        {0xa1, 0x08},
        {0xa2, 0x07},
        {0xa3, 0x98},
        {0xa4, 0x00},
        {0xa5, 0x08},
        {0xa6, 0x0a},
        {0xa7, 0x20},
        {0xfd, 0x04},
        {0x31, 0x4b},
        {0x32, 0x4b},
        {0xfd, 0x03},
        {0xc0, 0x01},
        {0xfd, 0x01},
    };
    for (size_t i = 0; i < sizeof mode / sizeof *mode; i++)
        if (sensor_write8(fd, mode[i].addr, mode[i].value)) return -1;
    return 0;
}

static int sensor_stream_sp5508(int fd, bool on)
{
    if (!on) sensor_delay(5000);
    if (sensor_write8(fd, 0xfd, 1) || sensor_write8(fd, 0xa0, on)) return -1;
    sensor_delay(on ? 30000 : 50000);
    return 0;
}

static int sensor_exposure_sp5508(int fd, unsigned lines, unsigned gain)
{
    /* Keep the vendor 1993-line frame period and its 12-line shutter margin.
     * Gain register 0x10 represents unity; register 0x01 latches each update. */
    if (sensor_write8(fd, 0xfd, 1) || sensor_write8(fd, 0x03, lines >> 8) ||
        sensor_write8(fd, 0x04, lines & 255) || sensor_write8(fd, 0x01, 1) ||
        sensor_write8(fd, 0x24, gain / 64) || sensor_write8(fd, 0x01, 1)) return -1;
    return 0;
}
