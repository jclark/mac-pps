#ifndef CHRONY_CLIENT_H
#define CHRONY_CLIENT_H

#include <sys/time.h>

typedef struct chrony_client chrony_client_t;

/* Create a new chrony client
 * local_path_format: format string for local socket path (must contain %d for PID)
 * remote_path: path to chrony socket
 * Returns NULL on error
 */
chrony_client_t *chrony_client_create(const char *local_path_format, const char *remote_path);

/* Send a PPS sample to chrony
 * client: chrony client instance
 * tv: system time when pulse was detected
 * offset: offset between true time and system time (in seconds)
 * Returns 0 on success, -1 on error
 */
int chrony_client_send_pps(chrony_client_t *client, const struct timeval *tv, double offset);

/* Get the remote socket path */
const char *chrony_client_remote_path(chrony_client_t *client);

/* Get the local socket path */
const char *chrony_client_local_path(chrony_client_t *client);

/* Destroy chrony client and cleanup sockets */
void chrony_client_destroy(chrony_client_t *client);

#endif /* CHRONY_CLIENT_H */