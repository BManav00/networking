# S.H.A.M. Reliable UDP Networking Project

## Overview
This project implements a reliable file transfer and chat protocol over UDP, named **S.H.A.M.** (Simple Hybrid ARQ Mechanism). It provides reliability, ordering, and flow control on top of UDP, mimicking some features of TCP, including a sliding window, retransmissions, and connection management (SYN/ACK/FIN).

## Features
- Reliable file transfer over UDP
- Sliding window protocol for efficient data transfer
- Packet loss simulation (configurable)
- Logging of all protocol events
- MD5 checksum verification for file integrity
- Simple chat mode (optional)

## Protocol Details
- **Packet Structure:** Defined in `sham.h` with custom headers (sequence, ack, flags, window size) and payload.
- **Flags:**
  - `SYN`: Connection initiation
  - `ACK`: Acknowledgment
  - `FIN`: Connection termination
- **Sliding Window:** Sender maintains a fixed-size window (default: 10 packets).
- **Timeouts:** Retransmission timeout (RTO) is 500ms by default.
- **Buffering:** Receiver buffers out-of-order packets up to 16KB.
- **MD5:** Used for file integrity check after transfer.

## File Descriptions
- `client.c`: Implements the client-side logic for sending files or chat messages to the server. Handles connection setup, file reading, packetization, retransmissions, and logging.
- `server.c`: Implements the server-side logic for receiving files or chat messages. Handles connection setup, file writing, out-of-order buffering, MD5 verification, and logging.
- `sham.h`: Shared protocol definitions, packet structures, constants, and function prototypes.
- `Makefile`: Build instructions for Linux (and macOS, see comments). Cleans up binaries and logs.

## Build Instructions
1. **Dependencies:**
   - GCC (tested on Linux)
   - OpenSSL (for MD5)
2. **Build:**
   ```bash
   make
   ```
   This produces `client` and `server` executables.

## Usage
### Start the Server
```bash
./server <port> [--log] [--loss <rate>]
```
- `<port>`: Port to listen on
- `--log`: Enable logging to `server_log.txt`
- `--loss <rate>`: Simulate packet loss (0.0 to 1.0)

### Start the Client
```bash
./client <server_ip> <port> <input_file> <output_file> [--log] [--loss <rate>]
```
- `<server_ip>`: IP address of the server
- `<port>`: Port number
- `<input_file>`: File to send
- `<output_file>`: Name to save as on server
- `--log`: Enable logging to `client_log.txt`
- `--loss <rate>`: Simulate packet loss (0.0 to 1.0)

### Example
```bash
# On server
./server 9000 --log

# On client
./client 127.0.0.1 9000 myfile.txt received_file --log
```

## Logging
- All protocol events (SYN, ACK, FIN, DATA, retransmissions) are logged with timestamps if `--log` is enabled.
- Logs are written to `client_log.txt` and `server_log.txt`.

## Cleaning Up
To remove binaries and logs:
```bash
make clean
```

## Notes
- The protocol is for educational purposes and not intended for production use.
- For macOS, see the commented section in the `Makefile` for OpenSSL path adjustments.

---

© 2025 S.H.A.M. Networking Project
