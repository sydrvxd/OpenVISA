/*
 * OpenVISA - TCPIP HiSLIP Transport
 * Implements IVI-6.1 HiSLIP (High-Speed LAN Instrument Protocol)
 *
 * Protocol overview:
 *   - Two TCP connections to port 4880: Synchronous (commands/data) + Asynchronous (SRQ/events)
 *   - Binary framing: 16-byte header per message
 *   - Handshake: Initialize → InitializeResponse (sync), AsyncInitialize → AsyncInitializeResponse (async)
 *   - Data transfer: Data(6) for intermediate fragments, DataEnd(7) for last fragment (EOM)
 */

#include "../core/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#ifdef OPENVISA_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET ov_socket_t;
    #define OV_INVALID_SOCKET   INVALID_SOCKET
    #define ov_closesocket      closesocket
    #define ov_socket_error()   WSAGetLastError()
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
    #define OV_INVALID_SOCKET   (-1)
    #define ov_closesocket      close
    #define ov_socket_error()   errno
#endif

/* ========== HiSLIP Constants ========== */

#define HISLIP_DEFAULT_PORT             4880
#define HISLIP_HEADER_SIZE              16      /* bytes */
#define HISLIP_VERSION_MAJOR            1
#define HISLIP_VERSION_MINOR            0
#define HISLIP_MAX_DISCARD_BUF          4096

/* HiSLIP Message Types (IVI-6.1 Table 3) */
#define HISLIP_MSG_INITIALIZE                   0
#define HISLIP_MSG_INITIALIZE_RESPONSE          1
#define HISLIP_MSG_FATAL_ERROR                  2
#define HISLIP_MSG_ERROR                        3
#define HISLIP_MSG_ASYNC_LOCK                   4
#define HISLIP_MSG_ASYNC_LOCK_RESPONSE          5
#define HISLIP_MSG_DATA                         6
#define HISLIP_MSG_DATA_END                     7
#define HISLIP_MSG_DEVICE_CLEAR_COMPLETE        8
#define HISLIP_MSG_DEVICE_CLEAR_ACKNOWLEDGE     9
#define HISLIP_MSG_ASYNC_REMOTE_LOCAL_CONTROL   10
#define HISLIP_MSG_ASYNC_REMOTE_LOCAL_RESPONSE  11
#define HISLIP_MSG_TRIGGER                      12
#define HISLIP_MSG_INTERRUPTED                  13
#define HISLIP_MSG_ASYNC_INTERRUPTED            14
#define HISLIP_MSG_ASYNC_MAX_MSG_SIZE           15
#define HISLIP_MSG_ASYNC_MAX_MSG_SIZE_RESPONSE  16
#define HISLIP_MSG_ASYNC_INITIALIZE             17
#define HISLIP_MSG_ASYNC_INITIALIZE_RESPONSE    18
#define HISLIP_MSG_ASYNC_DEVICE_CLEAR           19
#define HISLIP_MSG_ASYNC_SERVICE_REQUEST        20
#define HISLIP_MSG_ASYNC_STATUS_QUERY           21
#define HISLIP_MSG_ASYNC_STATUS_RESPONSE        22
#define HISLIP_MSG_ASYNC_DEVICE_CLEAR_ACKNOWLEDGE 23
#define HISLIP_MSG_ASYNC_LOCK_INFO              24
#define HISLIP_MSG_ASYNC_LOCK_INFO_RESPONSE     25

/* ========== HiSLIP Header (network byte order on wire) ========== */

/*
 * Wire layout (16 bytes):
 *   [0-1]   Prologue = 0x48 'H', 0x53 'S'
 *   [2]     MessageType
 *   [3]     ControlCode
 *   [4-7]   MessageParameter  (big-endian uint32)
 *   [8-15]  PayloadLength     (big-endian uint64)
 */
typedef struct {
    uint8_t  msg_type;
    uint8_t  control_code;
    uint32_t msg_param;       /* host byte order after parsing */
    uint64_t payload_length;  /* host byte order after parsing */
} HiSLIPHeader;

/* ========== Transport Implementation State ========== */

typedef struct {
    ov_socket_t sync_sock;   /* Synchronous channel (commands + data) */
    ov_socket_t async_sock;  /* Asynchronous channel (SRQ, clear, STB) */
    char        host[256];
    uint16_t    port;
    uint16_t    session_id;  /* assigned by server in InitializeResponse */
    uint32_t    message_id;  /* client message ID, incremented by 2 per write */
    uint64_t    max_msg_size;/* negotiated maximum message size */
    char        sub_addr[256];/* LAN device name, e.g. "hislip0" */
} HiSLIPImpl;

/* ========== Platform Initialisation ========== */

#ifdef OPENVISA_WINDOWS
static int g_hislip_wsa_init = 0;
static void hislip_platform_init(void) {
    if (!g_hislip_wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_hislip_wsa_init = 1;
    }
}
#else
static void hislip_platform_init(void) { /* no-op on POSIX */ }
#endif

/* ========== Byte-order Helpers ========== */

/* Host → network 64-bit big-endian (manual, no htonll guarantee) */
static uint64_t hislip_hton64(uint64_t v) {
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFFu));
    uint64_t result;
    memcpy(            &result,     &hi, 4);
    memcpy((uint8_t*)&result + 4,  &lo, 4);
    return result;
}

/* Network 64-bit big-endian → host */
static uint64_t hislip_ntoh64(uint64_t v) {
    uint32_t hi, lo;
    memcpy(&hi,  (const uint8_t*)&v,     4);
    memcpy(&lo,  (const uint8_t*)&v + 4, 4);
    return ((uint64_t)ntohl(hi) << 32) | ntohl(lo);
}

/* ========== Non-blocking Connect (same pattern as tcpip_raw.c) ========== */

static ViStatus hislip_tcp_connect(ov_socket_t sock, struct addrinfo *addr, ViUInt32 timeout_ms) {
#ifdef OPENVISA_WINDOWS
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int rc = connect(sock, addr->ai_addr, (int)addr->ai_addrlen);
    if (rc == 0) goto connected;

    if (WSAGetLastError() != WSAEWOULDBLOCK)
        return VI_ERROR_CONN_LOST;

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);
    struct timeval tv = { (long)(timeout_ms / 1000), (long)((timeout_ms % 1000) * 1000) };
    rc = select(0, NULL, &writefds, NULL, &tv);
    if (rc <= 0) return VI_ERROR_TMO;

connected:
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
    return VI_SUCCESS;

#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, addr->ai_addr, addr->ai_addrlen);
    if (rc == 0) goto connected;

    if (errno != EINPROGRESS)
        return VI_ERROR_CONN_LOST;

    struct pollfd pfd = { sock, POLLOUT, 0 };
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc <= 0) return VI_ERROR_TMO;

    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) return VI_ERROR_CONN_LOST;

connected:
    fcntl(sock, F_SETFL, flags); /* restore blocking */
    return VI_SUCCESS;
#endif
}

/* ========== Reliable Send / Receive ========== */

/* Send exactly 'len' bytes, looping on short writes */
static ViStatus hislip_send_all(ov_socket_t sock, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    while (len > 0) {
        int n = send(sock, (const char *)ptr, (int)len, 0);
        if (n <= 0) return VI_ERROR_IO;
        ptr += n;
        len -= (size_t)n;
    }
    return VI_SUCCESS;
}

/* Receive exactly 'len' bytes within 'timeout_ms', looping on short reads */
static ViStatus hislip_recv_all(ov_socket_t sock, void *data, size_t len, ViUInt32 timeout_ms) {
    /* Apply socket-level timeout */
#ifdef OPENVISA_WINDOWS
    DWORD tv_dw = (DWORD)timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_dw, sizeof(tv_dw));
#else
    struct timeval tv = { (long)(timeout_ms / 1000), (long)((timeout_ms % 1000) * 1000) };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint8_t *ptr = (uint8_t *)data;
    size_t remaining = len;
    while (remaining > 0) {
        int n = recv(sock, (char *)ptr, (int)remaining, 0);
        if (n < 0) {
#ifdef OPENVISA_WINDOWS
            if (WSAGetLastError() == WSAETIMEDOUT) return VI_ERROR_TMO;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) return VI_ERROR_TMO;
#endif
            return VI_ERROR_IO;
        }
        if (n == 0) return VI_ERROR_CONN_LOST; /* peer closed */
        ptr      += n;
        remaining -= (size_t)n;
    }
    return VI_SUCCESS;
}

/* Discard exactly 'len' bytes from a socket */
static ViStatus hislip_discard(ov_socket_t sock, uint64_t len, ViUInt32 timeout_ms) {
    uint8_t buf[HISLIP_MAX_DISCARD_BUF];
    while (len > 0) {
        size_t chunk = (len < sizeof(buf)) ? (size_t)len : sizeof(buf);
        ViStatus st = hislip_recv_all(sock, buf, chunk, timeout_ms);
        if (st != VI_SUCCESS) return st;
        len -= chunk;
    }
    return VI_SUCCESS;
}

/* ========== HiSLIP Message Framing ========== */

/* Build and send a complete HiSLIP message (header + optional payload) */
static ViStatus hislip_send_msg(ov_socket_t sock,
                                 uint8_t  msg_type,
                                 uint8_t  ctrl_code,
                                 uint32_t msg_param,
                                 const void *payload,
                                 uint64_t   payload_len)
{
    uint8_t hdr[HISLIP_HEADER_SIZE];
    uint32_t mp_be = htonl(msg_param);
    uint64_t pl_be = hislip_hton64(payload_len);

    hdr[0] = 'H';
    hdr[1] = 'S';
    hdr[2] = msg_type;
    hdr[3] = ctrl_code;
    memcpy(hdr + 4, &mp_be, 4);
    memcpy(hdr + 8, &pl_be, 8);

    ViStatus st = hislip_send_all(sock, hdr, HISLIP_HEADER_SIZE);
    if (st != VI_SUCCESS) return st;

    if (payload && payload_len > 0)
        st = hislip_send_all(sock, payload, (size_t)payload_len);

    return st;
}

/* Receive and decode a HiSLIP header; does NOT read the payload */
static ViStatus hislip_recv_header(ov_socket_t sock, HiSLIPHeader *out, ViUInt32 timeout_ms) {
    uint8_t raw[HISLIP_HEADER_SIZE];
    ViStatus st = hislip_recv_all(sock, raw, HISLIP_HEADER_SIZE, timeout_ms);
    if (st != VI_SUCCESS) return st;

    if (raw[0] != 'H' || raw[1] != 'S')
        return VI_ERROR_IO; /* invalid prologue */

    uint32_t mp_be;
    uint64_t pl_be;
    memcpy(&mp_be, raw + 4, 4);
    memcpy(&pl_be, raw + 8, 8);

    out->msg_type      = raw[2];
    out->control_code  = raw[3];
    out->msg_param     = ntohl(mp_be);
    out->payload_length = hislip_ntoh64(pl_be);
    return VI_SUCCESS;
}

/* ========== Helper: create + connect a TCP socket ========== */

static ViStatus hislip_make_socket(const char *host, uint16_t port, ViUInt32 timeout,
                                    ov_socket_t *out)
{
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        return VI_ERROR_RSRC_NFOUND;

    ov_socket_t sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == OV_INVALID_SOCKET) {
        freeaddrinfo(result);
        return VI_ERROR_SYSTEM_ERROR;
    }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));

    ViStatus st = hislip_tcp_connect(sock, result, timeout);
    freeaddrinfo(result);

    if (st != VI_SUCCESS) {
        ov_closesocket(sock);
        return st;
    }

    *out = sock;
    return VI_SUCCESS;
}

/* ========== Transport vtable implementations ========== */

/*
 * hislip_open
 *
 * Establishes both TCP connections and performs the full HiSLIP handshake:
 *   1. Connect sync channel → Initialize → InitializeResponse (obtain SessionID)
 *   2. Connect async channel → AsyncInitialize → AsyncInitializeResponse
 */
static ViStatus hislip_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;
    HiSLIPHeader resp;
    ViStatus st;

    hislip_platform_init();

    strncpy(impl->host, rsrc->host, sizeof(impl->host) - 1);
    impl->port = (rsrc->port != 0) ? rsrc->port : HISLIP_DEFAULT_PORT;

    /* Determine LAN device sub-address (e.g. "hislip0") */
    if (rsrc->deviceName[0] != '\0')
        strncpy(impl->sub_addr, rsrc->deviceName, sizeof(impl->sub_addr) - 1);
    else
        strncpy(impl->sub_addr, "hislip0", sizeof(impl->sub_addr) - 1);

    impl->message_id   = 0;           /* starts at 0, client increments by 2 */
    impl->max_msg_size = OV_BUF_SIZE; /* default; could negotiate AsyncMaximumMessageSize */

    /* ------------------------------------------------------------------
     * Step 1: Synchronous channel – TCP connect
     * ------------------------------------------------------------------ */
    st = hislip_make_socket(impl->host, impl->port, timeout, &impl->sync_sock);
    if (st != VI_SUCCESS)
        return st;

    /* ------------------------------------------------------------------
     * Step 2: Send Initialize
     *
     *   MessageParameter (4 bytes, big-endian):
     *     [byte 0] MostSignificantVersion  = 1
     *     [byte 1] LeastSignificantVersion = 0
     *     [byte 2] VendorID high           = 0  (generic / unregistered)
     *     [byte 3] VendorID low            = 0
     *   Payload: Sub-address string (NOT null-terminated per spec)
     * ------------------------------------------------------------------ */
    uint32_t init_param = ((uint32_t)HISLIP_VERSION_MAJOR << 24)
                        | ((uint32_t)HISLIP_VERSION_MINOR << 16)
                        | 0x0000u; /* VendorID = 0x0000 */

    size_t sub_len = strlen(impl->sub_addr);
    st = hislip_send_msg(impl->sync_sock, HISLIP_MSG_INITIALIZE, 0,
                         init_param, impl->sub_addr, (uint64_t)sub_len);
    if (st != VI_SUCCESS) goto fail_sync;

    /* ------------------------------------------------------------------
     * Step 3: Receive InitializeResponse
     *
     *   MessageParameter:
     *     [byte 0] server MostSignificantVersion
     *     [byte 1] server LeastSignificantVersion
     *     [byte 2] SessionID high
     *     [byte 3] SessionID low
     *   Payload: ServerVendorID (2 bytes) – we discard it
     * ------------------------------------------------------------------ */
    st = hislip_recv_header(impl->sync_sock, &resp, timeout);
    if (st != VI_SUCCESS) goto fail_sync;

    if (resp.msg_type == HISLIP_MSG_FATAL_ERROR || resp.msg_type == HISLIP_MSG_ERROR) {
        st = VI_ERROR_IO;
        goto fail_sync;
    }
    if (resp.msg_type != HISLIP_MSG_INITIALIZE_RESPONSE) {
        st = VI_ERROR_IO;
        goto fail_sync;
    }

    /* Session ID lives in the lower 16 bits of MessageParameter */
    impl->session_id = (uint16_t)(resp.msg_param & 0xFFFFu);

    /* Discard payload (ServerVendorID) */
    if (resp.payload_length > 0) {
        st = hislip_discard(impl->sync_sock, resp.payload_length, timeout);
        if (st != VI_SUCCESS) goto fail_sync;
    }

    /* ------------------------------------------------------------------
     * Step 4: Asynchronous channel – TCP connect
     * ------------------------------------------------------------------ */
    st = hislip_make_socket(impl->host, impl->port, timeout, &impl->async_sock);
    if (st != VI_SUCCESS) goto fail_sync;

    /* ------------------------------------------------------------------
     * Step 5: Send AsyncInitialize
     *
     *   MessageParameter: SessionID (lower 16 bits)
     *   No payload
     * ------------------------------------------------------------------ */
    st = hislip_send_msg(impl->async_sock, HISLIP_MSG_ASYNC_INITIALIZE, 0,
                         (uint32_t)impl->session_id, NULL, 0);
    if (st != VI_SUCCESS) goto fail_async;

    /* ------------------------------------------------------------------
     * Step 6: Receive AsyncInitializeResponse
     * ------------------------------------------------------------------ */
    st = hislip_recv_header(impl->async_sock, &resp, timeout);
    if (st != VI_SUCCESS) goto fail_async;

    if (resp.msg_type != HISLIP_MSG_ASYNC_INITIALIZE_RESPONSE) {
        st = VI_ERROR_IO;
        goto fail_async;
    }

    /* Discard any payload */
    if (resp.payload_length > 0) {
        st = hislip_discard(impl->async_sock, resp.payload_length, timeout);
        if (st != VI_SUCCESS) goto fail_async;
    }

    return VI_SUCCESS;

fail_async:
    ov_closesocket(impl->async_sock);
    impl->async_sock = OV_INVALID_SOCKET;
fail_sync:
    ov_closesocket(impl->sync_sock);
    impl->sync_sock = OV_INVALID_SOCKET;
    return st;
}

/*
 * hislip_close
 *
 * Shuts down both TCP sockets.  Async first (clean server-side state),
 * then sync.
 */
static ViStatus hislip_close(OvTransport *self) {
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;

    if (impl->async_sock != OV_INVALID_SOCKET) {
        ov_closesocket(impl->async_sock);
        impl->async_sock = OV_INVALID_SOCKET;
    }
    if (impl->sync_sock != OV_INVALID_SOCKET) {
        ov_closesocket(impl->sync_sock);
        impl->sync_sock = OV_INVALID_SOCKET;
    }
    return VI_SUCCESS;
}

/*
 * hislip_write
 *
 * Sends data as a single DataEnd message (one fragment = EOM).
 * For instruments that require fragmented transfers (very large payloads
 * exceeding max_msg_size), we split into Data + DataEnd segments.
 *
 * MessageID starts at 0 and is incremented by 2 before each new message.
 */
static ViStatus hislip_write(OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;
    if (impl->sync_sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    /* Advance message ID (always even, wraps at UINT32_MAX) */
    impl->message_id += 2;

    ViStatus st;
    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t remaining = count;
    uint64_t frag_size = impl->max_msg_size;

    if (frag_size == 0) frag_size = OV_BUF_SIZE;

    while (remaining > 0) {
        uint64_t chunk = (remaining > frag_size) ? frag_size : remaining;
        uint8_t  msg_type = (chunk < remaining)
                            ? HISLIP_MSG_DATA      /* more fragments follow */
                            : HISLIP_MSG_DATA_END; /* last fragment / EOM   */

        st = hislip_send_msg(impl->sync_sock, msg_type, 0,
                             impl->message_id, ptr, chunk);
        if (st != VI_SUCCESS) return st;

        ptr       += chunk;
        remaining -= (uint32_t)chunk;
    }

    if (retCount) *retCount = count;
    return VI_SUCCESS;
}

/*
 * hislip_read
 *
 * Receives Data / DataEnd fragments from the instrument until DataEnd (EOM).
 * Fills the user buffer; excess payload bytes are discarded (VI_SUCCESS_MAX_CNT).
 */
static ViStatus hislip_read(OvTransport *self, ViBuf buf, ViUInt32 count,
                             ViUInt32 *retCount, ViUInt32 timeout)
{
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;
    if (impl->sync_sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    ViUInt32 total        = 0;
    ViStatus final_status = VI_SUCCESS;

    for (;;) {
        HiSLIPHeader hdr;
        ViStatus st = hislip_recv_header(impl->sync_sock, &hdr, timeout);
        if (st != VI_SUCCESS) return st;

        /* Handle protocol error messages */
        if (hdr.msg_type == HISLIP_MSG_FATAL_ERROR || hdr.msg_type == HISLIP_MSG_ERROR) {
            hislip_discard(impl->sync_sock, hdr.payload_length, timeout);
            return VI_ERROR_IO;
        }

        /* Skip unexpected message types (e.g. Trigger, Interrupted) */
        if (hdr.msg_type != HISLIP_MSG_DATA && hdr.msg_type != HISLIP_MSG_DATA_END) {
            hislip_discard(impl->sync_sock, hdr.payload_length, timeout);
            continue;
        }

        uint64_t payload_len = hdr.payload_length;
        uint32_t space       = count - total;

        if (payload_len <= (uint64_t)space) {
            /* Fragment fits entirely into the user buffer */
            st = hislip_recv_all(impl->sync_sock, buf + total, (size_t)payload_len, timeout);
            if (st != VI_SUCCESS) return st;
            total += (ViUInt32)payload_len;
        } else {
            /* More data than remaining buffer space → truncate */
            st = hislip_recv_all(impl->sync_sock, buf + total, space, timeout);
            if (st != VI_SUCCESS) return st;
            total += space;

            /* Discard overflow */
            st = hislip_discard(impl->sync_sock, payload_len - space, timeout);
            if (st != VI_SUCCESS) return st;

            final_status = VI_SUCCESS_MAX_CNT;
        }

        /* DataEnd = last fragment; stop looping */
        if (hdr.msg_type == HISLIP_MSG_DATA_END)
            break;
    }

    if (retCount) *retCount = total;
    return final_status;
}

/*
 * hislip_readSTB
 *
 * Queries the instrument status byte via AsyncStatusQuery on the async channel.
 * The server responds with AsyncStatusResponse where ControlCode = status byte.
 */
static ViStatus hislip_readSTB(OvTransport *self, ViUInt16 *status) {
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;
    if (impl->async_sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    /*
     * AsyncStatusQuery:
     *   ControlCode = 0 (not requesting RQS info, just the STB)
     *   MessageParameter = current MessageID (for ordering)
     */
    ViStatus st = hislip_send_msg(impl->async_sock, HISLIP_MSG_ASYNC_STATUS_QUERY, 0,
                                   impl->message_id, NULL, 0);
    if (st != VI_SUCCESS) return st;

    HiSLIPHeader resp;
    st = hislip_recv_header(impl->async_sock, &resp, 5000);
    if (st != VI_SUCCESS) return st;

    if (resp.msg_type != HISLIP_MSG_ASYNC_STATUS_RESPONSE)
        return VI_ERROR_IO;

    /* Status byte is returned in the ControlCode field */
    if (status) *status = (ViUInt16)resp.control_code;

    /* Discard any payload (spec says none, but be defensive) */
    if (resp.payload_length > 0)
        hislip_discard(impl->async_sock, resp.payload_length, 5000);

    return VI_SUCCESS;
}

/*
 * hislip_clear
 *
 * Performs a HiSLIP device clear sequence:
 *   1. Send AsyncDeviceClear on async channel
 *   2. Receive AsyncDeviceClearAcknowledge on async channel
 *   3. Receive DeviceClearComplete on sync channel
 *   4. Send DeviceClearAcknowledge on sync channel
 *
 * This flushes both the device's input and output buffers and resets
 * the message ID sequence.
 */
static ViStatus hislip_clear(OvTransport *self) {
    HiSLIPImpl *impl = (HiSLIPImpl *)self->impl;
    if (impl->async_sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;
    if (impl->sync_sock  == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    /* Step 1: AsyncDeviceClear → async channel */
    ViStatus st = hislip_send_msg(impl->async_sock, HISLIP_MSG_ASYNC_DEVICE_CLEAR, 0, 0, NULL, 0);
    if (st != VI_SUCCESS) return st;

    /* Step 2: AsyncDeviceClearAcknowledge ← async channel */
    HiSLIPHeader hdr;
    st = hislip_recv_header(impl->async_sock, &hdr, 5000);
    if (st != VI_SUCCESS) return st;

    if (hdr.msg_type != HISLIP_MSG_ASYNC_DEVICE_CLEAR_ACKNOWLEDGE) {
        hislip_discard(impl->async_sock, hdr.payload_length, 5000);
        return VI_ERROR_IO;
    }
    if (hdr.payload_length > 0)
        hislip_discard(impl->async_sock, hdr.payload_length, 5000);

    /* Step 3: DeviceClearComplete ← sync channel */
    st = hislip_recv_header(impl->sync_sock, &hdr, 5000);
    if (st != VI_SUCCESS) return st;

    if (hdr.msg_type != HISLIP_MSG_DEVICE_CLEAR_COMPLETE) {
        hislip_discard(impl->sync_sock, hdr.payload_length, 5000);
        return VI_ERROR_IO;
    }
    uint8_t feature_flags = hdr.control_code;
    if (hdr.payload_length > 0)
        hislip_discard(impl->sync_sock, hdr.payload_length, 5000);

    /* Step 4: DeviceClearAcknowledge → sync channel
     *   ControlCode mirrors the feature flags received from the device
     */
    st = hislip_send_msg(impl->sync_sock, HISLIP_MSG_DEVICE_CLEAR_ACKNOWLEDGE,
                         feature_flags, 0, NULL, 0);
    if (st != VI_SUCCESS) return st;

    /* Reset message ID after device clear (spec §6.5.3) */
    impl->message_id = 0;

    return VI_SUCCESS;
}

/* ========== Factory ========== */

OvTransport *ov_transport_tcpip_hislip_create(void) {
    OvTransport *t = (OvTransport *)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    HiSLIPImpl *impl = (HiSLIPImpl *)calloc(1, sizeof(HiSLIPImpl));
    if (!impl) {
        free(t);
        return NULL;
    }

    impl->sync_sock   = OV_INVALID_SOCKET;
    impl->async_sock  = OV_INVALID_SOCKET;
    impl->message_id  = 0;
    impl->max_msg_size = OV_BUF_SIZE;

    t->impl     = impl;
    t->open     = hislip_open;
    t->close    = hislip_close;
    t->read     = hislip_read;
    t->write    = hislip_write;
    t->readSTB  = hislip_readSTB;
    t->clear    = hislip_clear;

    return t;
}
