#include "uart_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int fd = -1;
static uint8_t rx_buf[65536];
static size_t rx_len = 0;
static uint32_t tx_frames = 0;
static uint32_t rx_frames = 0;
static uint32_t rx_errors = 0;

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

static speed_t baud_to_speed(int baudrate)
{
    switch (baudrate) {
        case 3000000: return B3000000;
        case 1500000: return B1500000;
        case 921600:  return B921600;
        case 460800:  return B460800;
        case 115200:  return B115200;
        default:      return B115200;
    }
}

int uart_bridge_init(const char *device, int baudrate)
{
    uart_bridge_close();

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "uart_bridge: open %s failed: %s\n", device, strerror(errno));
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) < 0) {
        fprintf(stderr, "uart_bridge: tcgetattr failed\n");
        uart_bridge_close();
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;

    speed_t spd = baud_to_speed(baudrate);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "uart_bridge: tcsetattr failed\n");
        uart_bridge_close();
        return -1;
    }

    rx_len = 0;
    printf("uart_bridge: opened %s at %d baud\n", device, baudrate);
    return 0;
}

void uart_bridge_close(void)
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    rx_len = 0;
}

int uart_bridge_is_open(void)
{
    return fd >= 0;
}

int uart_bridge_send(const char *json)
{
    if (fd < 0 || !json) return -1;

    size_t pay_len = strlen(json);
    if (pay_len > 65535) return -1;

    uint8_t frame[1 + 2 + pay_len + 2 + 1];
    frame[0] = 0xAA;
    frame[1] = (uint8_t)(pay_len >> 8);
    frame[2] = (uint8_t)(pay_len);
    memcpy(frame + 3, json, pay_len);
    uint16_t crc = crc16((const uint8_t *)json, pay_len);
    frame[3 + pay_len]     = (uint8_t)(crc);
    frame[3 + pay_len + 1] = (uint8_t)(crc >> 8);
    frame[3 + pay_len + 2] = 0x55;

    size_t off = 0;
    while (off < sizeof(frame)) {
        ssize_t n = write(fd, frame + off, sizeof(frame) - off);
        if (n < 0 && errno == EAGAIN) continue;
        if (n <= 0) return -1;
        off += n;
    }
    tx_frames++;
    return 0;
}

int uart_bridge_recv(char *buf, size_t buf_size)
{
    if (fd < 0 || !buf || buf_size == 0) return -1;

    // Drain UART
    uint8_t tmp[256];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (rx_len + n > sizeof(rx_buf)) {
            memmove(rx_buf, rx_buf + rx_len / 2, (rx_len + 1) / 2);
            rx_len = (rx_len + 1) / 2;
        }
        memcpy(rx_buf + rx_len, tmp, n);
        rx_len += n;
    }

    // Search for STX
    while (rx_len >= 7) {
        uint8_t *stx = memchr(rx_buf, 0xAA, rx_len);
        if (!stx) { rx_len = 0; return 0; }

        size_t offset = stx - rx_buf;
        if (offset > 0) {
            memmove(rx_buf, stx, rx_len - offset);
            rx_len -= offset;
        }

        uint16_t pay_len = ((uint16_t)rx_buf[1] << 8) | rx_buf[2];
        size_t frame_len = 1 + 2 + pay_len + 2 + 1;
        if (pay_len > 32768) {
            memmove(rx_buf, rx_buf + 1, --rx_len);
            rx_errors++;
            continue;
        }

        if (rx_len < frame_len) return 0; // incomplete

        if (rx_buf[frame_len - 1] != 0x55) {
            memmove(rx_buf, rx_buf + 1, --rx_len);
            rx_errors++;
            continue;
        }

        uint16_t got = (uint16_t)rx_buf[3 + pay_len] |
                       ((uint16_t)rx_buf[3 + pay_len + 1] << 8);
        if (crc16(rx_buf + 3, pay_len) != got) {
            memmove(rx_buf, rx_buf + 1, --rx_len);
            rx_errors++;
            continue;
        }

        size_t copy_len = pay_len < buf_size - 1 ? pay_len : buf_size - 1;
        memcpy(buf, rx_buf + 3, copy_len);
        buf[copy_len] = '\0';

        memmove(rx_buf, rx_buf + frame_len, rx_len - frame_len);
        rx_len -= frame_len;
        rx_frames++;
        return (int)copy_len;
    }
    return 0;
}

uint32_t uart_bridge_tx_frames(void) { return tx_frames; }
uint32_t uart_bridge_rx_frames(void) { return rx_frames; }
uint32_t uart_bridge_rx_errors(void) { return rx_errors; }
