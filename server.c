
// llm code begins
#include "sham.h"

FILE *log_file = NULL;
int logging_enabled = 0;
float loss_rate = 0.0;

// For out-of-order packet buffering
typedef struct buffered_packet {
    int is_valid;
    uint32_t seq_num;
    int len;
    char data[PAYLOAD_SIZE];
} buffered_packet;

// For chat mode sending
typedef struct sent_packet_info {
    int is_valid;
    struct timeval sent_time;
    uint32_t seq_num;
    char data[PAYLOAD_SIZE];
    int payload_len;
} sent_packet_info;

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

void calculate_md5(const char *filename) {
    unsigned char c[MD5_DIGEST_LENGTH];
    FILE *inFile = fopen(filename, "rb");
    if (!inFile) {
        printf("MD5 Error: %s can't be opened.\n", filename);
        return;
    }
    MD5_CTX mdContext;
    MD5_Init(&mdContext);
    int bytes;
    unsigned char data[1024];
    while ((bytes = fread(data, 1, 1024, inFile)) != 0) MD5_Update(&mdContext, data, bytes);
    MD5_Final(c, &mdContext);
    printf("MD5: ");
    for(int i = 0; i < MD5_DIGEST_LENGTH; i++) printf("%02x", c[i]);
    printf("\n");
    fclose(inFile);
}

void handle_file_transfer(int sockfd, struct sockaddr_in *client_addr, uint32_t expected_seq_num) {
    socklen_t addr_len = sizeof(*client_addr);
    
    sham_packet packet;
    int n = recvfrom(sockfd, &packet, sizeof(packet), 0, (struct sockaddr*)client_addr, &addr_len);
    
    char output_filename[PAYLOAD_SIZE + 1];
    int filename_len = n - sizeof(sham_header);
    memcpy(output_filename, packet.data, filename_len);
    output_filename[filename_len] = '\0';
    uint32_t seq = ntohl(packet.header.seq_num);
    log_event("RCV FILENAME DATA SEQ=%u LEN=%d NAME=%s", seq, filename_len, output_filename);

    FILE *outfile = fopen(output_filename, "wb");
    if (!outfile) { perror("Failed to open output file"); return; }
    
    expected_seq_num = seq + filename_len;
    send_sham_packet(sockfd, client_addr, 0, expected_seq_num, ACK, RECEIVER_BUFFER_SIZE, NULL, 0);

    buffered_packet ooo_buffer[SENDER_WINDOW_SIZE*2] = {0};
    uint32_t server_fin_seq = 0; // Will be set upon receiving client's FIN

    while (1) {
        n = recvfrom(sockfd, &packet, sizeof(packet), 0, (struct sockaddr*)client_addr, &addr_len);
        if (n <= 0) continue;
        
        seq = ntohl(packet.header.seq_num);
        uint16_t flags = ntohs(packet.header.flags);
        int payload_len = n - sizeof(sham_header);

        // FIX: Only drop DATA packets
        if (payload_len > 0 && ((double)rand() / RAND_MAX) < loss_rate) {
            log_event("DROP DATA SEQ=%u", seq);
            continue;
        }

        if (flags & FIN) {
            log_event("RCV FIN SEQ=%u", seq);
            server_fin_seq = seq; // Store the client's FIN sequence number
            send_sham_packet(sockfd, client_addr, 0, seq + 1, ACK, 0, NULL, 0);
            break; // Exit the loop to complete the handshake
        }
        
        if (payload_len > 0) {
            log_event("RCV DATA SEQ=%u LEN=%d", seq, payload_len);
            
            if (seq == expected_seq_num) {
                fwrite(packet.data, 1, payload_len, outfile);
                expected_seq_num += payload_len;

                int processed = 1;
                while(processed) {
                    processed = 0;
                    for (int i = 0; i < SENDER_WINDOW_SIZE*2; ++i) {
                        if (ooo_buffer[i].is_valid && ooo_buffer[i].seq_num == expected_seq_num) {
                            fwrite(ooo_buffer[i].data, 1, ooo_buffer[i].len, outfile);
                            expected_seq_num += ooo_buffer[i].len;
                            ooo_buffer[i].is_valid = 0;
                            processed = 1;
                        }
                    }
                }
            } else if (seq > expected_seq_num) {
                int stored = 0;
                for (int i = 0; i < SENDER_WINDOW_SIZE*2; ++i) {
                    if (!ooo_buffer[i].is_valid) {
                        ooo_buffer[i] = (buffered_packet){.is_valid=1, .seq_num=seq, .len=payload_len};
                        memcpy(ooo_buffer[i].data, packet.data, payload_len);
                        stored = 1;
                        break;
                    }
                }
                if (!stored) log_event("Out-of-order buffer is full!");
            }
            send_sham_packet(sockfd, client_addr, 0, expected_seq_num, ACK, RECEIVER_BUFFER_SIZE, NULL, 0);
        }
    }
    fclose(outfile);
    calculate_md5(output_filename);

    // FIX: Complete the 4-way handshake after the loop
    uint32_t my_fin_seq = (rand() % 10000) + 50000;
    send_sham_packet(sockfd, client_addr, my_fin_seq, 0, FIN, 0, NULL, 0);
    
    sham_packet final_ack_pkt;
    recvfrom(sockfd, &final_ack_pkt, sizeof(final_ack_pkt), 0, (struct sockaddr*)client_addr, &addr_len);
    log_event("RCV ACK=%u", ntohl(final_ack_pkt.header.ack_num));
}

void handle_chat_mode(int sockfd, struct sockaddr_in *client_addr, uint32_t my_seq, uint32_t expected_peer_seq) {
    printf("Chat connection established. Type '/quit' to exit.\n");
    socklen_t addr_len = sizeof(*client_addr);
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
            int n = recvfrom(sockfd, &packet, sizeof(packet), 0, (struct sockaddr*)client_addr, &addr_len);
            if (n > 0) {
                uint32_t recv_seq = ntohl(packet.header.seq_num);
                uint16_t flags = ntohs(packet.header.flags);
                
                if (flags & FIN) {
                    log_event("RCV FIN SEQ=%u", recv_seq);
                    send_sham_packet(sockfd, client_addr, my_seq, recv_seq + 1, ACK, 0, NULL, 0);
                    send_sham_packet(sockfd, client_addr, my_seq, 0, FIN, 0, NULL, 0);
                    
                    sham_packet final_ack;
                    recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, (struct sockaddr*)client_addr, &addr_len);
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
                        send_sham_packet(sockfd, client_addr, my_seq, expected_peer_seq, ACK, RECEIVER_BUFFER_SIZE, NULL, 0);
                    }
                }
            }
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char buffer[PAYLOAD_SIZE];
            if (fgets(buffer, sizeof(buffer), stdin)) {
                if (strncmp(buffer, "/quit", 5) == 0) {
                    send_sham_packet(sockfd, client_addr, my_seq, 0, FIN, 0, NULL, 0);
                    
                    sham_packet ack_pkt, fin_pkt;
                    recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)client_addr, &addr_len);
                    log_event("RCV ACK FOR FIN");

                    recvfrom(sockfd, &fin_pkt, sizeof(fin_pkt), 0, (struct sockaddr*)client_addr, &addr_len);
                    log_event("RCV FIN SEQ=%u", ntohl(fin_pkt.header.seq_num));

                    send_sham_packet(sockfd, client_addr, my_seq + 1, ntohl(fin_pkt.header.seq_num) + 1, ACK, 0, NULL, 0);

                    printf("Chat session terminated by server.\n");
                    terminate = 1;
                } else {
                    int slot = -1;
                    for(int i=0; i<SENDER_WINDOW_SIZE; ++i) if(!send_window[i].is_valid) { slot = i; break; }

                    if (slot != -1) {
                        int len = strlen(buffer);
                        send_window[slot] = (sent_packet_info){.is_valid=1, .seq_num=my_seq, .payload_len=len};
                        memcpy(send_window[slot].data, buffer, len);
                        gettimeofday(&send_window[slot].sent_time, NULL);
                        send_sham_packet(sockfd, client_addr, my_seq, 0, 0, RECEIVER_BUFFER_SIZE, buffer, len);
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
                    send_sham_packet(sockfd, client_addr, send_window[i].seq_num, 0, 0, 0, send_window[i].data, send_window[i].payload_len);
                    gettimeofday(&send_window[i].sent_time, NULL);
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [--chat] [loss_rate]\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);
    int chat_mode = 0;
    
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--chat") == 0) chat_mode = 1;
        else loss_rate = atof(argv[i]);
    }
    
    if (getenv("RUDP_LOG")) {
        logging_enabled = 1;
        log_file = fopen("server_log.txt", "w");
    }
    srand(time(NULL));

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr, client_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed"); exit(1);
    }
    printf("Server listening on port %d...\n", port);
    
    while (1) {
        printf("Waiting for new connection...\n");
        sham_packet syn_packet;
        socklen_t addr_len = sizeof(client_addr);
        int n = recvfrom(sockfd, &syn_packet, sizeof(syn_packet), 0, (struct sockaddr*)&client_addr, &addr_len);
        
        if (n > 0 && (ntohs(syn_packet.header.flags) & SYN)) {
            uint32_t client_isn = ntohl(syn_packet.header.seq_num);
            log_event("RCV SYN SEQ=%u", client_isn);
            
            uint32_t server_isn = rand() % 50000;
            send_sham_packet(sockfd, &client_addr, server_isn, client_isn + 1, SYN | ACK, RECEIVER_BUFFER_SIZE, NULL, 0);

            sham_packet first_ack;
            recvfrom(sockfd, &first_ack, sizeof(first_ack), 0, (struct sockaddr*)&client_addr, &addr_len);
            log_event("RCV ACK FOR SYN");
            printf("Connection established.\n");
            
            uint32_t client_seq = ntohl(first_ack.header.seq_num);
            uint32_t server_ack = ntohl(first_ack.header.ack_num);

            if (chat_mode) {
                 handle_chat_mode(sockfd, &client_addr, server_ack, client_seq);
                 // ========================= FIX START =========================
                 // After a chat session, break the loop to terminate the server.
                 break;
                 // ========================== FIX END ==========================
            } else {
                 handle_file_transfer(sockfd, &client_addr, client_seq);
            }
            
            printf("Session finished.\n");
        }
    }

    close(sockfd);
    if (log_file) fclose(log_file);
    printf("Server shutting down.\n");
    return 0;
}

// llm code ends