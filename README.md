# NixMon (Unix/Linux Monitor)

Real-time Linux system monitor over TCP sockets. A lightweight server reads kernel metrics from `/proc` and streams them to connected clients, which render a live terminal dashboard.

Built as a minor project for Network Programming,
Bachelors in Software Engineering 8th Semester<br>
Nepal College of Information Technology <br> 
Pokhara University.

## What It Does

The server runs on any Linux machine (no root needed) and collects CPU usage, memory stats, load averages, and uptime by reading `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, and `/proc/uptime`. It pushes JSON snapshots every second over TCP to all connected clients. The client parses the stream and draws a live-updating TUI dashboard with color-coded bar gauges.

## Architecture

```
┌────────────┐    TCP/JSON     ┌────────────┐
│  /proc     │───────────────▶│  TUI       │
│  Server    │    (push)       │  Client    │
│  (Linux)   │◀───────────────│  (any OS)  │
└────────────┘    connect      └────────────┘
```

- **Server**: Single-threaded event loop using `poll()` for I/O multiplexing across multiple clients
- **Client**: ncurses-based terminal dashboard with live-updating bar gauges and auto-reconnect
- **Protocol**: Newline-delimited JSON — debuggable with `nc` or `telnet`

## Building

### Prerequisites

- GCC (C11)
- CMake 3.10+
- ncurses development headers

```bash
# Fedora / RHEL
sudo dnf install gcc cmake ncurses-devel

# Debian / Ubuntu
sudo apt install gcc cmake libncurses-dev
```

### Compile

```bash
mkdir build && cd build
cmake ..
make
```

This produces two binaries inside `build/`: `nixmon-server` and `nixmon-client`.

---

## How to Run

### 1. Start the server

Open a terminal and run:

```bash
cd build
./nixmon-server
```

Expected output:
```
NixMon server listening on port 8080  (Ctrl-C to stop)
Pushing: cpu=3.2% mem=40.0% load=1.23
Pushing: cpu=2.9% mem=40.0% load=1.21
...
```

The server will keep running and push metrics every second. Press `Ctrl-C` to stop it.

### 2. Start the client

Open a **second terminal** and run:

```bash
cd build
./nixmon-client
```

You will see a live dashboard like this:

```
╔══════════  NixMon — Real-Time System Monitor  ══════════╗
║ CPU                                                      ║
║   avg     [########....................]   23.7%         ║
║   core0   [######......................]   18.4%         ║
║   core1   [#########...................]   31.2%         ║
║                                                          ║
║ MEMORY                                                   ║
║   6554/16384 MB [############............]   40.0%       ║
║                                                          ║
║ LOAD AVERAGE                                             ║
║   1m: 1.23    5m: 0.98    15m: 0.76                     ║
║                                                          ║
║ UPTIME                                                   ║
║   05h 34m 21s                                            ║
║                                                          ║
║   q — quit   |   connected to 127.0.0.1:8080            ║
╚══════════════════════════════════════════════════════════╝
```

- Bars turn **yellow** above 50% and **red** above 80%
- Press `q` to quit the client
- If the server stops, the client shows a reconnecting screen and retries every 2 seconds automatically

### 3. Debug with netcat (optional)

To see the raw JSON stream without the TUI:

```bash
nc localhost 8080
```

Output:
```json
{"cpu_pct":23.7,"cpu_cores":[31.2,18.4],"mem_total_kb":16384000,"mem_avail_kb":9830400,"mem_pct":40.0,"load_1m":1.23,"load_5m":0.98,"load_15m":0.76,"uptime_sec":482736}
```

---

## Protocol

Each frame is a single JSON object terminated by a newline (`\n`). The server pushes one frame per second to every connected client — no client polling required.

```json
{"cpu_pct":23.7,"cpu_cores":[31.2,18.4,22.1,19.8],"mem_total_kb":16384000,"mem_avail_kb":9830400,"mem_pct":40.0,"load_1m":1.23,"load_5m":0.98,"load_15m":0.76,"uptime_sec":482736}
```

## Project Structure

```
nixmon/
├── src/
│   ├── server/main.c    # TCP server, /proc collector, JSON serializer
│   └── client/main.c    # TCP client, JSON parser, ncurses dashboard
├── include/
│   └── protocol.h       # SystemMetrics struct, shared constants
├── docs/                
├── tests/               
├── CMakeLists.txt
├── .gitignore
├── LICENSE
└── README.md
```

## Metrics Collected

| Metric | Source | Method |
|--------|--------|--------|
| CPU % (overall + per-core) | `/proc/stat` | Two-snapshot delta of tick counts |
| RAM usage | `/proc/meminfo` | MemTotal, MemAvailable |
| Load averages (1/5/15 min) | `/proc/loadavg` | Direct read |
| System uptime | `/proc/uptime` | Direct read |

## Limitations

- Server only runs on Linux (depends on `/proc` filesystem)
- Port is hardcoded to `8080` — change `PORT` in `include/protocol.h` to use a different port
- No authentication or encryption (intended for trusted networks / localhost)
- Monitoring only — no process management or service control

## Author

**Abhishek Man Basnet** <br>
Bachelors in Software Engineering 8th Semester<br>
Nepal College of Information Technology <br>
Supervisor: Er. Madan Kadariya

## License

MIT
