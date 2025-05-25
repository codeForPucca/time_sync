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

// ---- Configuration Constants ----
#define SYNC_PORT 5000
#define MICROTICK_INTERVAL_NS 1000000  // 1 ms
#define MICROTICKS_PER_UNIT 20         // 20 microticks = 1 macrotick
#define BROADCAST_INTERVAL 50          // send every 50 macroticks (i.e., every 1000 ms)

atomic_uint_least64_t global_time = 0; // atomic macrotick counter

// ---- UDP Broadcast Thread ----
void *udp_broadcast_thread(void *arg) {
    int sockfd;
    struct sockaddr_in broadcast_addr;
    uint64_t last_sent_time = 0;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("UDP socket creation failed");
        pthread_exit(NULL);
    }

    // Enable broadcast
    int broadcast_enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        perror("setsockopt SO_BROADCAST failed");
        close(sockfd);
        pthread_exit(NULL);
    }

    // Optional tuning
    int bufsize = 4096;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    int tos = 0x10;
    setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(SYNC_PORT);
    broadcast_addr.sin_addr.s_addr = inet_addr("192.168.1.255"); // broadcast address

    printf("Broadcast thread started. Sending every %d macroticks...\n", BROADCAST_INTERVAL);

    while (1) {
        uint64_t current_time = atomic_load(&global_time);
        if (current_time >= last_sent_time + BROADCAST_INTERVAL) {
            ssize_t sent = sendto(sockfd, &current_time, sizeof(current_time), 0,
                                  (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
            if (sent == sizeof(current_time)) {
                printf("Broadcasted macrotick: %lu\n", current_time);
                last_sent_time = current_time;
            } else {
                perror("Failed to send broadcast");
            }
        }
        usleep(1000); // small delay to prevent CPU spinning
    }

    close(sockfd);
    return NULL;
}

// ---- Microtick Timer Thread ----
void *microtick_timer_thread(void *arg) {
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1) {
        perror("timerfd_create");
        pthread_exit(NULL);
    }

    struct itimerspec timer = {
        .it_interval = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS },
        .it_value    = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS }
    };

    if (timerfd_settime(fd, 0, &timer, NULL) == -1) {
        perror("timerfd_settime");
        close(fd);
        pthread_exit(NULL);
    }

    uint64_t expirations;
    unsigned int microtick_counter = 0;

    printf("Microtick timer started (1 ms per tick)...\n");

    while (1) {
        if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
            perror("read from timerfd failed");
            break;
        }

        for (uint64_t i = 0; i < expirations; ++i) {
            microtick_counter++;
            if (microtick_counter >= MICROTICKS_PER_UNIT) {
                microtick_counter = 0;
                atomic_fetch_add(&global_time, 1);
                printf("Macrotick incremented: %lu\n", atomic_load(&global_time));
            }
        }
    }

    close(fd);
    return NULL;
}

// ---- Main Entry Point ----
int main() {
    pthread_t timer_thread, broadcast_thread;

    if (pthread_create(&timer_thread, NULL, microtick_timer_thread, NULL) != 0) {
        perror("Failed to create timer thread");
        return EXIT_FAILURE;
    }

    if (pthread_create(&broadcast_thread, NULL, udp_broadcast_thread, NULL) != 0) {
        perror("Failed to create broadcast thread");
        return EXIT_FAILURE;
    }

    // Join threads (wait forever)
    pthread_join(timer_thread, NULL);
    pthread_join(broadcast_thread, NULL);

    return 0;
}
