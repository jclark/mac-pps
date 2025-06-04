# Audio PPS Daemon Development Plan

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

### 1.2 Audio Device Discovery
- **Action:** Write a utility function to enumerate and list available audio input devices.
- **Details:**
    - Use `AudioObjectGetPropertyData` with `kAudioHardwarePropertyDevices` to get all `AudioDeviceID`s.
    - Iterate through the IDs, querying properties like `kAudioDevicePropertyDeviceName` and `kAudioDevicePropertyDeviceUID`.
    - Print a list of devices so you can copy the `UID` of your specific USB audio interface for later use.

### 1.3 Audio Capture Implementation
- **Action:** Set up `AudioQueueServices` to capture audio from the target device.
- **Details:**
    - Define an `AudioStreamBasicDescription` for the desired input format (e.g., 48kHz, 32-bit float, mono).
    - Create a new `AudioQueue` with `AudioQueueNewInput`.
    - Allocate several buffers with `AudioQueueAllocateBuffer` and enqueue them with `AudioQueueEnqueueBuffer`.
    - Implement the `AudioQueueInputCallback` function. Initially, this can just log that it has been called.
    - Start the queue with `AudioQueueStart`.

### 1.4 Pulse Detection Logic
- **Action:** Implement a simple pulse detection algorithm inside the callback.
- **Details:**
    - Within the callback, iterate through the sample data in the `inBuffer`.
    - Implement a basic threshold detector: find the first sample whose absolute value exceeds a predefined level (e.g., `0.5`).
    - Once detected, note the sample's index within the buffer. For now, we'll ignore this offset for simplicity.

### 1.5 High-Precision Time Conversion
- **Action:** Integrate the robust time conversion logic. This is the key milestone for Phase 1.
- **Details:**
    - Create the `TimebaseInfo` struct and the `setup_timebase_info()` function to be called from `main()`.
    - Implement the `convert_past_host_time_to_timeval()` function, which uses the "sandwiched" `gettimeofday()` between two `mach_absolute_time()` calls.
    - When your pulse detection logic fires, take the `inStartTime->mHostTime` from the callback's `AudioTimeStamp`.
    - Call your conversion function to turn this monotonic `mHostTime` into a `struct timeval`.
    - Print the resulting `tv_sec` and `tv_usec` to the console.

**Success at the end of this phase means you can run `./audiopps` and see an accurate UNIX timestamp printed once per second.**

---

## Phase 2: Chrony Integration & Communication

**Objective:** Take the timestamp from Phase 1, calculate the clock offset, and send it to `chrony` over its UNIX socket.

### 2.1 UNIX Socket Implementation
- **Action:** Add the C code to manage the UNIX domain datagram socket.
- **Details:**
    - Create a socket of type `AF_UNIX` and `SOCK_DGRAM`.
    - Set up a `struct sockaddr_un` with the path to chrony's socket (e.g., `/var/run/chrony/chrony.sock`).

### 2.2 Offset Calculation & Data Formatting
- **Action:** Calculate the clock offset and populate the `chrony` data structure.
- **Details:**
    - Define the `struct refclock_sock_sample` in your code.
    - After getting the pulse's `timeval`, calculate the offset. This is the microsecond part of the timestamp (`pulse_time.tv_usec`). Convert this to a `double` representing seconds (e.g., `2500` usec becomes `0.0025`).
    - Populate the fields of your `refclock_sock_sample` struct with the `timeval` and the calculated `offset`.

### 2.3 Data Transmission
- **Action:** Send the data packet to the `chrony` daemon.
- **Details:**
    - In the audio callback, instead of `printf`, use `sendto()` to send the populated `refclock_sock_sample` struct to the `chrony` socket.
    - Configure `chrony.conf` to accept the `SOCK` driver data and restart `chronyd`.
    - Use `chronyc sources -v` to verify that `chrony` is receiving and using the samples.

**Success at the end of this phase means `chrony` recognizes your daemon as a reference clock.**

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