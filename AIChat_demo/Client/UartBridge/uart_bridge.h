/*
 * UART bridge for RV1106 ↔ T113 communication (C version).
 *
 * Protocol (same as T113 side):
 *   STX(0xAA) | LEN(2B, big-endian) | PAYLOAD(NB) | CRC16(2B) | ETX(0x55)
 *
 * Usage:
 *   uart_bridge_init("/dev/ttyS1", 3000000);
 *   uart_bridge_send("{\"face\":{\"x\":120}}");
 *   // In main loop:
 *   char buf[4096];
 *   while (uart_bridge_recv(buf, sizeof(buf)) > 0) { handle(buf); }
 */

#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  uart_bridge_init(const char *device, int baudrate);
void uart_bridge_close(void);
int  uart_bridge_is_open(void);

// Send JSON payload. Returns 0 on success, -1 on error.
int  uart_bridge_send(const char *json);

// Receive one JSON payload. Returns payload length, 0 if no frame, -1 on error.
int  uart_bridge_recv(char *buf, size_t buf_size);

// Statistics
uint32_t uart_bridge_tx_frames(void);
uint32_t uart_bridge_rx_frames(void);
uint32_t uart_bridge_rx_errors(void);

#ifdef __cplusplus
}
#endif

#endif // UART_BRIDGE_H
