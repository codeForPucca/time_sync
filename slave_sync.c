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

#include <fcntl.h>
#include <stdarg.h>

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


#define LOG_MSG_MAX_LEN 256
char last_log_msg[LOG_MSG_MAX_LEN] = "";
FILE *log_file = NULL;

// ---- Global time variable (atomic) ----
atomic_uint_least64_t global_time = 0;
int sockfd;
atomic_int sync_status = 0;           
atomic_uint_least64_t last_sync_time = 0;      // time since last update from master


void log_message(const char *format, ...) {
    char current_msg[LOG_MSG_MAX_LEN];

    va_list args;
    va_start(args, format);
    vsnprintf(current_msg, LOG_MSG_MAX_LEN, format, args);
    va_end(args);

    if (strcmp(current_msg, last_log_msg) != 0) {
        strncpy(last_log_msg, current_msg, LOG_MSG_MAX_LEN);
        if (log_file) {
            fprintf(log_file, "%s",  current_msg);
            fflush(log_file);
        }
    }
}

void handle_interrupt(int sig) {
    log_message("\nInterrupted. Closing socket and cleaning up...\n");
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
        log_message( "socket() error code: %d (%s)\n", errno, strerror(errno));
        perror("UDP socket creation failed");
        pthread_exit(NULL);
    }

    // Optional tuning
    int bufsize = 4096;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    int tos = 0x10;
    setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    // Set socket to non-blocking mode
    // Note: this cannot be done with setsockeopt(), which is used to configure socket-level options.
    // non-blocking behaviour must be controlled at the file descriptor level, not the socket level.

    int flags = fcntl(sockfd, F_GETFL, 0);
    
    if (flags == -1) {
        log_message("fcntl F_GETFL failed: %s\n", strerror(errno));
        perror("fcntl F_GETFL failed");
        close(sockfd);
        pthread_exit(NULL);
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_message("fcntl F_SETFL O_NONBLOCK failed: %s\n", strerror(errno));
        perror("fcntl F_SETFL O_NONBLOCK failed");
        close(sockfd);
        pthread_exit(NULL);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(SYNC_PORT);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        log_message("bind() error code: %d (%s)\n", errno, strerror(errno));
        perror("UDP bind failed");
        close(sockfd);
        pthread_exit(NULL);
    }

    log_message("Listening for time sync packets on UDP port %d...\n", SYNC_PORT);

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
                    DEBUG_PRINT("in sync again");
                }

            }
            else {
                DEBUG_PRINT("in sync");
                atomic_store(&last_sync_time, local);
                if (atomic_load(&sync_status) == 0){
                    gpioWrite(SYNCPIN, 1);
                    atomic_store(&sync_status, 1);
                }
            }
        } else {
            switch (errno) {
                case EAGAIN: // alsoe EWOULDBLOCK
                    // Emit message just if no answer for longer than 2seconds.
                    if ((atomic_load(&global_time) - remote_tick) > 100) {
                        log_message("EAGAIN or EWOULDBLOCK: Non-blocking socket has no data.\n");
                    }                  
                    break; // No data, retry
                case ECONNREFUSED:
                    log_message("ECONNREFUSED: Connection refused.\n");
                    break; //
                case EINTR:
                    log_message("EINTR: Interrupted by a signal.\n");
                    break; // Interrupted, retry
                case EINVAL:
                    log_message("EINVAL: Invalid argument passed to recv().\n");
                    break; // Invalid argument, check socket state
                case ENOTCONN:
                    log_message("ENOTCONN: Socket is not connected.\n");
                    break; // Socket not connected, check bind
                case EBADF:
                    log_message("EBADF: Bad file descriptor.\n");
                    break; // Invalid socket descriptor
                case EFAULT:
                    log_message("EFAULT: Bad address in recv().\n");
                    break; // Invalid memory address
                case ENOMEM:
                    log_message("ENOMEM: Out of memory.\n");
                    break; // Memory allocation failure
                case ENOTSOCK:
                    log_message("ENOTSOCK: File descriptor is not a socket.\n");
                    break; // Invalid socket descriptor
                default:
                    log_message("recv() error code: %d (%s)\n", errno, strerror(errno));
                    perror("Invalid sync packet");
            }         
        }
    }

    close(sockfd);
    return NULL;
}


// ---- Main program with microtick timer ----
int main() {

     uint64_t new_time = 0;
     uint64_t last_time = 0;

    log_file = fopen("sync_log.txt", "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }


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
        log_message("Failed to create UDP sync thread: %s\n", strerror(errno));
        perror("Failed to create UDP sync thread");
        exit(EXIT_FAILURE);
    }

    // Create timerfd
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1) {
        log_message("timerfd_create() error code: %d (%s)\n", errno, strerror(errno));
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    // Configure 1ms periodic timer
    struct itimerspec timer = {
        .it_interval = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS },
        .it_value    = { .tv_sec = 0, .tv_nsec = MICROTICK_INTERVAL_NS }
    };

    if (timerfd_settime(fd, 0, &timer, NULL) == -1) {
        log_message("timerfd_settime() error code: %d (%s)\n", errno, strerror(errno));
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    uint64_t expirations;
    unsigned int microtick_counter = 0;

    log_message("Microtick timer started (1ms interval)...\n");

    while (1) {
        // Block until the timer expires (1ms tick or multiple if delayed)
        if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
            log_message("read() error code: %d (%s)\n", errno, strerror(errno));
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
                log_message("Global time incremented: %lu\n", new_time);

                last_time = atomic_load(&last_sync_time);
                //atomic_load(&sync_status) == 1 && 
                if (new_time - last_time > 100) {  // 1macrotick is 20ms * 100 = 2 sec
                    log_message("Desync: >2s no new time from master\n");
                    DEBUG_PRINT("Desync: >2s no new time from master\n");
                    gpioWrite(SYNCPIN, 0);
                    atomic_store(&sync_status, 0);
                }

                if (new_time % 10 == 0) {
                    gpio_pulse_ms(SYNCPIN, PIN_HIGH_LONG);
                } 
                else {
                    gpio_pulse_ms(SYNCPIN, PIN_HIGH_SHORT);
                }
            }
        }
    }

    close(fd);
    if (log_file) fclose(log_file);
    return 0;
}
