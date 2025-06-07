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
#include <pigpio.h>
#include <signal.h>

#ifdef ENABLE_DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) do {} while (0)
#endif


// ---- Global Configuration ----
#define SYNC_PORT 5000
#define SYNC_BUF_SIZE 8 // one uint64_t
#define MICROTICK_INTERVAL_NS 1000000 // 1 ms
#define MICROTICKS_PER_UNIT 20        // 20 ms = 1 logical time unit
#define SYNCPIN 24
#define TICKPIN 23
#define PIN_HIGH_LONG 2
#define PIN_HIGH_SHORT 1


// ---- Global time variable (atomic) ----
atomic_uint_least64_t global_time = 0;
int sockfd;
atomic_int sync_status = 0;           
atomic_uint_least64_t last_sync_time = 0;      // time since last update from master

void handle_interrupt(int sig) {
    DEBUG_PRINT("\nInterrupted. Cleaning up...\n");
    if (sockfd != -1) {
        close(sockfd);
    } 
    gpioWrite(SYNCPIN, 0);
    gpioWrite(TICKPIN, 0);
    gpioTerminate();
    exit(1);
}

void gpio_pulse_ms(unsigned gpio, unsigned duration_ms) {
    gpioWrite(gpio, 1);
    // gpioDelay-> Delay for a number of microseconds -> we want milliseconds
    gpioDelay(duration_ms * 1000); 
    gpioWrite(gpio, 0);
}

// ---- UDP synchronization thread ----
void *udp_sync_thread(void *arg) {
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

    DEBUG_PRINT("Listening for time sync packets on UDP port %d...\n", SYNC_PORT);

    while (1) {
        ssize_t n = recv(sockfd, &remote_tick, sizeof(remote_tick), 0);
        if (n == sizeof(remote_tick)) {
            uint64_t local = atomic_load(&global_time);
            if (remote_tick > local + 1 || remote_tick + 1 < local) {
                DEBUG_PRINT("Time correction: local=%lu, received=%lu → updating\n", local, remote_tick);
                atomic_store(&global_time, remote_tick);
                atomic_store(&last_sync_time, remote_tick);
                 if (atomic_load(&sync_status) == 0){
                gpioWrite(SYNCPIN, 1);
                atomic_store(&sync_status, 1);
                DEBUG_PRINT("in sync again");}

            }
            else {
                DEBUG_PRINT("in sync");
                atomic_store(&last_sync_time, local);
                if (atomic_load(&sync_status) == 0){
                gpioWrite(SYNCPIN, 1);
                atomic_store(&sync_status, 1);}
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

     uint64_t new_time = 0;
     uint64_t last_time = 0;

    //Initialze GPIO
    if (gpioInitialise() < 0) {
    fprintf(stderr, "pigpio init failed\n");
    exit(EXIT_FAILURE);
    }
    gpioSetMode(TICKPIN, PI_OUTPUT);
    gpioWrite(TICKPIN, 0);
    gpioSetMode(SYNCPIN, PI_OUTPUT);
    gpioWrite(SYNCPIN, 0);

    // remove GPIO handler
    gpioSetSignalFunc(SIGINT, NULL);
    gpioSetSignalFunc(SIGTERM, NULL);

    // Register own handler
    signal(SIGINT, handle_interrupt);
    signal(SIGTERM, handle_interrupt);

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

    DEBUG_PRINT("Microtick timer started (1ms interval)...\n");

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
                new_time=atomic_fetch_add(&global_time, 1)+1;

                DEBUG_PRINT("Global time incremented: %lu\n", new_time);

                last_time = atomic_load(&last_sync_time);
                //atomic_load(&sync_status) == 1 && 
                if (new_time - last_time > 100) {  // 1macrotick is 20ms * 100 = 2 sec
                    DEBUG_PRINT("Desync: >2s no new time from master 0\n");
                    gpioWrite(SYNCPIN, 0);
                    atomic_store(&sync_status, 0);
                }

                if (new_time % 10 == 0) {
                 gpio_pulse_ms(SYNCPIN, PIN_HIGH_LONG);
                 } else {
                gpio_pulse_ms(SYNCPIN, PIN_HIGH_SHORT);
                }
            }
        }
    }

    close(fd);
    return 0;
}
