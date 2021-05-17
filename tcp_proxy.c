/*
################################################################################
#
#  Copyright 2020-2021 Inango Systems Ltd.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
################################################################################
*/
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/queue.h>
#include <sys/wait.h>
#include <sys/un.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// logging
#define DEBUG(...)  writeLog(DEBUG, __VA_ARGS__)
#define INFO(...)   writeLog(INFO, __VA_ARGS__)
#define ERROR(...)  writeLog(ERROR, __VA_ARGS__)
#define ERROR_ERRNO(message) ERROR(message " (errno %d: %s)", errno, strerror(errno))

// common
#define ARRSZ(p) (sizeof(p) / sizeof(p[0]))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

// default configuration
#define MAX_CONNECTIONS (FD_SETSIZE-1)
#define SOCKET_OPTION_ENABLE 1

// for getopt
#define SIP_PARAM   300
#define DIP_PARAM   301
#define SPORT_PARAM 302
#define DPORT_PARAM 303

enum verbosity
{
    LOWEST_VERBOSITY = 0,
    SILENT = LOWEST_VERBOSITY,
    ERROR,
    WARNING,
    INFO,
    DEBUG,
    HIGHEST_VERBOSITY = DEBUG,
};

struct Options
{
    int srcPort;
    int dstPort;
    struct in_addr srcIP;
    struct in_addr dstIP;
    int bindToInterface;
    char networkInterface[IFNAMSIZ];

    size_t exchangeBufferSize;
    enum verbosity currentVerbosityLevel;
};

static struct Options globalOptions = {0};
static int globalProxyServerSocket = 0;

static int parseOptions(int argc, char *argv[]);
static void sighandler(int signum);
static int configSignals(void);
static void usage(const char* programName);
static void setDefaults();
static void cleanup();

static void writeLog(enum verbosity level, const char* message, ...);

static int socketWrite(int fd, char* buffer, int bufferSize);
static int exchange(int from, int to, char* exchangeBuffer);
static void fillFDSet2(fd_set* set, int fd1, int fd2);
static void exchangeLoop(int clientfd, int serverfd);
static void handleConnection(int clientSocket, struct sockaddr_in remoteServerAddr);

static void writeLog(enum verbosity level, const char* message, ...)
{
    if ((level < LOWEST_VERBOSITY) || (level > HIGHEST_VERBOSITY)
        || (level == SILENT) || (level > globalOptions.currentVerbosityLevel))
    {
        return;
    }

    char str[300];
    va_list args;

    va_start(args, message);

    vsnprintf(str, sizeof(str), message, args);
    time_t lt = time(NULL);
    struct tm* ptr = localtime(&lt);

    printf("%02d/%02d/%04d %02d:%02d:%02d [PID %d] %s\n", ptr->tm_mday, ptr->tm_mon+1, ptr->tm_year+1900,
                                                          ptr->tm_hour, ptr->tm_min, ptr->tm_sec,
                                                          getpid(), str);

    va_end(args);
}

static int socketWrite(int fd, char* buffer, int bufferSize)
{
    int nleft, nwritten;

    nleft = bufferSize;
    while (nleft > 0)
    {
        nwritten = send(fd, buffer, nleft, MSG_NOSIGNAL);
        if (nwritten <= 0) {
            ERROR("socketWrite error, fd = %d, size = %d, errno = %s, nleft = %d, write returned %d", fd, bufferSize, strerror(errno), nleft, nwritten);
            return nwritten;
        }
        nleft -= nwritten;
        buffer += nwritten;
    }
    return (bufferSize - nleft);
}

static void usage(const char* programName)
{
    printf("Usage: %s --dst-ip <IP address> --src-port <INTEGER> [OTHER OPTIONS]\n\n", programName);
    printf("\t--src-ip <IP address>, proxy server ip address (default: 0.0.0.0)\n"
           "\t--dst-ip <IP address>, remote server ip address (mandatory argument)\n"
           "\t--src-port <INTEGER>, proxy server port (mandatory argument)\n"
           "\t--dst-port <INTEGER>, destination port (default: value of --src-port)\n"
           "\t-i, --interface <STRING>, network interface to bind to\n"
           "\t-b, --buffer-size <INTEGER>, size of buffer used to move data between client and server, in bytes (default: 16KiB)\n"
           "\t-v, --verbose, increase verbosity (default level: INFO, one -v to get to highest verbosity (DEBUG))\n"
           "\t-h, --help, show this help message\n");
}

#define CHECK_MANDATORY_OPTION(option, optionName) \
    if (haveOption.option == 0) { \
        ERROR("missing mandatory option: %s", optionName); \
        return -1; \
    }

static int parseOptions(int argc, char *argv[])
{
    int c;

    // 1 if option was found on command line, 0 otherwise
    struct mandatoryOptions {
        int dstIP;
        int srcPort;
    } haveOption = {0};

    while (1) {
        int option_index = 0;

        static struct option proxy_long_options[] = {
            {"src-ip",      1, NULL, SIP_PARAM},
            {"dst-ip",      1, NULL, DIP_PARAM},
            {"src-port",    1, NULL, SPORT_PARAM},
            {"dst-port",    1, NULL, DPORT_PARAM},
            {"interface",   1, NULL, 'i'},
            {"buffer-size", 1, NULL, 'b'},
            {"verbose",     0, NULL, 'v'},
            {"help",        0, NULL, 'h'},
            {0,             0, NULL, 0}
        };

        c = getopt_long(argc, argv, "i:b:vh",
                        proxy_long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {

        case 'h':
            usage(argv[0]);
            return -1;

        case SIP_PARAM:
            if (optarg)
            {
                inet_aton(optarg, &globalOptions.srcIP);
            }
            break;

        case DIP_PARAM:
            if (optarg)
            {
                inet_aton(optarg, &globalOptions.dstIP);
                haveOption.dstIP = 1;
            }
            break;

        case SPORT_PARAM:
            if (optarg)
            {
                globalOptions.srcPort = atoi(optarg);
                haveOption.srcPort = 1;
            }
            break;

        case DPORT_PARAM:
            if (optarg)
            {
                globalOptions.dstPort = atoi(optarg);
            }
            break;

        case 'i':
            if (optarg)
            {
                strncpy(globalOptions.networkInterface, optarg, IFNAMSIZ);
                globalOptions.bindToInterface = 1;
            }
            break;

        case 'b':
        {
            char** endptr = NULL;
            errno = 0;
            size_t newSize = strtoul(optarg, endptr, 10);
            if ((newSize == 0) || (endptr != NULL) || (isdigit(optarg[0]) == 0) || (errno != 0))
            {
                ERROR("invalid size of exchange buffer");
                return -1;
            }
            globalOptions.exchangeBufferSize = newSize;
            break;
        }

        case 'v':
            if (globalOptions.currentVerbosityLevel < HIGHEST_VERBOSITY)
            {
                globalOptions.currentVerbosityLevel += 1;
            }
            break;

        default:
            break;
        }
    }

    // check if all mandatory options are set OR EXIT WITH -1
    CHECK_MANDATORY_OPTION(dstIP, "--dst-ip");
    CHECK_MANDATORY_OPTION(srcPort, "--src-port");

    if (globalOptions.dstPort == 0)
    {
        INFO("--dst-port not supplied, using --src-port for its value");
        globalOptions.dstPort = globalOptions.srcPort;
    }

    return 0;
}

// executes when we receive signals (see `configSignals`)
static void sighandler(int signum)
{
    INFO("exiting due to signal %d", signum);
    cleanup();
    exit(0);
}

static int configSignals()
{
    struct sigaction act;
    int sigs[] = { SIGINT, SIGTERM };

    memset(&act, 0, sizeof(act));
    act.sa_handler = sighandler;
    act.sa_flags = SA_RESTART;

    size_t n;
    for (n = 0; n < ARRSZ(sigs); n++)
    {
        if (sigaction(sigs[n], &act, NULL) < 0)
        {
            ERROR_ERRNO("sigaction");
            exit(-1);
        }
    }
    return 0;
}

static int exchange(int from, int to, char* exchangeBuffer)
{
    ssize_t readCount, writeCount;

    readCount = read(from, exchangeBuffer, globalOptions.exchangeBufferSize);
    if (readCount < 0)
    {
        ERROR("error reading from fd %d: %s (%d)", from, strerror(errno), errno);
        return -1;
    }
    else if (readCount == 0)
    {
        INFO("client closed the connection (fd %d)", from);
        return -1;
    }

    writeCount = socketWrite(to, exchangeBuffer, readCount);
    if (writeCount != readCount)
    {
        DEBUG("tried to write %ld bytes to fd %d, but wrote %ld", readCount, to, writeCount);
        return -1;
    }

    return 0;
}

static void fillFDSet2(fd_set* set, int fd1, int fd2)
{
    FD_ZERO(set);
    FD_SET(fd1, set);
    FD_SET(fd2, set);
}

static void exchangeLoop(int clientfd, int serverfd)
{
    fd_set readfds;
    fd_set exceptfds;
    char* exchangeBuffer = malloc(globalOptions.exchangeBufferSize);
    DEBUG("fds: client=%d; server=%d", clientfd, serverfd);
    const int maxSocket = MAX(clientfd, serverfd);
    while (1)
    {
        fillFDSet2(&readfds, clientfd, serverfd);
        fillFDSet2(&exceptfds, clientfd, serverfd);

        int retval = select(maxSocket+1, &readfds, NULL, &exceptfds, NULL);
        if (retval <= 0)
        {
            ERROR_ERRNO("select");
            break;
        }

        if (FD_ISSET(clientfd, &readfds))
        {
            if (exchange(clientfd, serverfd, exchangeBuffer) == -1)
            {
                break;
            }
        }
        if (FD_ISSET(serverfd, &readfds))
        {
            if (exchange(serverfd, clientfd, exchangeBuffer) == -1)
            {
                break;
            }
        }

        if (FD_ISSET(clientfd, &exceptfds))
        {
            DEBUG("exceptional condition on client fd (see the discussion of POLLPRI in poll(2))");
            if (exchange(clientfd, serverfd, exchangeBuffer) == -1)
            {
                break;
            }
        }
        if (FD_ISSET(serverfd, &exceptfds))
        {
            DEBUG("exceptional condition on server fd (see the discussion of POLLPRI in poll(2))");
            if (exchange(serverfd, clientfd, exchangeBuffer) == -1)
            {
                break;
            }
        }
    }

    free(exchangeBuffer);
    DEBUG("closing connection (client=%d; server=%d)", clientfd, serverfd);
}

// this function is used as entry point for forked processes
static void handleConnection(int clientSocket, struct sockaddr_in remoteServerAddr)
{
    int optval = SOCKET_OPTION_ENABLE;
    int serverSocket;
    if (setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
        ERROR_ERRNO("setsockopt failed to set keep-alive for client (proxy connection will not be created)");
        goto close_client;
    }

    // socket for remote server connection
    if ((serverSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR_ERRNO("failed to create socket (proxy connection will not be created)");
        goto close_client;
    }

    DEBUG("Connect to remote server");

    // create connection to remote server
    if (connect(serverSocket, (struct sockaddr *) &remoteServerAddr, sizeof(struct sockaddr)) < 0)
    {
        ERROR_ERRNO("failed to connect to remote server (proxy connection will not be created)");
        goto close_both;
    }

    if (setsockopt(serverSocket, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
        ERROR_ERRNO("setsockopt failed to set keep-alive for server (proxy connection will not be created)");
        goto close_both;
    }

    exchangeLoop(clientSocket, serverSocket);

close_both:
    close(serverSocket);
close_client:
    close(clientSocket);

    exit(0);
}

static void setDefaults()
{
    globalOptions.srcIP.s_addr = INADDR_ANY;
    globalOptions.exchangeBufferSize = 16384; // 16KiB
    globalOptions.currentVerbosityLevel = INFO;
    globalOptions.bindToInterface = 0;
    globalOptions.networkInterface[0] = '\0';
}

static void cleanup()
{
    close(globalProxyServerSocket);
}

int main(int argc, char *argv[])
{
    setDefaults();
    if (parseOptions(argc, argv) < 0)
    {
        printf("\n");
        usage(argv[0]);
        return -1;
    }

    const struct sockaddr_in proxyServerAddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(globalOptions.srcPort),
        .sin_addr   = globalOptions.srcIP,
    };

    struct sockaddr_in remoteServerAddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(globalOptions.dstPort),
        .sin_addr   = globalOptions.dstIP,
    };

    configSignals();
    signal(SIGCHLD, SIG_IGN);

    if ((globalProxyServerSocket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        ERROR_ERRNO("could not create socket (terminating program)");
        return -1;
    }

    if (globalOptions.bindToInterface)
    {
        if (setsockopt(globalProxyServerSocket, SOL_SOCKET, SO_BINDTODEVICE, globalOptions.networkInterface, strlen(globalOptions.networkInterface)) < 0)
        {
            ERROR_ERRNO("setsockopt(SO_BINDTODEVICE) failed");
            return -1;
        }
    }

    if (bind(globalProxyServerSocket, (const struct sockaddr *)&proxyServerAddr, sizeof(struct sockaddr)) < 0) {
        ERROR_ERRNO("could not bind to specified IP and port (terminating program)");
        return -1;
    }

    if (listen(globalProxyServerSocket, MAX_CONNECTIONS) < 0) {
        ERROR_ERRNO("could not listen on socket (terminating program)");
        return -1;
    }

    while (1)
    {
        // new client
        int clientSocket;
        socklen_t sin_size = sizeof(struct sockaddr_in);
        struct sockaddr_in c_addr = {0};
        if ((clientSocket = accept(globalProxyServerSocket, (struct sockaddr *)&c_addr, &sin_size)) < 0)
        {
            ERROR_ERRNO("could not accept client connection, waiting for next client");
            continue;
        }

        INFO("server: got connection from %s", inet_ntoa(c_addr.sin_addr));

        int pid = fork();
        if (pid == 0)
        {
            // child process
            handleConnection(clientSocket, remoteServerAddr);
            close(globalProxyServerSocket);
        }
        else if (pid < 0)
        {
            ERROR_ERRNO("fork failed");
        }

        close(clientSocket);
    } // main loop

    cleanup();

    return 0;
}
