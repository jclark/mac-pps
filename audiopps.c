#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <math.h>

static CFRunLoopRef runLoop = NULL;
static AudioQueueRef audioQueue = NULL;
static volatile sig_atomic_t keepRunning = 1;
static int debugMode = 0;
static float pulseThreshold = 0.5f;

void list_input_sources(AudioDeviceID deviceID);

typedef struct {
    mach_timebase_info_data_t timebase;
    double ticks_per_second;
} TimebaseInfo;

static TimebaseInfo timebaseInfo;

void setup_timebase_info(void) {
    mach_timebase_info(&timebaseInfo.timebase);
    timebaseInfo.ticks_per_second = 1e9 * (double)timebaseInfo.timebase.denom / 
                                    (double)timebaseInfo.timebase.numer;
}

void convert_past_host_time_to_timeval(uint64_t hostTime, struct timeval *result) {
    struct timeval tv_before, tv_after;
    uint64_t mt_before, mt_after;
    
    // "Sandwich" gettimeofday() between two mach_absolute_time() calls
    mt_before = mach_absolute_time();
    gettimeofday(&tv_before, NULL);
    mt_after = mach_absolute_time();
    
    // Calculate the midpoint time in mach time
    uint64_t mt_midpoint = (mt_before + mt_after) / 2;
    
    // Calculate how long ago the hostTime was relative to our midpoint
    uint64_t mt_diff = mt_midpoint - hostTime;
    double seconds_ago = (double)mt_diff * timebaseInfo.timebase.numer / 
                         (timebaseInfo.timebase.denom * 1e9);
    
    // Convert tv_before to double seconds
    double tv_before_seconds = tv_before.tv_sec + tv_before.tv_usec / 1e6;
    
    // Subtract the time difference to get the target time
    double target_time = tv_before_seconds - seconds_ago;
    
    // Convert back to struct timeval
    result->tv_sec = (time_t)target_time;
    result->tv_usec = (suseconds_t)((target_time - result->tv_sec) * 1e6);
}

void signal_handler(int sig) {
    keepRunning = 0;
    if (runLoop) {
        CFRunLoopStop(runLoop);
    }
}

void audio_input_callback(void *inUserData,
                         AudioQueueRef inAQ,
                         AudioQueueBufferRef inBuffer,
                         const AudioTimeStamp *inStartTime,
                         UInt32 inNumberPacketDescriptions,
                         const AudioStreamPacketDescription *inPacketDescs) {
    static int pulse_detected = 0;
    static uint64_t last_pulse_time = 0;
    static int callback_count = 0;
    
    float *samples = (float *)inBuffer->mAudioData;
    UInt32 numSamples = inBuffer->mAudioDataByteSize / sizeof(float);
    
    callback_count++;
    
    float max_level = 0.0f;
    float min_level = 0.0f;
    
    for (UInt32 i = 0; i < numSamples; i++) {
        float sample = samples[i];
        if (sample > max_level) max_level = sample;
        if (sample < min_level) min_level = sample;
        
        if (fabsf(sample) > pulseThreshold) {
            // Calculate precise time including sample offset
            uint64_t buffer_start_time = inStartTime->mHostTime;
            
            // Calculate time offset for this specific sample within the buffer
            double sample_rate = 48000.0; // Should match the format we set
            double sample_offset_seconds = (double)i / sample_rate;
            uint64_t sample_offset_ticks = (uint64_t)(sample_offset_seconds * 
                                          timebaseInfo.timebase.denom * 1e9 / 
                                          timebaseInfo.timebase.numer);
            
            uint64_t precise_pulse_time = buffer_start_time + sample_offset_ticks;
            
            uint64_t time_since_last = precise_pulse_time - last_pulse_time;
            double seconds_since_last = (double)time_since_last * 
                                       timebaseInfo.timebase.numer / 
                                       (timebaseInfo.timebase.denom * 1e9);
            
            if (seconds_since_last > 0.5) {
                struct timeval pulse_time;
                convert_past_host_time_to_timeval(precise_pulse_time, &pulse_time);
                
                printf("PPS detected at %ld.%06d (level: %.3f, sample: %u/%u)\n", 
                       pulse_time.tv_sec, pulse_time.tv_usec, sample, i, numSamples);
                
                last_pulse_time = precise_pulse_time;
                pulse_detected = 1;
                break;
            }
        }
    }
    
    if (debugMode && (callback_count % 20 == 0)) {
        printf("Audio levels: min=%.3f, max=%.3f, samples=%u, threshold=%.3f\n", 
               min_level, max_level, numSamples, pulseThreshold);
    }
    
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

void list_audio_devices(void) {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    
    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                    &propertyAddress,
                                                    0, NULL,
                                                    &dataSize);
    if (status != noErr) {
        fprintf(stderr, "Error getting device list size\n");
        return;
    }
    
    UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID *devices = malloc(dataSize);
    
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                       &propertyAddress,
                                       0, NULL,
                                       &dataSize,
                                       devices);
    if (status != noErr) {
        fprintf(stderr, "Error getting device list\n");
        free(devices);
        return;
    }
    
    printf("Available Audio Input Devices:\n");
    printf("------------------------------\n");
    
    for (UInt32 i = 0; i < deviceCount; i++) {
        propertyAddress.mSelector = kAudioDevicePropertyStreams;
        propertyAddress.mScope = kAudioDevicePropertyScopeInput;
        
        status = AudioObjectGetPropertyDataSize(devices[i], &propertyAddress, 0, NULL, &dataSize);
        if (status != noErr || dataSize == 0) {
            continue;
        }
        
        CFStringRef deviceName = NULL;
        dataSize = sizeof(deviceName);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        
        status = AudioObjectGetPropertyData(devices[i],
                                           &propertyAddress,
                                           0, NULL,
                                           &dataSize,
                                           &deviceName);
        
        if (status == noErr && deviceName != NULL) {
            char name[256];
            Boolean result = CFStringGetCString(deviceName,
                                               name,
                                               sizeof(name),
                                               kCFStringEncodingUTF8);
            if (result) {
                printf("Device: %s\n", name);
            }
            CFRelease(deviceName);
        }
        
        CFStringRef deviceUID = NULL;
        dataSize = sizeof(deviceUID);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceUID;
        
        status = AudioObjectGetPropertyData(devices[i],
                                           &propertyAddress,
                                           0, NULL,
                                           &dataSize,
                                           &deviceUID);
        
        if (status == noErr && deviceUID != NULL) {
            char uid[256];
            Boolean result = CFStringGetCString(deviceUID,
                                               uid,
                                               sizeof(uid),
                                               kCFStringEncodingUTF8);
            if (result) {
                printf("  UID: %s\n", uid);
            }
            CFRelease(deviceUID);
        }
        
        list_input_sources(devices[i]);
        printf("\n");
    }
    
    free(devices);
}

AudioDeviceID find_device_by_uid(const char *uid) {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                  &propertyAddress,
                                  0, NULL,
                                  &dataSize);
    
    UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID *devices = malloc(dataSize);
    AudioDeviceID foundDevice = 0;
    
    AudioObjectGetPropertyData(kAudioObjectSystemObject,
                              &propertyAddress,
                              0, NULL,
                              &dataSize,
                              devices);
    
    for (UInt32 i = 0; i < deviceCount; i++) {
        CFStringRef deviceUID = NULL;
        dataSize = sizeof(deviceUID);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceUID;
        
        OSStatus status = AudioObjectGetPropertyData(devices[i],
                                                    &propertyAddress,
                                                    0, NULL,
                                                    &dataSize,
                                                    &deviceUID);
        
        if (status == noErr && deviceUID != NULL) {
            char currentUID[256];
            Boolean result = CFStringGetCString(deviceUID,
                                               currentUID,
                                               sizeof(currentUID),
                                               kCFStringEncodingUTF8);
            if (result && strcmp(currentUID, uid) == 0) {
                foundDevice = devices[i];
                CFRelease(deviceUID);
                break;
            }
            CFRelease(deviceUID);
        }
    }
    
    free(devices);
    return foundDevice;
}

void list_input_sources(AudioDeviceID deviceID) {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDataSources,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    
    Boolean hasProperty = AudioObjectHasProperty(deviceID, &propertyAddress);
    if (!hasProperty) {
        printf("  Device does not support input source selection\n");
        return;
    }
    
    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, NULL, &dataSize);
    if (status != noErr || dataSize == 0) {
        return;
    }
    
    UInt32 numDataSources = dataSize / sizeof(UInt32);
    UInt32 *dataSources = malloc(dataSize);
    
    status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, dataSources);
    if (status != noErr) {
        free(dataSources);
        return;
    }
    
    printf("  Available input sources:\n");
    
    for (UInt32 i = 0; i < numDataSources; i++) {
        CFStringRef dataSourceName = NULL;
        AudioValueTranslation translation = {
            &dataSources[i],
            sizeof(UInt32),
            &dataSourceName,
            sizeof(CFStringRef)
        };
        
        propertyAddress.mSelector = kAudioDevicePropertyDataSourceNameForIDCFString;
        dataSize = sizeof(translation);
        
        status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &translation);
        if (status == noErr && dataSourceName != NULL) {
            char name[256];
            Boolean success = CFStringGetCString(dataSourceName, name, sizeof(name), kCFStringEncodingUTF8);
            if (success) {
                printf("    - %s (ID: 0x%08X)\n", name, dataSources[i]);
            }
            CFRelease(dataSourceName);
        }
    }
    
    free(dataSources);
}

UInt32 find_data_source_by_name(AudioDeviceID deviceID, const char *targetName) {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDataSources,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    
    UInt32 foundDataSource = 0;
    UInt32 dataSize = 0;
    
    OSStatus status = AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, NULL, &dataSize);
    if (status != noErr || dataSize == 0) {
        return 0;
    }
    
    UInt32 numDataSources = dataSize / sizeof(UInt32);
    UInt32 *dataSources = malloc(dataSize);
    
    status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, dataSources);
    if (status != noErr) {
        free(dataSources);
        return 0;
    }
    
    for (UInt32 i = 0; i < numDataSources; i++) {
        CFStringRef dataSourceName = NULL;
        AudioValueTranslation translation = {
            &dataSources[i],
            sizeof(UInt32),
            &dataSourceName,
            sizeof(CFStringRef)
        };
        
        propertyAddress.mSelector = kAudioDevicePropertyDataSourceNameForIDCFString;
        dataSize = sizeof(translation);
        
        status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &translation);
        if (status == noErr && dataSourceName != NULL) {
            char name[256];
            Boolean success = CFStringGetCString(dataSourceName, name, sizeof(name), kCFStringEncodingUTF8);
            if (success && strcmp(name, targetName) == 0) {
                foundDataSource = dataSources[i];
                CFRelease(dataSourceName);
                break;
            }
            CFRelease(dataSourceName);
        }
    }
    
    free(dataSources);
    return foundDataSource;
}

OSStatus set_input_source(AudioDeviceID deviceID, UInt32 dataSourceID) {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDataSource,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    
    UInt32 size = sizeof(dataSourceID);
    
    OSStatus status = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, NULL, size, &dataSourceID);
    return status;
}

void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [options] [device-UID [input-source]]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --list-devices    List all audio input devices and their sources\n");
    fprintf(stderr, "  --help            Show this help message\n");
    fprintf(stderr, "  --debug           Show audio levels and detection info\n");
    fprintf(stderr, "  --threshold N     Set pulse detection threshold (default: 0.5)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s\n", progname);
    fprintf(stderr, "  %s --debug --threshold 0.1\n", progname);
    fprintf(stderr, "  %s \"AppleUSBAudioEngine:...:2\"\n", progname);
    fprintf(stderr, "  %s \"AppleUSBAudioEngine:...:2\" \"External Line Connector\"\n", progname);
}

int main(int argc, char *argv[]) {
    const char *deviceUID = NULL;
    const char *inputSourceName = NULL;
    
    int argIndex = 1;
    while (argIndex < argc) {
        if (strcmp(argv[argIndex], "--list-devices") == 0) {
            list_audio_devices();
            return 0;
        } else if (strcmp(argv[argIndex], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[argIndex], "--debug") == 0) {
            debugMode = 1;
            argIndex++;
        } else if (strcmp(argv[argIndex], "--threshold") == 0) {
            if (argIndex + 1 < argc) {
                pulseThreshold = atof(argv[argIndex + 1]);
                argIndex += 2;
            } else {
                fprintf(stderr, "Error: --threshold requires a value\n");
                usage(argv[0]);
                return 1;
            }
        } else if (argv[argIndex][0] != '-') {
            if (deviceUID == NULL) {
                deviceUID = argv[argIndex];
            } else if (inputSourceName == NULL) {
                inputSourceName = argv[argIndex];
            }
            argIndex++;
        } else {
            fprintf(stderr, "Error: Unknown option %s\n", argv[argIndex]);
            usage(argv[0]);
            return 1;
        }
    }
    
    setup_timebase_info();
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    format.mBytesPerPacket = format.mBytesPerFrame = 4;
    
    AudioDeviceID selectedDevice = 0;
    
    if (deviceUID) {
        selectedDevice = find_device_by_uid(deviceUID);
        if (selectedDevice == 0) {
            fprintf(stderr, "Device with UID '%s' not found\n", deviceUID);
            return 1;
        }
        
        if (inputSourceName) {
            UInt32 dataSourceID = find_data_source_by_name(selectedDevice, inputSourceName);
            if (dataSourceID != 0) {
                OSStatus sourceStatus = set_input_source(selectedDevice, dataSourceID);
                if (sourceStatus == noErr) {
                    printf("Selected input source: %s\n", inputSourceName);
                } else {
                    fprintf(stderr, "Error setting input source '%s': %d\n", inputSourceName, (int)sourceStatus);
                }
            } else {
                fprintf(stderr, "Input source '%s' not found on device\n", inputSourceName);
                fprintf(stderr, "Use --list-devices to see available input sources\n");
                return 1;
            }
        }
    }
    
    OSStatus status = AudioQueueNewInput(&format,
                                        audio_input_callback,
                                        NULL,
                                        CFRunLoopGetCurrent(),
                                        kCFRunLoopCommonModes,
                                        0,
                                        &audioQueue);
    
    if (status != noErr) {
        fprintf(stderr, "Error creating audio queue: %d\n", (int)status);
        return 1;
    }
    
    if (deviceUID) {
        CFStringRef uidRef = CFStringCreateWithCString(kCFAllocatorDefault,
                                                      deviceUID,
                                                      kCFStringEncodingUTF8);
        if (uidRef) {
            CFStringRef uidToSet = uidRef;
            status = AudioQueueSetProperty(audioQueue,
                                         kAudioQueueProperty_CurrentDevice,
                                         &uidToSet,
                                         sizeof(uidToSet));
            CFRelease(uidRef);
            
            if (status != noErr) {
                fprintf(stderr, "Error setting audio device: %d\n", (int)status);
                AudioQueueDispose(audioQueue, true);
                return 1;
            }
            printf("Successfully set audio device\n");
        }
    }
    
    const int kNumberBuffers = 3;
    const int kBufferSize = 4096;
    
    for (int i = 0; i < kNumberBuffers; i++) {
        AudioQueueBufferRef buffer;
        status = AudioQueueAllocateBuffer(audioQueue, kBufferSize, &buffer);
        if (status == noErr) {
            status = AudioQueueEnqueueBuffer(audioQueue, buffer, 0, NULL);
        }
    }
    
    status = AudioQueueStart(audioQueue, NULL);
    if (status != noErr) {
        fprintf(stderr, "Error starting audio queue: %d\n", (int)status);
        AudioQueueDispose(audioQueue, true);
        return 1;
    }
    
    printf("Audio PPS daemon started. Press Ctrl+C to stop.\n");
    if (deviceUID) {
        printf("Using device UID: %s\n", deviceUID);
    } else {
        printf("Using default audio input device\n");
    }
    
    runLoop = CFRunLoopGetCurrent();
    while (keepRunning) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    }
    
    printf("\nShutting down...\n");
    
    AudioQueueStop(audioQueue, true);
    AudioQueueDispose(audioQueue, true);
    
    return 0;
}