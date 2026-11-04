
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

static volatile sig_atomic_t exit_requested = 0;

void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

/* Append a complete packet to the data file */
int append_to_file(const char *data, size_t len)
{
    FILE *file = fopen(DATA_FILE, "a");

    if (file == NULL) {
        perror("fopen");
        return -1;
    }

    if (fwrite(data, 1, len, file) != len) {
        perror("fwrite");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        perror("fclose");
        return -1;
    }

    return 0;
}

/* Send all bytes, handling partial sends */
int send_all(int client_fd, const char *data, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t bytes_sent = send(
            client_fd,
            data + total_sent,
            len - total_sent,
            MSG_NOSIGNAL
        );

        if (bytes_sent < 0) {
            if (errno == EINTR) {
                if (exit_requested) {
                    return -1;
                }
                continue;
            }

            perror("send");
            return -1;
        }

        if (bytes_sent == 0) {
            return -1;
        }

        total_sent += (size_t)bytes_sent;
    }

    return 0;
}

/* Send the entire data file to the client */
int send_file(int client_fd)
{
    FILE *file = fopen(DATA_FILE, "r");

    if (file == NULL) {
        perror("fopen");
        return -1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    int result = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send_all(client_fd, buffer, bytes_read) == -1) {
            result = -1;
            break;
        }
    }

    if (ferror(file)) {
        perror("fread");
        result = -1;
    }

    if (fclose(file) != 0) {
        perror("fclose");
        result = -1;
    }

    return result;
}

/* Append received bytes to a dynamically growing packet buffer */
int append_packet_data(
    char **packet_buffer,
    size_t *packet_len,
    size_t *packet_capacity,
    const char *data,
    size_t len)
{
    if (len > SIZE_MAX - *packet_len) {
        return -1;
    }

    size_t required = *packet_len + len;

    if (required > *packet_capacity) {
        size_t new_capacity = (*packet_capacity == 0)
                                  ? BUFFER_SIZE
                                  : *packet_capacity;

        while (new_capacity < required) {
            if (new_capacity > SIZE_MAX / 2) {
                new_capacity = required;
                break;
            }

            new_capacity *= 2;
        }

        char *new_buffer = realloc(*packet_buffer, new_capacity);

        if (new_buffer == NULL) {
            perror("realloc");
            return -1;
        }

        *packet_buffer = new_buffer;
        *packet_capacity = new_capacity;
    }

    memcpy(*packet_buffer + *packet_len, data, len);
    *packet_len = required;

    return 0;
}

/* Receive and process packets from one client */
int handle_client(int client_fd)
{
    char buffer[BUFFER_SIZE];

    char *packet_buffer = NULL;
    size_t packet_len = 0;
    size_t packet_capacity = 0;

    int result = 0;

    while (!exit_requested) {
        ssize_t bytes_received = recv(
            client_fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytes_received < 0) {
            if (errno == EINTR) {
                if (exit_requested) {
                    break;
                }
                continue;
            }

            perror("recv");
            result = -1;
            break;
        }

        if (bytes_received == 0) {
            break;
        }

        for (ssize_t i = 0; i < bytes_received; i++) {

            if (append_packet_data(
                    &packet_buffer,
                    &packet_len,
                    &packet_capacity,
                    &buffer[i],
                    1) == -1) {
                result = -1;
                break;
            }

            if (buffer[i] == '\n') {

                if (append_to_file(packet_buffer, packet_len) == -1) {
                    result = -1;
                    break;
                }

                if (send_file(client_fd) == -1) {
                    result = -1;
                    break;
                }

                /* Start a new packet */
                packet_len = 0;
            }
        }

        if (result == -1) {
            break;
        }
    }

    free(packet_buffer);

    return result;
}

/* Redirect standard file descriptors for daemon mode */
int daemonize_process(int server_fd)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid > 0) {
        /* Parent process */
        close(server_fd);
        exit(0);
    }

    /* Child process */
    if (setsid() == -1) {
        perror("setsid");
        return -1;
    }

    umask(0);

    if (chdir("/") == -1) {
        perror("chdir");
        return -1;
    }

    int fd = open("/dev/null", O_RDWR);

    if (fd == -1) {
        perror("open /dev/null");
        return -1;
    }

    if (dup2(fd, STDIN_FILENO) == -1 ||
        dup2(fd, STDOUT_FILENO) == -1 ||
        dup2(fd, STDERR_FILENO) == -1) {
        perror("dup2");
        close(fd);
        return -1;
    }

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;

    /* Check command-line arguments */
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    } else if (argc > 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int server_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len;

    /* Step 1: Create TCP socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* Allow reuse of local address */
    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Step 2: Configure server address */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Step 3: Bind socket */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Step 4: Daemon mode, after successful bind */
    if (daemon_mode) {
        if (daemonize_process(server_fd) == -1) {
            close(server_fd);
            return EXIT_FAILURE;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    /* Step 5: Configure signal handlers */
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        close(server_fd);
        closelog();
        return EXIT_FAILURE;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        close(server_fd);
        closelog();
        return EXIT_FAILURE;
    }

    /* Step 6: Listen for incoming connections */
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        close(server_fd);
        closelog();
        return EXIT_FAILURE;
    }

    /* Step 7: Accept client connections */
    while (!exit_requested) {

        client_len = sizeof(client_addr);

        client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd == -1) {
            if (errno == EINTR && exit_requested) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        /* Get client IP address */
        char client_ip[INET_ADDRSTRLEN] = "unknown";

        if (inet_ntop(AF_INET,
                      &client_addr.sin_addr,
                      client_ip,
                      sizeof(client_ip)) == NULL) {
            perror("inet_ntop");
        }

        syslog(LOG_INFO,
               "Accepted connection from %s",
               client_ip);

        /* Handle client */
        handle_client(client_fd);

        syslog(LOG_INFO,
               "Closed connection from %s",
               client_ip);

        close(client_fd);
    }

    /* Step 8: Graceful shutdown */
    syslog(LOG_INFO, "Caught signal, exiting");

    close(server_fd);

    remove(DATA_FILE);

    closelog();

    return EXIT_SUCCESS;
}
