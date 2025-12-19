// llm code begins

#ifndef SHAM_H
#define SHAM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/md5.h>
#include <stdarg.h>

// --- Configuration ---
#define PAYLOAD_SIZE 1024
#define SENDER_WINDOW_SIZE 10      // Fixed sender sliding window size (in packets)
#define RECEIVER_BUFFER_SIZE 16384 // Receiver buffer size (in bytes)
#define RTO_MS 500                 // Retransmission Timeout in milliseconds

// --- S.H.A.M. Flags ---
#define SYN 0x1
#define ACK 0x2
#define FIN 0x4

// --- S.H.A.M. Packet Structure ---
#pragma pack(push, 1)
typedef struct sham_header {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t flags;
    uint16_t window_size;
} sham_header;

typedef struct sham_packet {
    sham_header header;
    char data[PAYLOAD_SIZE];
} sham_packet;
#pragma pack(pop)

// --- Global Logging Variables ---
extern FILE *log_file;
extern int logging_enabled;

// --- Function Prototypes ---
void log_event(const char *format, ...);
void send_sham_packet(int sockfd, const struct sockaddr_in *dest_addr, uint32_t seq, uint32_t ack, uint16_t flags, uint16_t window, const char* payload, int payload_len);
void calculate_md5(const char *filename);

#endif // SHAM_H

// llm code endns