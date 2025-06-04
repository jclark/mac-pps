# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

mac-pps implements two approaches for timestamping Pulse Per Second (PPS) signals on macOS for precision timing applications with GPS receivers and atomic clocks:

1. **pollpps** - Monitors USB-to-serial adapter CTS modem control lines (with chrony integration)
2. **audiopps** - Captures PPS signals converted to audio impulses via CoreAudio (chrony integration pending)

## Essential Commands

### Build
```bash
make
```

### Run
```bash
./pollpps <device>
# Example: ./pollpps /dev/tty.usbserial-0001
```

### Clean build
```bash
make clean
make
```

## Architecture

### Core Components

- **chrony_client.c/h** - Shared library for communicating with chrony via SOCK refclock
- **pollpps.c** - USB-to-serial CTS line monitoring program (chrony integration complete)
- **audiopps.c** - CoreAudio-based PPS detection program (chrony integration pending)

### pollpps Program

1. Opens USB-to-serial device and monitors CTS modem control line
2. Polls CTS status every 100 microseconds using `ioctl(TIOCMGET)`
3. Detects falling edges (CTS on→off transitions) representing PPS pulses
4. Timestamps using `clock_gettime(CLOCK_REALTIME)`
5. Calculates offset and optionally sends to chrony via shared chrony_client library

Key technical aspects:
- Polls CTS line every 100 microseconds
- Typical timing jitter: ~1ms due to USB latency
- Uses signal handlers for graceful shutdown
- Handles RS232 to TTL signal polarity conversion (CTS asserted = 0V, deasserted = 3.3V)

### audiopps Program

Currently implements Phase 1 of audio-plan.md:
1. Audio device enumeration and selection
2. CoreAudio capture setup with threshold detection
3. Sample-accurate pulse timing within audio buffers
4. Console output of timestamps

Pending: Integration with shared chrony_client library for Phase 2

## Development Notes

- The code contains detailed comments explaining signal polarity and timing
- Error handling is implemented for all device operations
- The binary targets ARM64 architecture (Apple Silicon)
- No external dependencies beyond standard C libraries