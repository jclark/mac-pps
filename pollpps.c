#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include "chrony_client.h"

#define DEFAULT_REMOTE_PATH "/var/run/chrony.pollpps.sock"

static volatile sig_atomic_t interrupted = 0;
static chrony_client_t *chrony_client = NULL;
static char remote_path[256] = DEFAULT_REMOTE_PATH;
static bool use_chrony = false;

void handle_signal(int sig) {
    interrupted = 1;
}

void print_usage(const char *prog) {
    fprintf(stderr, "usage: %s [options] <device>\n", prog);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -c, --chrony             Send samples to chrony\n");
    fprintf(stderr, "  -r, --remote-path PATH   Remote chrony socket path (default: %s)\n", DEFAULT_REMOTE_PATH);
    fprintf(stderr, "  -h, --help              Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *device = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--chrony") == 0) {
            use_chrony = true;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--remote-path") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
            strncpy(remote_path, argv[++i], sizeof(remote_path) - 1);
            remote_path[sizeof(remote_path) - 1] = '\0';
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (device == NULL) {
            device = argv[i];
        } else {
            fprintf(stderr, "Error: Too many arguments\n");
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (device == NULL) {
        fprintf(stderr, "Error: Device argument required\n");
        print_usage(argv[0]);
        return 1;
    }
    
    /* Open serial port */
    int fd = open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    /* Save original terminal settings */
    struct termios orig_tios;
    if (tcgetattr(fd, &orig_tios) < 0) {
        perror("tcgetattr");
        close(fd);
        return 1;
    }

    /* Set up raw mode */
    struct termios raw_tios = orig_tios;
    cfmakeraw(&raw_tios);
    
    if (tcsetattr(fd, TCSANOW, &raw_tios) < 0) {
        perror("tcsetattr");
        close(fd);
        return 1;
    }

    /* Set up chrony client if requested */
    if (use_chrony) {
        chrony_client = chrony_client_create(NULL, remote_path);
        if (chrony_client == NULL) {
            fprintf(stderr, "Failed to setup chrony client\n");
            tcsetattr(fd, TCSANOW, &orig_tios);
            close(fd);
            return 1;
        }
    }

    /* Set up signal handler */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("Monitoring PPS on CTS line of %s\n", device);
    if (use_chrony) {
        printf("Local socket: %s\n", chrony_client_local_path(chrony_client));
        printf("Remote socket: %s\n", chrony_client_remote_path(chrony_client));
    } else {
        printf("Chrony integration disabled\n");
    }

    bool last_cts = false;
    int pps_count = 0;
    int status;

    while (!interrupted) {
        /* Get modem status */
        if (ioctl(fd, TIOCMGET, &status) < 0) {
            perror("ioctl(TIOCMGET)");
            struct timespec sleep_time = { 0, 100000 };
            nanosleep(&sleep_time, NULL);  /* 0.1ms on error */
            continue;
        }

        /* Check if CTS flag is set */
        bool cts = (status & TIOCM_CTS) != 0;

        /* Check for transition from flag being on to off.
         * This is the opposite of what you might expect.
         * In RS232, CTS is asserted by having a negative voltage, and deasserted by a positive voltage.
         * With a USB to TTL serial adapter, an RS232 negative voltage is represented by a logic low (0V),
         * and a positive voltage is represented by a logic high (3.3V).
         * Thus in TTL, the CTS being asserted corresponds to a logic low (0V);
         * logic high (3.3V) means CTS is deasserted.
         * A PPS leading edge with normal polarity is TTL logic level going from low to high,
         * which corresponds to CTS flag going from on to off.
         */
        if (!cts && last_cts) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            
            pps_count++;
            
            /* Calculate offset: system time fractional part minus true time (0.0 at top of second) */
            double offset = ((double)ts.tv_nsec / 1000000000.0) - 0.0;
            
            /* Convert timespec to timeval for chrony */
            struct timeval tv;
            tv.tv_sec = ts.tv_sec;
            tv.tv_usec = ts.tv_nsec / 1000;
            
            /* Send sample to chrony if enabled */
            if (use_chrony && chrony_client_send_pps(chrony_client, &tv, offset) < 0) {
                fprintf(stderr, "Failed to send chrony sample\n");
            }
            
            /* Format time for debug output */
            struct tm *tm = localtime(&ts.tv_sec);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
            
            printf("PPS #%d at %s.%09ld (%ld.%09ld) offset=%.6f\n",
                   pps_count,
                   time_buf,
                   ts.tv_nsec,
                   ts.tv_sec, ts.tv_nsec,
                   offset);
        }

        last_cts = cts;
        
        /* Poll every 0.1ms (100 microseconds) using nanosleep */
        struct timespec sleep_time = {
            .tv_sec = 0,
            .tv_nsec = 100000  /* 100 microseconds = 100,000 nanoseconds */
        };
        nanosleep(&sleep_time, NULL);
    }

    printf("\nReceived interrupt, shutting down...\n");

    /* Cleanup chrony client */
    if (chrony_client) {
        chrony_client_destroy(chrony_client);
    }

    /* Restore original terminal settings */
    tcsetattr(fd, TCSANOW, &orig_tios);
    close(fd);

    return 0;
}