#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

// ---- Global Configuration ----
#define SYNC_PORT 5000
#define SYNC_BUF_SIZE 8 // one uint64_t
#define MICROTICK_INTERVAL_NS 1000000 // 1 ms
#define MICROTICKS_PER_UNIT 20        // 20 ms = 1 logical time unit

// ---- Global time variable (atomic) ----
atomic_uint_least64_t global_time = 0;

// ---- UDP synchronization thread ----
void *udp_sync_thread(void *arg) {
    int sockfd;
    struct sockaddr_in servaddr;
    uint64_t remote_tick;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("UDP socket creation failed");
        pthread_exit(NULL);
    }

    // Optional tuning
    int bufsize = 4096;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    int tos = 0x10;
    setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(SYNC_PORT);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("UDP bind failed");
        close(sockfd);
        pthread_exit(NULL);
    }

    printf("Listening for time sync packets on UDP port %d...\n", SYNC_PORT);

    while (1) {
        ssize_t n = recv(sockfd, &remote_tick, sizeof(remote_tick), 0);
        if (n == sizeof(remote_tick)) {
            uint64_t local = atomic_load(&global_time);
            if (remote_tick > local + 1 || remote_tick + 1 < local) {
                printf("Time correction: local=%lu, received=%lu → updating\n", local, remote_tick);
                atomic_store(&global_time, remote_tick);
            }
        } else {
            perror("Invalid sync packet");
        }
    }

    close(sockfd);
    return NULL;
}

// ---- Main program with microtick timer ----
int main() {
    // Start UDP synchronization thread
    pthread_t udp_thread;
    if (pthread_create(&udp_thread, NULL, udp_sync_thread, NULL) != 0) {
        perror("Failed to create UDP sync thread");
        exit(EXIT_FAILURE);
    }

    // Create timerfd
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1) {
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    // Configure 1ms periodic timer
    struct itimerspec timer = {
        .it_interval = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS },
        .it_value    = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS }
    };

    if (timerfd_settime(fd, 0, &timer, NULL) == -1) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    uint64_t expirations;
    unsigned int microtick_counter = 0;

    printf("Microtick timer started (1ms interval)...\n");

    while (1) {
        // Block until the timer expires (1ms tick or multiple if delayed)
        if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
            perror("read");
            exit(EXIT_FAILURE);
        }

        // For each missed or elapsed tick, update local microtick state
        for (uint64_t i = 0; i < expirations; ++i) {
            microtick_counter++;
            if (microtick_counter >= MICROTICKS_PER_UNIT) {
                microtick_counter = 0;
                atomic_fetch_add(&global_time, 1);
                printf("Global time incremented: %lu\n", atomic_load(&global_time));
            }
        }
    }

    close(fd);
    return 0;
}
