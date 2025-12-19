///////// llm generated code begins

#include "sham.h"

FILE *log_file = NULL;
int logging_enabled = 0;
float loss_rate = 0.0;

// Struct to track packets in the sliding window
typedef struct sent_packet_info {
    int is_valid;
    struct timeval sent_time;
    uint32_t seq_num;
    char data[PAYLOAD_SIZE];
    int payload_len;
} sent_packet_info;

// Utility functions
void log_event(const char *format, ...) {
    if (!logging_enabled) return;
    char time_buffer[30];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    strftime(time_buffer, 30, "%Y-%m-%d %H:%M:%S", localtime(&tv.tv_sec));
    fprintf(log_file, "[%s.%06ld] [LOG] ", time_buffer, tv.tv_usec);
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
}

void send_sham_packet(int sockfd, const struct sockaddr_in *dest_addr, uint32_t seq, uint32_t ack, uint16_t flags, uint16_t window, const char* payload, int payload_len) {
    sham_packet packet = {0};
    packet.header.seq_num = htonl(seq);
    packet.header.ack_num = htonl(ack);
    packet.header.flags = htons(flags);
    packet.header.window_size = htons(window);
    if (payload) memcpy(packet.data, payload, payload_len);
    
    sendto(sockfd, &packet, sizeof(sham_header) + payload_len, 0, (struct sockaddr*)dest_addr, sizeof(*dest_addr));

    if (flags & SYN && flags & ACK) log_event("SND SYN-ACK SEQ=%u ACK=%u", seq, ack);
    else if (flags & SYN) log_event("SND SYN SEQ=%u", seq);
    else if (flags & FIN) log_event("SND FIN SEQ=%u", seq);
    else if (flags & ACK && payload_len == 0) log_event("SND ACK=%u WIN=%u", ack, window);
    else if (payload_len > 0) log_event("SND DATA SEQ=%u LEN=%d", seq, payload_len);
}

// FIX: This function is now self-contained and manages the entire file transfer.
void send_file(int sockfd, struct sockaddr_in *server_addr, const char *input_file, const char *output_file, uint32_t base_seq) {
    FILE *infile = fopen(input_file, "rb");
    if (!infile) {
        perror("Failed to open input file");
        return;
    }

    // 1. Send filename and wait for ACK
    send_sham_packet(sockfd, server_addr, base_seq, 0, 0, RECEIVER_BUFFER_SIZE, output_file, strlen(output_file));
    log_event("SND FILENAME DATA SEQ=%u", base_seq);

    sham_packet file_ack;
    recvfrom(sockfd, &file_ack, sizeof(file_ack), 0, NULL, NULL);
    base_seq = ntohl(file_ack.header.ack_num);
    log_event("RCV ACK for filename SEQ=%u", base_seq);

    // 2. Start the sliding window transfer
    sent_packet_info window_buffer[SENDER_WINDOW_SIZE] = {0};
    uint32_t base = base_seq;
    uint32_t next_seq_num = base_seq;
    uint16_t receiver_window = RECEIVER_BUFFER_SIZE;
    int file_done = 0;

    // BUFFER FILLING
    while (!file_done || base < next_seq_num) {
        while (!file_done && (next_seq_num - base < receiver_window)) {
            int slot = -1;
            for(int i=0; i<SENDER_WINDOW_SIZE; ++i) if(!window_buffer[i].is_valid) { slot=i; break; }
            if (slot == -1) break;

            char buffer[PAYLOAD_SIZE];
            int bytes_read = fread(buffer, 1, PAYLOAD_SIZE, infile);
            if (bytes_read <= 0) {
                file_done = 1;
                break;
            }

            window_buffer[slot] = (sent_packet_info){ .is_valid = 1, .seq_num = next_seq_num, .payload_len = bytes_read };
            memcpy(window_buffer[slot].data, buffer, bytes_read);
            gettimeofday(&window_buffer[slot].sent_time, NULL);
            
            send_sham_packet(sockfd, server_addr, next_seq_num, 0, 0, 0, buffer, bytes_read);
            next_seq_num += bytes_read;
        }
        // Check for incoming ACKs
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        struct timeval timeout = {0, 100000};
        
        if (select(sockfd + 1, &readfds, NULL, NULL, &timeout) > 0) {
            sham_packet ack_packet;
            recvfrom(sockfd, &ack_packet, sizeof(ack_packet), 0, NULL, NULL);
            
            // ========================= FIX START =========================
            // Simulate the loss of an ACK packet from the server
            if ((ntohs(ack_packet.header.flags) & ACK) && ((double)rand() / RAND_MAX) < loss_rate) {
                log_event("DROP ACK=%u", ntohl(ack_packet.header.ack_num));
                continue; // Skip processing this ACK to simulate loss
            }
            // ========================== FIX END ==========================

            uint32_t ack_num = ntohl(ack_packet.header.ack_num);
            uint16_t flags = ntohs(ack_packet.header.flags);

            if (flags & ACK) {
                log_event("RCV ACK=%u", ack_num);
                if (ack_num > base) base = ack_num;
                receiver_window = ntohs(ack_packet.header.window_size);
                if (receiver_window > 0) log_event("FLOW WIN UPDATE=%u", receiver_window);

                for (int i = 0; i < SENDER_WINDOW_SIZE; ++i) {
                    if (window_buffer[i].is_valid && (window_buffer[i].seq_num + window_buffer[i].payload_len <= base)) {
                        window_buffer[i].is_valid = 0;
                    }
                }
            }
        }
        
        //Retransmission on timeout
        struct timeval now;
        gettimeofday(&now, NULL);
        for (int i = 0; i < SENDER_WINDOW_SIZE; ++i) {
            if (window_buffer[i].is_valid) {
                long elapsed_ms = (now.tv_sec - window_buffer[i].sent_time.tv_sec) * 1000 + (now.tv_usec - window_buffer[i].sent_time.tv_usec) / 1000;
                if (elapsed_ms >= RTO_MS) {
                    log_event("TIMEOUT SEQ=%u", window_buffer[i].seq_num);
                    log_event("RETX DATA SEQ=%u LEN=%d", window_buffer[i].seq_num, window_buffer[i].payload_len);
                    send_sham_packet(sockfd, server_addr, window_buffer[i].seq_num, 0, 0, 0, window_buffer[i].data, window_buffer[i].payload_len);
                    gettimeofday(&window_buffer[i].sent_time, NULL);
                }
            }
        }
    }
    fclose(infile);

    // 4-Way Handshake
    send_sham_packet(sockfd, server_addr, next_seq_num, 0, FIN, 0, NULL, 0);
    sham_packet ack_pkt, fin_pkt;
    recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, NULL, NULL);
    log_event("RCV ACK FOR FIN");
    recvfrom(sockfd, &fin_pkt, sizeof(fin_pkt), 0, NULL, NULL);
    log_event("RCV FIN SEQ=%u", ntohl(fin_pkt.header.seq_num));
    send_sham_packet(sockfd, server_addr, next_seq_num + 1, ntohl(fin_pkt.header.seq_num) + 1, ACK, 0, NULL, 0);
    printf("File sent successfully.\n");
}

void handle_chat_mode(int sockfd, struct sockaddr_in *server_addr, uint32_t my_seq, uint32_t expected_peer_seq) {
    printf("Chat connection established. Type '/quit' to exit.\n");
    sent_packet_info send_window[SENDER_WINDOW_SIZE] = {0};
    int terminate = 0;

    while(!terminate) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        struct timeval timeout = {0, 100000};
        int activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (activity < 0) perror("select error");

        if (FD_ISSET(sockfd, &readfds)) {
            sham_packet packet;
            int n = recvfrom(sockfd, &packet, sizeof(packet), 0, NULL, NULL);
            if (n > 0) {
                uint32_t recv_seq = ntohl(packet.header.seq_num);
                uint16_t flags = ntohs(packet.header.flags);
                
                if (flags & FIN) {
                    log_event("RCV FIN SEQ=%u", recv_seq);
                    send_sham_packet(sockfd, server_addr, my_seq, recv_seq + 1, ACK, 0, NULL, 0);
                    send_sham_packet(sockfd, server_addr, my_seq, 0, FIN, 0, NULL, 0);
                    
                    sham_packet final_ack;
                    recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, NULL, NULL);
                    log_event("RCV ACK FOR FIN");
                    printf("Peer has left the chat.\n");
                    terminate = 1;
                }
                
                if (flags & ACK) {
                    uint32_t ack_num = ntohl(packet.header.ack_num);
                    log_event("RCV ACK=%u", ack_num);
                    for (int i = 0; i < SENDER_WINDOW_SIZE; ++i) {
                         if (send_window[i].is_valid && (send_window[i].seq_num + send_window[i].payload_len <= ack_num)) {
                            send_window[i].is_valid = 0;
                         }
                    }
                }
                
                int payload_len = n - sizeof(sham_header);
                if (payload_len > 0) {
                    if (((double)rand() / RAND_MAX) < loss_rate) {
                        log_event("DROP DATA SEQ=%u", recv_seq);
                    } else {
                        log_event("RCV DATA SEQ=%u LEN=%d", recv_seq, payload_len);
                        if (recv_seq == expected_peer_seq) {
                            printf("Peer: %.*s", payload_len, packet.data);
                            fflush(stdout);
                            expected_peer_seq += payload_len;
                        }
                        send_sham_packet(sockfd, server_addr, my_seq, expected_peer_seq, ACK, RECEIVER_BUFFER_SIZE, NULL, 0);
                    }
                }
            }
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char buffer[PAYLOAD_SIZE];
            if (fgets(buffer, sizeof(buffer), stdin)) {
                if (strncmp(buffer, "/quit", 5) == 0) {
                    send_sham_packet(sockfd, server_addr, my_seq, 0, FIN, 0, NULL, 0);
                    
                    sham_packet ack_pkt, fin_pkt;
                    recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, NULL, NULL);
                    log_event("RCV ACK FOR FIN");
                    
                    recvfrom(sockfd, &fin_pkt, sizeof(fin_pkt), 0, NULL, NULL);
                    log_event("RCV FIN SEQ=%u", ntohl(fin_pkt.header.seq_num));
                    
                    send_sham_packet(sockfd, server_addr, my_seq + 1, ntohl(fin_pkt.header.seq_num) + 1, ACK, 0, NULL, 0);
                    
                    printf("Chat session terminated.\n");
                    terminate = 1;
                } else {
                    int slot = -1;
                    for(int i=0; i<SENDER_WINDOW_SIZE; ++i) if(!send_window[i].is_valid) { slot = i; break; }

                    if (slot != -1) {
                        int len = strlen(buffer);
                        send_window[slot] = (sent_packet_info){.is_valid = 1, .seq_num = my_seq, .payload_len = len};
                        memcpy(send_window[slot].data, buffer, len);
                        gettimeofday(&send_window[slot].sent_time, NULL);
                        send_sham_packet(sockfd, server_addr, my_seq, 0, 0, RECEIVER_BUFFER_SIZE, buffer, len);
                        my_seq += len;
                    } else {
                         printf("Send window is full. Please wait.\n");
                    }
                }
            }
        }

        struct timeval now;
        gettimeofday(&now, NULL);
        for (int i = 0; i < SENDER_WINDOW_SIZE; ++i) {
            if (send_window[i].is_valid) {
                long elapsed_ms = (now.tv_sec - send_window[i].sent_time.tv_sec) * 1000 + (now.tv_usec - send_window[i].sent_time.tv_usec) / 1000;
                if (elapsed_ms >= RTO_MS) {
                    log_event("TIMEOUT SEQ=%u", send_window[i].seq_num);
                    send_sham_packet(sockfd, server_addr, send_window[i].seq_num, 0, 0, 0, send_window[i].data, send_window[i].payload_len);
                    gettimeofday(&send_window[i].sent_time, NULL);
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\nFile: %s <ip> <port> <infile> <outfile> [loss_rate]\nChat: %s <ip> <port> --chat [loss_rate]\n", argv[0], argv[0]);
        exit(1);
    }
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    int chat_mode = (strcmp(argv[3], "--chat") == 0);

    if (chat_mode) {
        if (argc > 4) loss_rate = atof(argv[4]);
    } else if (!chat_mode && argc > 5) {
        loss_rate = atof(argv[5]);
    }
    
    if (getenv("RUDP_LOG")) {
        logging_enabled = 1;
        log_file = fopen("client_log.txt", "w");
    }
    srand(time(NULL));

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    uint32_t client_isn = (rand() % 10000) + 1;
    
    send_sham_packet(sockfd, &server_addr, client_isn, 0, SYN, RECEIVER_BUFFER_SIZE, NULL, 0);
    
    sham_packet syn_ack_packet;
    recvfrom(sockfd, &syn_ack_packet, sizeof(syn_ack_packet), 0, NULL, NULL);
    uint32_t s_seq = ntohl(syn_ack_packet.header.seq_num);
    uint32_t s_ack = ntohl(syn_ack_packet.header.ack_num);
    uint16_t s_flags = ntohs(syn_ack_packet.header.flags);

    if ((s_flags & SYN) && (s_flags & ACK) && (s_ack == client_isn + 1)) {
        log_event("RCV SYN-ACK SEQ=%u ACK=%u", s_seq, s_ack);
        
        uint32_t final_ack_seq = client_isn + 1;
        uint32_t final_ack_num = s_seq + 1;
        send_sham_packet(sockfd, &server_addr, final_ack_seq, final_ack_num, ACK, RECEIVER_BUFFER_SIZE, NULL, 0);
        printf("Connection established.\n");

        if (chat_mode) {
            handle_chat_mode(sockfd, &server_addr, final_ack_seq, final_ack_num);
        } else if (argc >= 5) {
            const char *input_file = argv[3];
            const char *output_file = argv[4];
            // FIX: The main function now delegates the entire file transfer process,
            // including sending the filename, to the send_file function.
            send_file(sockfd, &server_addr, input_file, output_file, final_ack_seq);
        } else { 
            fprintf(stderr, "Invalid arguments for file transfer mode.\n"); 
        }
    } else {
        fprintf(stderr, "Handshake failed.\n");
    }

    close(sockfd);
    if (log_file) fclose(log_file);
    return 0;
}

///llm generated code end