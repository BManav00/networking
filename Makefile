# llm generated code begins

# Compiler and flags
CC=gcc
CFLAGS=-Wall -g
LDFLAGS=-lcrypto

# Use this for Linux
all: client server

# --- For macOS, uncomment the lines below and comment out the Linux section ---
# CFLAGS_MAC=-Wall -g -I$(shell brew --prefix openssl)/include
# LDFLAGS_MAC=-L$(shell brew --prefix openssl)/lib -lcrypto
# all: client_mac server_mac
# client_mac: client.c
# 	$(CC) $(CFLAGS_MAC) client.c -o client $(LDFLAGS_MAC)
# server_mac: server.c
# 	$(CC) $(CFLAGS_MAC) server.c -o server $(LDFLAGS_MAC)
# -----------------------------------------------------------------------------

client: client.c sham.h
	$(CC) $(CFLAGS) client.c -o client $(LDFLAGS)

server: server.c sham.h
	$(CC) $(CFLAGS) server.c -o server $(LDFLAGS)

clean:
	rm -f client server *.o client_log.txt server_log.txt received_file

# llm generated code ends