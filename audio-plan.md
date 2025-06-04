# Audio PPS Daemon Development Plan

Current status: stage 2.2 implemented.

## 1. Background

The goal of this project is to create a high-precision timing daemon for macOS. It will capture a one-pulse-per-second (PPS) signal from a GPS receiver, which has been converted into an audio impulse and fed into a USB audio device. The daemon will precisely timestamp this pulse, calculate the system clock's offset from this "true time" reference, and feed the correction to the `chrony` daemon via its `SOCK` driver interface. This provides a robust, microsecond-accurate time synchronization source for a Mac.

---

## Phase 1: Core Timestamping Engine (Command-Line Tool)

**Objective:** Create a command-line application that correctly identifies the audio pulse and prints an accurate wall-clock timestamp to the console.

### 1.1 Project Skeleton & Run Loop
- **Action:** Create a `audiopps.c` file.
- **Details:**
    - Include necessary headers: `CoreAudio/CoreAudio.h`, `AudioToolbox/AudioToolbox.h`, `sys/time.h`, `signal.h`.
    - Implement a `main()` function that sets up a `CFRunLoop`.
    - Add a signal handler for `SIGINT` and `SIGTERM` that calls `CFRunLoopStop()` to ensure a clean shutdown. This forms the basic structure of a long-running console application.

### 1.2 Audio Device Discovery & Input Source Selection
- **Action:** Write utility functions to enumerate audio input devices and their available input sources.
- **Details:**
    - Use `AudioObjectGetPropertyData` with `kAudioHardwarePropertyDevices` to get all `AudioDeviceID`s.
    - Iterate through the IDs, querying properties like `kAudioDevicePropertyDeviceName` and `kAudioDevicePropertyDeviceUID`.
    - For each device, enumerate available input sources using `kAudioDevicePropertyDataSources` and `kAudioDevicePropertyDataSourceNameForIDCFString`.
    - Implement functions to find devices by UID and input sources by name.
    - Add `--list-devices` command line option to display all devices and their input sources.
    - Print device UIDs and input source names so users can specify exact audio routing (e.g., "External Line Connector").

### 1.3 Audio Capture Implementation
- **Action:** Set up `AudioQueueServices` to capture audio from the target device.
- **Details:**
    - Define an `AudioStreamBasicDescription` for the desired input format (48kHz, 32-bit float, mono).
    - Create a new `AudioQueue` with `AudioQueueNewInput`.
    - **Critical discovery:** AudioQueue device selection requires passing the device UID as a `CFStringRef` to `AudioQueueSetProperty` with `kAudioQueueProperty_CurrentDevice`, not an `AudioDeviceID`.
    - Set the input source on the selected device using `AudioObjectSetPropertyData` with `kAudioDevicePropertyDataSource`.
    - Allocate several buffers with `AudioQueueAllocateBuffer` and enqueue them with `AudioQueueEnqueueBuffer`.
    - Implement the `AudioQueueInputCallback` function with pulse detection logic.
    - Start the queue with `AudioQueueStart`.

### 1.4 Pulse Detection Logic & Debugging
- **Action:** Implement a configurable pulse detection algorithm with debugging capabilities.
- **Details:**
    - Within the callback, iterate through the sample data in the `inBuffer`.
    - Implement a threshold detector: find the first sample whose absolute value exceeds a configurable level (default 0.5, adjustable via `--threshold`).
    - Add `--debug` mode to show audio levels every ~1 second to help diagnose signal levels.
    - Track minimum and maximum audio levels in each buffer for debugging.
    - Implement debouncing logic to prevent multiple detections within 0.5 seconds.
    - Note the sample's index within the buffer for precise timing calculations.

### 1.5 High-Precision Time Conversion
- **Action:** Integrate the robust time conversion logic with sample-level precision. This is the key milestone for Phase 1.
- **Details:**
    - Create the `TimebaseInfo` struct and the `setup_timebase_info()` function to be called from `main()`.
    - Implement the `convert_past_host_time_to_timeval()` function, which uses the "sandwiched" `gettimeofday()` between two `mach_absolute_time()` calls and calculates the midpoint for maximum accuracy.
    - When pulse detection fires, take the `inStartTime->mHostTime` from the callback's `AudioTimeStamp`.
    - **Sample-level precision:** Calculate the exact time offset for the specific sample within the buffer using the sample rate (48kHz) and sample index.
    - Add the sample offset to the buffer start time to get the precise pulse timestamp.
    - Call your conversion function to turn this monotonic `mHostTime` into a `struct timeval`.
    - Print the resulting `tv_sec` and `tv_usec` to the console, along with signal level and sample position for debugging.

### 1.6 Command Line Interface
- **Action:** Implement a complete command line interface for production use.
- **Details:**
    - Add `--help` option with usage information and examples.
    - Add `--list-devices` to enumerate all audio devices and input sources.
    - Add `--debug` mode for troubleshooting audio levels and detection.
    - Add `--threshold N` to adjust pulse detection sensitivity.
    - Support device UID and input source name as positional arguments.
    - Provide clear error messages for invalid devices or input sources.

**Success at the end of this phase means you can run `./audiopps "device-UID" "External Line Connector"` and see accurate UNIX timestamps with ~20μs jitter printed once per second.**

---

## Phase 2: Chrony Integration & Communication

**Objective:** Take the timestamp from Phase 1, calculate the clock offset, and send it to `chrony` over its UNIX socket.

**Status:** Shared chrony client library implemented (`chrony_client.c/h`) and integrated into `pollpps`. Integration into `audiopps` is next.

### 2.1 Shared Chrony Client Library
- **Action:** Create reusable chrony communication library.
- **Details:**
    - Implements proper `sock_sample` structure matching chrony source code
    - Creates local Unix datagram socket with PID-based naming (e.g., `/tmp/pps-chrony{PID}.sock`)
    - Manages remote socket connection to chrony (configurable path)
    - Handles socket cleanup and error conditions
    - API: `chrony_client_create()`, `chrony_client_send_pps()`, `chrony_client_destroy()`

### 2.2 Integration with pollpps 
- **Action:** Add chrony support to modem status line program.
- **Details:**
    - Uses shared chrony_client library
    - Calculates correct offset: `system_time_fractional - 0.0` (positive when system ahead)
    - Optional chrony integration via `--chrony` flag
    - Configurable remote socket path via `--remote-path`
    - Tested and working with chrony SOCK refclock driver

### 2.3 Integration with audiopps
- **Action:** Add chrony support to audio-based PPS program.
- **Details:**
    - Link with shared chrony_client library (Makefile already configured)
    - Add command line options (`--chrony`, `--remote-path`)
    - Convert audio timestamps to chrony format
    - Calculate offset from sample-accurate timing
    - Test integration with chrony

**Success at the end of this phase means both `pollpps` and `audiopps` can send timing data to chrony as reference clocks.**

---

## Phase 3: Daemonization & System Integration

**Objective:** Convert the working command-line tool into a proper system service that starts on boot.

### 3.1 Transition to `syslog`
- **Action:** Remove all console output and implement system logging.
- **Details:**
    - Replace all `printf()` and `fprintf(stderr, ...)` statements with calls to `syslog()`.
    - Use `openlog()` at the start of `main()` to set your daemon's identity and `closelog()` before exiting.

### 3.2 Create `launchd` Service File
- **Action:** Write a `.plist` file to define your daemon for macOS's service manager.
- **Details:**
    - Create `com.yourcompany.audiopps.plist` with key-value pairs.
    - Essential keys:
        - `Label`: A unique reverse-DNS name for the service.
        - `ProgramArguments`: An array with the full path to your compiled executable.
        - `RunAtLoad`: `<true/>` to start it on boot.
        - `KeepAlive`: `<true/>` to have `launchd` automatically restart it if it crashes.

### 3.3 Deployment
- **Action:** Install the executable and service file to their standard system locations.
- **Details:**
    - Copy the final compiled binary to `/usr/local/sbin/`.
    - Copy the `.plist` file to `/Library/LaunchDaemons/`.
    - Load and start the service with `sudo launchctl load /Library/LaunchDaemons/com.yourcompany.audiopps.plist`.

**Success at the end of this phase means your daemon runs automatically in the background after a system reboot.**