// spidev_opt
/*
 * Cross-compile with cross-gcc -I/path/to/cross-kernel/include
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PACKET_DATA_LEN    2048
#define PACKET_TOTAL_LEN   2056
#define FRAME_HEADER_HIGH  0x55
#define FRAME_HEADER_LOW   0xAA

enum
{
    SEND_STATUS_SUCCESS = 0,
    SEND_STATUS_FAILED = 1,
    SEND_STATUS_RETRY = 2,
};

static void pabort(const char *s)
{
	if (errno != 0)
		perror(s);
	else
		printf("%s\n", s);

	abort();
}

static const char *device = "/dev/spidev1.0";
static uint32_t mode = 0;
static uint8_t bits = 8;
static char *input_file;
static uint32_t speed = 15000000;

int set_gpio(int gpio, int value) {
    int ret;
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);

    int fd = open(path, O_WRONLY);
    if (fd < 0) 
        ret = -1;

    if (value)
        ret = write(fd, "1", 1);
    else
        ret = write(fd, "0", 1);

    close(fd);
    return ret;
}

// ---- CRC32 实现 ----
uint32_t crc32_table[256];

void init_crc32_table(void) {
    uint32_t poly = 0xEDB88320;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ poly) : (crc >> 1);
        crc32_table[i] = crc;
    }
}

uint32_t calc_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

// ---- 发送数据包 ----
int send_packet(int fd, uint16_t pkt_id, const uint8_t *data) 
{
    int ret;    
    uint8_t tx_buf[PACKET_TOTAL_LEN];
    memset(tx_buf, 0, sizeof(tx_buf));

    ret = set_gpio(59,0);
    if(ret <=  0)
        return -1;
    // 帧头
    tx_buf[0] = FRAME_HEADER_HIGH;
    tx_buf[1] = FRAME_HEADER_LOW;

    // 包编号（大端）
    tx_buf[2] = (pkt_id >> 8) & 0xFF;
    tx_buf[3] = pkt_id & 0xFF;

    memcpy(&tx_buf[4], data, PACKET_DATA_LEN);

    // CRC32
    uint32_t crc = calc_crc32(&tx_buf[4], PACKET_DATA_LEN);
    //  uint32_t crc  = 0;
    printf("Packet crc %08X\n",  crc);
    tx_buf[2055] = (crc >> 24) & 0xFF;
    tx_buf[2054] = (crc >> 16) & 0xFF;
    tx_buf[2053] = (crc >> 8) & 0xFF;
    tx_buf[2052] = crc & 0xFF;

   uint8_t dummy = 0x00, ack = 0;
    
    usleep(1000);
    if (write(fd, tx_buf, PACKET_TOTAL_LEN) != PACKET_TOTAL_LEN) {
        perror("write failed");
        return -1;
    }

    ret = set_gpio(59,1);
    if(ret <= 0)
        return -1;

    while(ack == 0xff || ack == 0x0)
    {
        
        if (write(fd, &dummy, 1) != 1 || read(fd, &ack, 1) != 1) {              //write(fd, &dummy, 1) != 1 || 
            perror("ack failed");
            return -1;
        }
        printf("======Packet ack read %04d  %02X ACK received\n", pkt_id, ack);
        if (ack == 0x06) {
            printf("Packet %04d  ACK received\n", pkt_id);
            return 0;
        } else if (ack == 0x15){
            printf("Packet %04d  NACK received (0x%02X)\n", pkt_id, ack);
            return -1;
        }
        // usleep(500);
    }
    return 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s \n", prog);
	puts("  -D --device   device to use (default /dev/spidev1.1)\n"
	     "  -s --speed    max speed (Hz)\n"
	     "  -i --input    input data from a file (e.g. \"test.bin\")\n");
	exit(1);
}

static void parse_opts(int argc, char *argv[])
{
	while (1) {
		static const struct option lopts[] = {
			{ "device",  1, 0, 'D' },
			{ "speed",   1, 0, 's' },
			{ "input",   1, 0, 'i' },
			{ NULL, 0, 0, 0 },
		};
		int c;

		c = getopt_long(argc, argv, "D:s:i:", lopts, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 'D':
			device = optarg;
			break;
		case 's':
			speed = atoi(optarg);
			break;
		case 'i':
			input_file = optarg;
			break;
		default:
			print_usage(argv[0]);
		}
	}
}

uint8_t transfer_file(int fd, char *filename)
{
    uint8_t buffer[PACKET_DATA_LEN];
    uint16_t pkt_id = 1;
    uint8_t send_status = SEND_STATUS_SUCCESS;
    struct timespec last_stat;
    struct timespec current;

	clock_gettime(CLOCK_MONOTONIC, &last_stat);
    

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return SEND_STATUS_FAILED;
    }

    while (1) {
        size_t len = fread(buffer, 1, PACKET_DATA_LEN, fp);
        if (len == 0) 
        {
            send_status = SEND_STATUS_FAILED;
            break;
        }

        if (len < PACKET_DATA_LEN)
            memset(buffer + len, 0xFF, PACKET_DATA_LEN - len);

        int retry = 0;
        while (retry < 3) {
            if (send_packet(fd, pkt_id, buffer) == 0)
            {
                break;
            }
            else {
                retry++;
                usleep(20000);
            }
        }

        if (retry == 3) {
            printf(" Failed to send packet %d after 3 retries.\n", pkt_id);
            send_status = SEND_STATUS_FAILED;
            break;
        }
        pkt_id++;
        // usleep(10000);  // 可调节延迟
    }

    printf(" All packets sent. Total: %d\n", pkt_id);
    clock_gettime(CLOCK_MONOTONIC, &current);

    uint64_t time = current.tv_sec - last_stat.tv_sec;

    if (current.tv_sec - last_stat.tv_sec > 0) {
        printf(" packets sent speed %ld kb/s  time %ld \n", pkt_id*2/time,time);
    }

    fclose(fp);
    return send_status;
}

int main(int argc, char *argv[])
{
	int ret = 0;
	int fd;

	parse_opts(argc, argv);

	fd = open(device, O_RDWR);
	if (fd < 0)
		pabort("can't open device");

	/*
	 * spi mode
	 */
	ret = ioctl(fd, SPI_IOC_WR_MODE32, &mode);
	if (ret == -1)
		pabort("can't set spi mode");

	ret = ioctl(fd, SPI_IOC_RD_MODE32, &mode);
	if (ret == -1)
		pabort("can't get spi mode");

	/*
	 * bits per word
	 */
	ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	if (ret == -1)
		pabort("can't set bits per word");

	ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
	if (ret == -1)
		pabort("can't get bits per word");

	/*
	 * max speed hz
	 */
	ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	if (ret == -1)
		pabort("can't set max speed hz");

	ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
	if (ret == -1)
		pabort("can't get max speed hz");

	printf("spi mode: 0x%x\n", mode);
	printf("bits per word: %u\n", bits);
	printf("max speed: %u Hz (%u kHz)\n", speed, speed/1000);

	if (input_file)
    {
        init_crc32_table();
		transfer_file(fd, input_file);
        
    }
	close(fd);

	return ret;
}
