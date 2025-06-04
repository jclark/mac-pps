#include "chrony_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>

#define DEFAULT_LOCAL_PATH_FORMAT "/tmp/pps-chrony%d.sock"
#define SOCK_MAGIC 0x534f434b

struct sock_sample {
    struct timeval tv;
    double offset;
    int pulse;
    int leap;
    int _pad;
    int magic;
};

struct chrony_client {
    int sock_fd;
    char local_path[256];
    char remote_path[256];
};

chrony_client_t *chrony_client_create(const char *local_path_format, const char *remote_path) {
    if (remote_path == NULL) {
        return NULL;
    }
    
    chrony_client_t *client = malloc(sizeof(chrony_client_t));
    if (client == NULL) {
        return NULL;
    }
    
    client->sock_fd = -1;
    client->local_path[0] = '\0';
    strncpy(client->remote_path, remote_path, sizeof(client->remote_path) - 1);
    client->remote_path[sizeof(client->remote_path) - 1] = '\0';
    
    if (local_path_format == NULL) {
        local_path_format = DEFAULT_LOCAL_PATH_FORMAT;
    }
    
    pid_t pid = getpid();
    snprintf(client->local_path, sizeof(client->local_path), local_path_format, pid);
    
    /* Remove any existing socket */
    unlink(client->local_path);
    
    /* Create socket */
    client->sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (client->sock_fd < 0) {
        perror("socket");
        free(client);
        return NULL;
    }
    
    /* Bind to local path */
    struct sockaddr_un local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;
    strncpy(local_addr.sun_path, client->local_path, sizeof(local_addr.sun_path) - 1);
    
    if (bind(client->sock_fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(client->sock_fd);
        free(client);
        return NULL;
    }
    
    /* Set permissions */
    if (chmod(client->local_path, 0660) < 0) {
        perror("chmod");
        close(client->sock_fd);
        unlink(client->local_path);
        free(client);
        return NULL;
    }
    
    return client;
}

int chrony_client_send_pps(chrony_client_t *client, const struct timeval *tv, double offset) {
    if (client == NULL || client->sock_fd < 0 || tv == NULL) {
        return -1;
    }
    
    struct sock_sample sample;
    memset(&sample, 0, sizeof(sample));
    sample.tv = *tv;
    sample.offset = offset;
    sample.pulse = 1;  /* This is a PPS signal */
    sample.leap = 0;   /* No leap second info */
    sample.magic = SOCK_MAGIC;
    
    struct sockaddr_un remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sun_family = AF_UNIX;
    strncpy(remote_addr.sun_path, client->remote_path, sizeof(remote_addr.sun_path) - 1);
    
    ssize_t sent = sendto(client->sock_fd, &sample, sizeof(sample), 0,
                         (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    
    return 0;
}

const char *chrony_client_remote_path(chrony_client_t *client) {
    return client ? client->remote_path : NULL;
}

const char *chrony_client_local_path(chrony_client_t *client) {
    return client ? client->local_path : NULL;
}

void chrony_client_destroy(chrony_client_t *client) {
    if (client == NULL) {
        return;
    }
    
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
    }
    
    if (strlen(client->local_path) > 0) {
        unlink(client->local_path);
    }
    
    free(client);
}