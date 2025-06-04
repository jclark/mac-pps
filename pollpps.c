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

static volatile sig_atomic_t interrupted = 0;

void handle_signal(int sig) {
    interrupted = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <device>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    
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

    /* Set up signal handler */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("Monitoring PPS on CTS line of %s\n", device);

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
            
            /* Format time similar to Go output */
            struct tm *tm = localtime(&ts.tv_sec);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
            
            printf("PPS #%d at %s.%09ld (%ld.%09ld)\n",
                   pps_count,
                   time_buf,
                   ts.tv_nsec,
                   ts.tv_sec, ts.tv_nsec);
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

    /* Restore original terminal settings */
    tcsetattr(fd, TCSANOW, &orig_tios);
    close(fd);

    return 0;
}