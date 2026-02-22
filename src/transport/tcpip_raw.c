/*
 * OpenVISA - TCPIP Raw Socket Transport
 * Handles TCPIP::host::port::SOCKET and basic SCPI over TCP
 */

#include "../core/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef OPENVISA_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET ov_socket_t;
    #define OV_INVALID_SOCKET INVALID_SOCKET
    #define ov_closesocket closesocket
    #define ov_socket_error() WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    typedef int ov_socket_t;
    #define OV_INVALID_SOCKET (-1)
    #define ov_closesocket close
    #define ov_socket_error() errno
#endif

typedef struct {
    ov_socket_t sock;
    char host[256];
    uint16_t port;
} TcpipRawImpl;

/* ========== Platform init ========== */

#ifdef OPENVISA_WINDOWS
static bool g_wsa_initialized = false;

static void tcpip_platform_init(void) {
    if (!g_wsa_initialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsa_initialized = true;
    }
}
#else
static void tcpip_platform_init(void) { /* no-op on POSIX */ }
#endif

/* ========== Connect with timeout ========== */

static ViStatus tcpip_connect(ov_socket_t sock, struct addrinfo *addr, ViUInt32 timeout_ms) {
#ifdef OPENVISA_WINDOWS
    /* Set non-blocking */
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int rc = connect(sock, addr->ai_addr, (int)addr->ai_addrlen);
    if (rc == 0) goto connected;

    if (WSAGetLastError() != WSAEWOULDBLOCK)
        return VI_ERROR_CONN_LOST;

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };

    rc = select(0, NULL, &writefds, NULL, &tv);
    if (rc <= 0) return VI_ERROR_TMO;

connected:
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
    return VI_SUCCESS;

#else
    /* Set non-blocking */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, addr->ai_addr, addr->ai_addrlen);
    if (rc == 0) goto connected;

    if (errno != EINPROGRESS)
        return VI_ERROR_CONN_LOST;

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc <= 0) return VI_ERROR_TMO;

    /* Check for connection error */
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) return VI_ERROR_CONN_LOST;

connected:
    fcntl(sock, F_SETFL, flags); /* restore blocking */
    return VI_SUCCESS;
#endif
}

/* ========== Transport Operations ========== */

static ViStatus tcpip_raw_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    TcpipRawImpl *impl = (TcpipRawImpl*)self->impl;

    tcpip_platform_init();

    strncpy(impl->host, rsrc->host, sizeof(impl->host) - 1);
    impl->port = rsrc->port;

    /* Default port for raw SCPI-over-TCP is 5025 if SOCKET mode */
    if (rsrc->isSocket && impl->port == 0)
        impl->port = 5025;
    /* For VXI-11 (non-socket INSTR), default port is 111 (portmapper) */
    /* But for now, raw transport handles SOCKET only. VXI-11 needs RPC. */

    /* Resolve hostname */
    struct addrinfo hints = {0}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", impl->port);

    int rc = getaddrinfo(impl->host, port_str, &hints, &result);
    if (rc != 0) return VI_ERROR_RSRC_NFOUND;

    /* Create socket */
    impl->sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (impl->sock == OV_INVALID_SOCKET) {
        freeaddrinfo(result);
        return VI_ERROR_SYSTEM_ERROR;
    }

    /* TCP_NODELAY for low latency */
    int flag = 1;
    setsockopt(impl->sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    /* Connect with timeout */
    ViStatus st = tcpip_connect(impl->sock, result, timeout);
    freeaddrinfo(result);

    if (st != VI_SUCCESS) {
        ov_closesocket(impl->sock);
        impl->sock = OV_INVALID_SOCKET;
        return st;
    }

    return VI_SUCCESS;
}

static ViStatus tcpip_raw_close(OvTransport *self) {
    TcpipRawImpl *impl = (TcpipRawImpl*)self->impl;
    if (impl->sock != OV_INVALID_SOCKET) {
        ov_closesocket(impl->sock);
        impl->sock = OV_INVALID_SOCKET;
    }
    return VI_SUCCESS;
}

static ViStatus tcpip_raw_write(OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    TcpipRawImpl *impl = (TcpipRawImpl*)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    int sent = send(impl->sock, (const char*)buf, count, 0);
    if (sent < 0) return VI_ERROR_IO;

    if (retCount) *retCount = (ViUInt32)sent;
    return VI_SUCCESS;
}

static ViStatus tcpip_raw_read(OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount, ViUInt32 timeout) {
    TcpipRawImpl *impl = (TcpipRawImpl*)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    /* Set receive timeout */
#ifdef OPENVISA_WINDOWS
    DWORD tv = timeout;
    setsockopt(impl->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    setsockopt(impl->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    int received = recv(impl->sock, (char*)buf, count, 0);
    if (received < 0) {
#ifdef OPENVISA_WINDOWS
        if (WSAGetLastError() == WSAETIMEDOUT)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            return VI_ERROR_TMO;
        return VI_ERROR_IO;
    }
    if (received == 0) return VI_ERROR_CONN_LOST;

    if (retCount) *retCount = (ViUInt32)received;

    /* Check if terminated by newline */
    if (received > 0 && buf[received - 1] == '\n')
        return VI_SUCCESS_TERM_CHAR;

    return VI_SUCCESS;
}

static ViStatus tcpip_raw_readSTB(OvTransport *self, ViUInt16 *status) {
    /* Send *STB? and parse response */
    const char *cmd = "*STB?\n";
    ViUInt32 retCount;
    ViStatus st = tcpip_raw_write(self, (ViBuf)cmd, 6, &retCount);
    if (st != VI_SUCCESS) return st;

    char buf[64];
    st = tcpip_raw_read(self, (ViBuf)buf, sizeof(buf) - 1, &retCount, 5000);
    if (st != VI_SUCCESS && st != VI_SUCCESS_TERM_CHAR) return st;

    buf[retCount] = '\0';
    if (status) *status = (ViUInt16)atoi(buf);
    return VI_SUCCESS;
}

static ViStatus tcpip_raw_clear(OvTransport *self) {
    /* Send *CLS */
    const char *cmd = "*CLS\n";
    ViUInt32 retCount;
    return tcpip_raw_write(self, (ViBuf)cmd, 5, &retCount);
}

/* ========== Factory ========== */

OvTransport* ov_transport_tcpip_raw_create(void) {
    OvTransport *t = (OvTransport*)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    TcpipRawImpl *impl = (TcpipRawImpl*)calloc(1, sizeof(TcpipRawImpl));
    if (!impl) { free(t); return NULL; }

    impl->sock = OV_INVALID_SOCKET;
    t->impl = impl;
    t->open = tcpip_raw_open;
    t->close = tcpip_raw_close;
    t->read = tcpip_raw_read;
    t->write = tcpip_raw_write;
    t->readSTB = tcpip_raw_readSTB;
    t->clear = tcpip_raw_clear;

    return t;
}
