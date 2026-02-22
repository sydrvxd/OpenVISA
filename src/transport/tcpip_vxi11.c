/*
 * OpenVISA - TCPIP VXI-11 Transport
 *
 * Implements VXI-11 over ONC RPC (RFC 5531 / Sun RPC) without any external
 * RPC library.  Uses the same socket-abstraction pattern as tcpip_raw.c.
 *
 * Protocol overview
 * -----------------
 *  - TCP Record Marking (RM) wraps every RPC message:
 *      4-byte header: bit31 = last-fragment, bits30-0 = fragment length
 *  - XDR (big-endian, 4-byte aligned) encodes all fields
 *  - Portmapper (prog 100000 v2, proc 3 = GETPORT) on port 111 returns the
 *    VXI-11 Core port
 *  - VXI-11 Core RPC prog 0x0607AF v1 carries all instrument operations
 *
 * Procedures implemented: create_link (10), device_write (11), device_read
 * (12), device_readstb (13), device_clear (15), destroy_link (23).
 */

/* Enable POSIX 2001 extensions (struct addrinfo, struct timeval, poll, etc.)
 * when compiling with strict C standards modes (e.g. -std=c11). */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "../core/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* ========== Platform socket abstraction ========== */

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
    #include <sys/time.h>
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

/* ========== VXI-11 / ONC RPC constants ========== */

#define VXI11_CORE_PROG         0x0607AFu
#define VXI11_CORE_VERS         1u

#define PORTMAP_PROG            100000u
#define PORTMAP_VERS            2u
#define PORTMAP_PROC_GETPORT    3u
#define PORTMAP_PORT            111

/* VXI-11 Core procedure numbers */
#define VXI11_PROC_CREATE_LINK   10u
#define VXI11_PROC_DEVICE_WRITE  11u
#define VXI11_PROC_DEVICE_READ   12u
#define VXI11_PROC_DEVICE_READSTB 13u
#define VXI11_PROC_DEVICE_TRIGGER 14u
#define VXI11_PROC_DEVICE_CLEAR  15u
#define VXI11_PROC_DEVICE_REMOTE 16u
#define VXI11_PROC_DEVICE_LOCAL  17u
#define VXI11_PROC_DEVICE_LOCK   18u
#define VXI11_PROC_DEVICE_UNLOCK 19u
#define VXI11_PROC_DESTROY_LINK  23u

/* Device_Flags bits */
#define VXI11_FLAG_WAITLOCK     0x01u
#define VXI11_FLAG_END          0x08u
#define VXI11_FLAG_TERMCHRSET   0x80u

/* reason bits in device_read reply */
#define VXI11_REASON_REQCNT     0x01u   /* requested byte count reached */
#define VXI11_REASON_CHR        0x02u   /* term char matched */
#define VXI11_REASON_END        0x04u   /* END indicator from device */

/* RPC message types */
#define RPC_CALL                0u
#define RPC_REPLY               1u
#define RPC_MSG_ACCEPTED        0u
#define RPC_ACCEPT_SUCCESS      0u
#define RPC_VERS                2u
#define AUTH_NULL               0u

/* Internal buffer sizes */
#define VXI11_HDR_BUF           128u    /* max RPC call header */
#define VXI11_MAX_MSG           (65536u + 1024u)   /* max send/recv buffer */

/* Default write timeout (write vtable has no timeout arg) */
#define VXI11_WRITE_TIMEOUT_MS  10000u

/* ========== Transport implementation state ========== */

typedef struct {
    ov_socket_t sock;
    char        host[256];
    uint16_t    core_port;
    int32_t     lid;            /* Device_Link returned by create_link */
    uint32_t    xid;            /* rolling RPC transaction ID */
    uint32_t    max_recv_size;  /* advertised by create_link reply */
    char        device[256];    /* LAN device name, e.g. "inst0" */
} Vxi11Impl;

/* ========== Platform initialisation ========== */

#ifdef OPENVISA_WINDOWS
static volatile int g_vxi_wsa_init = 0;
static void vxi11_platform_init(void) {
    if (!g_vxi_wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_vxi_wsa_init = 1;
    }
}
#else
static void vxi11_platform_init(void) { /* no-op on POSIX */ }
#endif

/* ========== XDR helpers ========== */

/* Encode unsigned 32-bit integer into buf (big-endian). Returns bytes written (4). */
static uint32_t xdr_put_u32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >>  8);
    buf[3] = (uint8_t)(v      );
    return 4;
}

/* Encode signed 32-bit integer (same wire format). Returns 4. */
static uint32_t xdr_put_i32(uint8_t *buf, int32_t v)
{
    return xdr_put_u32(buf, (uint32_t)v);
}

/* Decode unsigned 32-bit integer from buf. Returns bytes consumed (4). */
static uint32_t xdr_get_u32(const uint8_t *buf, uint32_t *out)
{
    *out = ((uint32_t)buf[0] << 24)
         | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] <<  8)
         | ((uint32_t)buf[3]      );
    return 4;
}

/* Decode signed 32-bit integer from buf. Returns bytes consumed (4). */
static uint32_t xdr_get_i32(const uint8_t *buf, int32_t *out)
{
    uint32_t u;
    uint32_t n = xdr_get_u32(buf, &u);
    *out = (int32_t)u;
    return n;
}

/*
 * Encode an XDR string (or variable-length opaque with NUL-terminated src).
 * Layout: [4-byte length][data bytes][0-3 pad bytes to align to 4]
 * Returns total bytes written.
 */
static uint32_t xdr_put_string(uint8_t *buf, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    uint32_t pad = (4u - (len & 3u)) & 3u;
    uint32_t n   = xdr_put_u32(buf, len);
    memcpy(buf + n, s, len);
    n += len;
    memset(buf + n, 0, pad);
    n += pad;
    return n;
}

/*
 * Encode variable-length opaque data.
 * Layout: [4-byte length][data][0-3 pad bytes]
 * Returns total bytes written.
 */
static uint32_t xdr_put_opaque(uint8_t *buf, const uint8_t *data, uint32_t len)
{
    uint32_t pad = (4u - (len & 3u)) & 3u;
    uint32_t n   = xdr_put_u32(buf, len);
    if (data && len) memcpy(buf + n, data, len);
    n += len;
    memset(buf + n, 0, pad);
    n += pad;
    return n;
}

/*
 * Decode variable-length opaque / string from buf.
 * Copies up to out_max bytes into out_data (may be NULL to skip).
 * Sets *out_len to the actual data length (before truncation).
 * Returns total bytes consumed (length field + padded data).
 */
static uint32_t xdr_get_opaque(const uint8_t *buf,
                                uint8_t *out_data, uint32_t out_max,
                                uint32_t *out_len)
{
    uint32_t len, n;
    n = xdr_get_u32(buf, &len);
    uint32_t pad  = (4u - (len & 3u)) & 3u;
    if (out_len) *out_len = len;
    if (out_data) {
        uint32_t copy = (len < out_max) ? len : out_max;
        memcpy(out_data, buf + n, copy);
    }
    n += len + pad;
    return n;
}

/* ========== RPC call-header builder ========== */

/*
 * Write an ONC RPC Call message header into buf.
 * Returns number of bytes written (always the same, 10 × 4 = 40 bytes).
 *
 * Layout (XDR):
 *   xid, CALL(0), rpcvers(2), prog, vers, proc,
 *   cred(AUTH_NULL: flavor=0, len=0),
 *   verf(AUTH_NULL: flavor=0, len=0)
 */
static uint32_t rpc_build_call_hdr(uint8_t *buf, uint32_t xid,
                                    uint32_t prog, uint32_t vers, uint32_t proc)
{
    uint32_t n = 0;
    n += xdr_put_u32(buf + n, xid);
    n += xdr_put_u32(buf + n, RPC_CALL);
    n += xdr_put_u32(buf + n, RPC_VERS);
    n += xdr_put_u32(buf + n, prog);
    n += xdr_put_u32(buf + n, vers);
    n += xdr_put_u32(buf + n, proc);
    /* AUTH_NULL credential */
    n += xdr_put_u32(buf + n, AUTH_NULL);
    n += xdr_put_u32(buf + n, 0u);
    /* AUTH_NULL verifier */
    n += xdr_put_u32(buf + n, AUTH_NULL);
    n += xdr_put_u32(buf + n, 0u);
    return n;  /* = 40 */
}

/* ========== TCP Record Marking send / receive ========== */

/*
 * Send msg as a single-fragment RPC record over sock.
 * Record-mark header: bit31=1 (last fragment), bits30-0 = len.
 */
static ViStatus rm_send(ov_socket_t sock, const uint8_t *msg, uint32_t len)
{
    uint8_t  rm[4];
    uint32_t rm_val = 0x80000000u | len;
    rm[0] = (uint8_t)(rm_val >> 24);
    rm[1] = (uint8_t)(rm_val >> 16);
    rm[2] = (uint8_t)(rm_val >>  8);
    rm[3] = (uint8_t)(rm_val      );

    if (send(sock, (const char *)rm, 4, 0) != 4) return VI_ERROR_IO;

    uint32_t sent = 0;
    while (sent < len) {
        int rc = send(sock, (const char *)(msg + sent), (int)(len - sent), 0);
        if (rc <= 0) return VI_ERROR_IO;
        sent += (uint32_t)rc;
    }
    return VI_SUCCESS;
}

/* Receive exactly n bytes from sock, handling partial reads. */
static ViStatus rm_recv_exact(ov_socket_t sock, uint8_t *buf, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        int rc = recv(sock, (char *)(buf + got), (int)(n - got), 0);
        if (rc == 0) return VI_ERROR_CONN_LOST;
        if (rc < 0) {
#ifdef OPENVISA_WINDOWS
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT) return VI_ERROR_TMO;
#else
            int e = errno;
            if (e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT) return VI_ERROR_TMO;
#endif
            (void)e;
            return VI_ERROR_IO;
        }
        got += (uint32_t)rc;
    }
    return VI_SUCCESS;
}

/*
 * Receive one complete RPC record (possibly multiple fragments) into buf.
 * Sets SO_RCVTIMEO to timeout_ms before reading.
 * Returns VI_SUCCESS and sets *out_len to total payload length.
 */
static ViStatus rm_recv(ov_socket_t sock,
                        uint8_t *buf, uint32_t buf_size,
                        uint32_t *out_len,
                        ViUInt32 timeout_ms)
{
    /* Apply receive timeout */
#ifdef OPENVISA_WINDOWS
    DWORD tvw = (DWORD)timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tvw, sizeof(tvw));
#else
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint32_t total    = 0;
    int      last_frag = 0;

    while (!last_frag) {
        /* Read 4-byte record mark */
        uint8_t  rm[4];
        ViStatus st = rm_recv_exact(sock, rm, 4);
        if (st != VI_SUCCESS) return st;

        uint32_t rm_val  = ((uint32_t)rm[0] << 24) | ((uint32_t)rm[1] << 16)
                         | ((uint32_t)rm[2] <<  8) | ((uint32_t)rm[3]);
        last_frag        = (rm_val & 0x80000000u) ? 1 : 0;
        uint32_t frag_len = rm_val & 0x7FFFFFFFu;

        if (total + frag_len > buf_size) return VI_ERROR_INV_SETUP;

        st = rm_recv_exact(sock, buf + total, frag_len);
        if (st != VI_SUCCESS) return st;
        total += frag_len;
    }

    if (out_len) *out_len = total;
    return VI_SUCCESS;
}

/* ========== RPC reply parser ========== */

/*
 * Validate an RPC reply header in buf[0..len).
 * Returns byte offset of the procedure result data, or -1 on error.
 *
 * Expected layout:
 *   xid, REPLY(1), MSG_ACCEPTED(0),
 *   verf(flavor, len, [data]),
 *   accept_stat(0 = SUCCESS),
 *   <result data>
 */
static int rpc_parse_reply(const uint8_t *buf, uint32_t len, uint32_t expected_xid)
{
    if (len < 24u) return -1;

    uint32_t xid, msg_type, reply_stat, verf_flavor, verf_len, accept_stat;
    uint32_t p = 0;

    p += xdr_get_u32(buf + p, &xid);
    p += xdr_get_u32(buf + p, &msg_type);
    if (xid != expected_xid || msg_type != RPC_REPLY) return -1;

    p += xdr_get_u32(buf + p, &reply_stat);
    if (reply_stat != RPC_MSG_ACCEPTED) return -1;

    /* Skip verifier (AUTH_NULL → flavor=0, len=0; but be tolerant) */
    p += xdr_get_u32(buf + p, &verf_flavor);
    p += xdr_get_u32(buf + p, &verf_len);
    /* verf body (if any) is padded to 4-byte boundary */
    uint32_t verf_body = verf_len + ((4u - (verf_len & 3u)) & 3u);
    if (p + verf_body + 4u > len) return -1;
    p += verf_body;

    p += xdr_get_u32(buf + p, &accept_stat);
    if (accept_stat != RPC_ACCEPT_SUCCESS) return -1;

    return (int)p;
}

/* ========== Non-blocking connect helper ========== */

static ViStatus vxi11_connect(ov_socket_t sock,
                               struct addrinfo *ai,
                               ViUInt32 timeout_ms)
{
#ifdef OPENVISA_WINDOWS
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int rc = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
    if (rc == 0) goto done;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return VI_ERROR_CONN_LOST;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = { (long)(timeout_ms / 1000u), (long)((timeout_ms % 1000u) * 1000u) };
    rc = select(0, NULL, &wfds, NULL, &tv);
    if (rc <= 0) return VI_ERROR_TMO;

done:
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
    return VI_SUCCESS;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) goto done;
    if (errno != EINPROGRESS) return VI_ERROR_CONN_LOST;

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc == 0) return VI_ERROR_TMO;
    if (rc < 0)  return VI_ERROR_CONN_LOST;

    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) return VI_ERROR_CONN_LOST;

done:
    fcntl(sock, F_SETFL, flags); /* restore blocking */
    return VI_SUCCESS;
#endif
}

/* Open a TCP connection to host:port with timeout. Returns the socket. */
static ViStatus vxi11_tcp_connect(const char *host, uint16_t port,
                                   ViUInt32 timeout_ms,
                                   ov_socket_t *out_sock)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        return VI_ERROR_RSRC_NFOUND;

    ov_socket_t sock = socket(result->ai_family,
                               result->ai_socktype,
                               result->ai_protocol);
    if (sock == OV_INVALID_SOCKET) {
        freeaddrinfo(result);
        return VI_ERROR_SYSTEM_ERROR;
    }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

    ViStatus st = vxi11_connect(sock, result, timeout_ms);
    freeaddrinfo(result);
    if (st != VI_SUCCESS) {
        ov_closesocket(sock);
        return st;
    }

    *out_sock = sock;
    return VI_SUCCESS;
}

/* ========== Portmapper GETPORT ========== */

/*
 * Ask the portmapper on host:111 for the TCP port of VXI11_CORE_PROG v1.
 * Uses a transient TCP connection (closed after the query).
 */
static ViStatus vxi11_getport(Vxi11Impl *impl, ViUInt32 timeout_ms,
                               uint16_t *out_port)
{
    ov_socket_t sock;
    ViStatus st = vxi11_tcp_connect(impl->host, PORTMAP_PORT, timeout_ms, &sock);
    if (st != VI_SUCCESS) return st;

    /* Build GETPORT call */
    uint8_t  msg[256];
    uint32_t xid = impl->xid++;
    uint32_t n   = rpc_build_call_hdr(msg, xid, PORTMAP_PROG, PORTMAP_VERS,
                                        PORTMAP_PROC_GETPORT);
    /* Mapping: { prog, vers, prot=IPPROTO_TCP(6), port=0 } */
    n += xdr_put_u32(msg + n, VXI11_CORE_PROG);
    n += xdr_put_u32(msg + n, VXI11_CORE_VERS);
    n += xdr_put_u32(msg + n, 6u);   /* IPPROTO_TCP */
    n += xdr_put_u32(msg + n, 0u);

    st = rm_send(sock, msg, n);
    if (st != VI_SUCCESS) { ov_closesocket(sock); return st; }

    uint8_t  rbuf[256];
    uint32_t rlen = 0;
    st = rm_recv(sock, rbuf, sizeof(rbuf), &rlen, timeout_ms);
    ov_closesocket(sock);
    if (st != VI_SUCCESS) return st;

    int off = rpc_parse_reply(rbuf, rlen, xid);
    if (off < 0) return VI_ERROR_IO;

    uint32_t port = 0;
    xdr_get_u32(rbuf + (uint32_t)off, &port);
    if (port == 0 || port > 65535u) return VI_ERROR_RSRC_NFOUND;

    *out_port = (uint16_t)port;
    return VI_SUCCESS;
}

/* ========== Generic VXI-11 call helper ========== */

/*
 * Send a VXI-11 Core RPC call, receive the reply, validate it, and set
 * *roff to the byte offset of the procedure result data within rbuf.
 *
 * The call message is assembled as:  [RPC header | params[0..params_len)]
 * rbuf/rbuf_size are caller-supplied to avoid heap allocation on every call.
 */
static ViStatus vxi11_call(Vxi11Impl *impl,
                            uint32_t proc,
                            const uint8_t *params, uint32_t params_len,
                            uint8_t *rbuf, uint32_t rbuf_size,
                            uint32_t *roff,
                            ViUInt32 timeout_ms)
{
    /* Assemble call message */
    uint8_t  hdr[VXI11_HDR_BUF];
    uint32_t xid = impl->xid++;
    uint32_t hn  = rpc_build_call_hdr(hdr, xid,
                                       VXI11_CORE_PROG, VXI11_CORE_VERS, proc);

    /* Avoid a large stack buffer – use a single dynamically allocated frame */
    uint32_t  msg_len = hn + params_len;
    uint8_t  *msg     = (uint8_t *)malloc(msg_len);
    if (!msg) return VI_ERROR_ALLOC;

    memcpy(msg,      hdr,    hn);
    if (params && params_len)
        memcpy(msg + hn, params, params_len);

    ViStatus st = rm_send(impl->sock, msg, msg_len);
    free(msg);
    if (st != VI_SUCCESS) return st;

    uint32_t rlen = 0;
    st = rm_recv(impl->sock, rbuf, rbuf_size, &rlen, timeout_ms);
    if (st != VI_SUCCESS) return st;

    int off = rpc_parse_reply(rbuf, rlen, xid);
    if (off < 0) return VI_ERROR_IO;

    *roff = (uint32_t)off;
    return VI_SUCCESS;
}

/* ========== Transport operation implementations ========== */

static ViStatus vxi11_open(OvTransport *self,
                            const OvResource *rsrc,
                            ViUInt32 timeout)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    vxi11_platform_init();

    strncpy(impl->host, rsrc->host, sizeof(impl->host) - 1);

    /* Device name: parsed from resource string (e.g. "inst0") or default */
    const char *devname = rsrc->deviceName[0] ? rsrc->deviceName : "inst0";
    strncpy(impl->device, devname, sizeof(impl->device) - 1);

    /* Seed XID from current time XOR impl address for uniqueness */
    impl->xid = (uint32_t)((uintptr_t)time(NULL) ^ (uintptr_t)impl);

    /* ---- Step 1: query portmapper for VXI-11 Core port ---- */
    uint16_t core_port = 0;
    ViStatus st = vxi11_getport(impl, timeout, &core_port);
    if (st != VI_SUCCESS) return st;
    impl->core_port = core_port;

    /* ---- Step 2: connect to VXI-11 Core ---- */
    st = vxi11_tcp_connect(impl->host, impl->core_port, timeout, &impl->sock);
    if (st != VI_SUCCESS) return st;

    /* ---- Step 3: create_link ---- */
    uint8_t  params[512];
    uint32_t pn = 0;
    pn += xdr_put_i32(params + pn, 0);           /* clientId (arbitrary) */
    pn += xdr_put_i32(params + pn, 0);           /* lockDevice = false */
    pn += xdr_put_u32(params + pn, 0u);          /* lock_timeout (ms) */
    pn += xdr_put_string(params + pn, impl->device);

    uint8_t  rbuf[256];
    uint32_t roff = 0;
    st = vxi11_call(impl, VXI11_PROC_CREATE_LINK,
                    params, pn,
                    rbuf, sizeof(rbuf), &roff, timeout);
    if (st != VI_SUCCESS) {
        ov_closesocket(impl->sock);
        impl->sock = OV_INVALID_SOCKET;
        return st;
    }

    /* Parse create_link reply: error, lid, abort_port, max_recv_size */
    int32_t  error = 0;
    int32_t  lid   = 0;
    uint32_t abort_port   = 0;
    uint32_t max_recv_sz  = 0;
    uint32_t p = roff;
    p += xdr_get_i32(rbuf + p, &error);
    p += xdr_get_i32(rbuf + p, &lid);
    p += xdr_get_u32(rbuf + p, &abort_port);
    p += xdr_get_u32(rbuf + p, &max_recv_sz);
    (void)abort_port;   /* not used in this implementation */

    if (error != 0) {
        ov_closesocket(impl->sock);
        impl->sock = OV_INVALID_SOCKET;
        return VI_ERROR_CONN_LOST;
    }

    impl->lid          = lid;
    impl->max_recv_size = max_recv_sz ? max_recv_sz : 65536u;

    return VI_SUCCESS;
}

static ViStatus vxi11_close(OvTransport *self)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_SUCCESS;

    /* destroy_link — best-effort, ignore errors */
    uint8_t  params[8];
    uint32_t pn = xdr_put_i32(params, impl->lid);

    uint8_t  rbuf[128];
    uint32_t roff = 0;
    vxi11_call(impl, VXI11_PROC_DESTROY_LINK,
               params, pn,
               rbuf, sizeof(rbuf), &roff, 2000u);

    ov_closesocket(impl->sock);
    impl->sock = OV_INVALID_SOCKET;
    return VI_SUCCESS;
}

/*
 * device_write: may call the RPC multiple times if data exceeds max_recv_size.
 * Sets the END flag only on the last chunk.
 * Note: the write vtable signature carries no timeout, so we use a fixed default.
 */
static ViStatus vxi11_write(OvTransport *self,
                             ViBuf buf, ViUInt32 count,
                             ViUInt32 *retCount)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    const uint32_t io_timeout = VXI11_WRITE_TIMEOUT_MS;
    uint32_t       written    = 0;

    while (written < (uint32_t)count) {
        uint32_t chunk = (uint32_t)count - written;
        if (chunk > impl->max_recv_size)
            chunk = impl->max_recv_size;

        /* Build device_write params */
        uint8_t *params = (uint8_t *)malloc(chunk + 64u);
        if (!params) return VI_ERROR_ALLOC;

        uint32_t pn = 0;
        pn += xdr_put_i32(params + pn, impl->lid);
        pn += xdr_put_u32(params + pn, io_timeout);
        pn += xdr_put_u32(params + pn, 0u);    /* lock_timeout */
        /* END flag on last chunk */
        uint32_t flags = ((written + chunk) >= (uint32_t)count)
                         ? VXI11_FLAG_END : 0u;
        pn += xdr_put_u32(params + pn, flags);
        pn += xdr_put_opaque(params + pn,
                             (const uint8_t *)buf + written, chunk);

        uint8_t  rbuf[128];
        uint32_t roff = 0;
        ViStatus st = vxi11_call(impl, VXI11_PROC_DEVICE_WRITE,
                                 params, pn,
                                 rbuf, sizeof(rbuf), &roff,
                                 io_timeout + 2000u);
        free(params);
        if (st != VI_SUCCESS) return st;

        int32_t  error = 0;
        uint32_t size  = 0;
        uint32_t p = roff;
        p += xdr_get_i32(rbuf + p, &error);
        p += xdr_get_u32(rbuf + p, &size);

        if (error != 0) return VI_ERROR_IO;
        written += size;
        /* Guard against zero-byte progress (device bug) */
        if (size == 0) break;
    }

    if (retCount) *retCount = written;
    return VI_SUCCESS;
}

/*
 * device_read: reads up to min(count, max_recv_size) bytes per call.
 * Loops while the device indicates more data is available (no END, no
 * REQCNT / CHR reason) up to the caller's buffer limit.
 */
static ViStatus vxi11_read(OvTransport *self,
                            ViBuf buf, ViUInt32 count,
                            ViUInt32 *retCount,
                            ViUInt32 timeout)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    uint32_t total        = 0;
    uint32_t buf_size     = impl->max_recv_size + 512u;
    uint8_t *rbuf         = (uint8_t *)malloc(buf_size);
    if (!rbuf) return VI_ERROR_ALLOC;

    ViStatus final_st = VI_SUCCESS;
    int      done     = 0;

    while (!done && total < (uint32_t)count) {
        uint32_t request_size = (uint32_t)count - total;
        if (request_size > impl->max_recv_size)
            request_size = impl->max_recv_size;

        /* Build device_read params */
        uint8_t  params[32];
        uint32_t pn = 0;
        pn += xdr_put_i32(params + pn, impl->lid);
        pn += xdr_put_u32(params + pn, request_size);
        pn += xdr_put_u32(params + pn, timeout);
        pn += xdr_put_u32(params + pn, 0u);     /* lock_timeout */
        pn += xdr_put_u32(params + pn, 0u);     /* flags */
        pn += xdr_put_i32(params + pn, 0);      /* term_char (not used) */

        uint32_t roff = 0;
        ViStatus st = vxi11_call(impl, VXI11_PROC_DEVICE_READ,
                                 params, pn,
                                 rbuf, buf_size, &roff,
                                 timeout + 2000u);
        if (st != VI_SUCCESS) {
            free(rbuf);
            return st;
        }

        int32_t  error  = 0;
        uint32_t reason = 0;
        uint32_t p = roff;
        p += xdr_get_i32(rbuf + p, &error);
        p += xdr_get_u32(rbuf + p, &reason);

        if (error != 0) {
            free(rbuf);
            return VI_ERROR_IO;
        }

        uint32_t data_len = 0;
        xdr_get_opaque(rbuf + p,
                       (uint8_t *)buf + total,
                       (uint32_t)count - total,
                       &data_len);
        total += data_len;

        /* Stop reading when device signals end-of-message */
        if (reason & (VXI11_REASON_END | VXI11_REASON_REQCNT | VXI11_REASON_CHR)) {
            if (reason & (VXI11_REASON_END | VXI11_REASON_CHR))
                final_st = VI_SUCCESS_TERM_CHAR;
            done = 1;
        }
        /* Also stop if device sent less than we asked for */
        if (data_len < request_size) done = 1;
    }

    free(rbuf);
    if (retCount) *retCount = total;
    return final_st;
}

/*
 * device_readstb: reads the serial poll byte from the device.
 * Equivalent to IEEE 488 serial poll (SPE/SPD sequence).
 */
static ViStatus vxi11_readSTB(OvTransport *self, ViUInt16 *status)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    uint8_t  params[32];
    uint32_t pn = 0;
    pn += xdr_put_i32(params + pn, impl->lid);
    pn += xdr_put_u32(params + pn, 0u);        /* flags */
    pn += xdr_put_u32(params + pn, 0u);        /* lock_timeout */
    pn += xdr_put_u32(params + pn, 5000u);     /* io_timeout */

    uint8_t  rbuf[128];
    uint32_t roff = 0;
    ViStatus st = vxi11_call(impl, VXI11_PROC_DEVICE_READSTB,
                             params, pn,
                             rbuf, sizeof(rbuf), &roff, 7000u);
    if (st != VI_SUCCESS) return st;

    int32_t  error = 0;
    uint32_t stb   = 0;
    uint32_t p = roff;
    p += xdr_get_i32(rbuf + p, &error);
    p += xdr_get_u32(rbuf + p, &stb);

    if (error != 0) return VI_ERROR_IO;
    if (status) *status = (ViUInt16)(stb & 0xFFu);
    return VI_SUCCESS;
}

/*
 * device_clear: sends the Selected Device Clear (SDC) command.
 * Equivalent to IEEE 488 SDC / GPIB DCL.
 */
static ViStatus vxi11_clear(OvTransport *self)
{
    Vxi11Impl *impl = (Vxi11Impl *)self->impl;
    if (impl->sock == OV_INVALID_SOCKET) return VI_ERROR_CONN_LOST;

    uint8_t  params[32];
    uint32_t pn = 0;
    pn += xdr_put_i32(params + pn, impl->lid);
    pn += xdr_put_u32(params + pn, 0u);        /* flags */
    pn += xdr_put_u32(params + pn, 0u);        /* lock_timeout */
    pn += xdr_put_u32(params + pn, 5000u);     /* io_timeout */

    uint8_t  rbuf[128];
    uint32_t roff = 0;
    ViStatus st = vxi11_call(impl, VXI11_PROC_DEVICE_CLEAR,
                             params, pn,
                             rbuf, sizeof(rbuf), &roff, 7000u);
    if (st != VI_SUCCESS) return st;

    int32_t error = 0;
    xdr_get_i32(rbuf + roff, &error);
    if (error != 0) return VI_ERROR_IO;

    return VI_SUCCESS;
}

/* ========== Factory ========== */

OvTransport *ov_transport_tcpip_vxi11_create(void)
{
    OvTransport *t = (OvTransport *)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    Vxi11Impl *impl = (Vxi11Impl *)calloc(1, sizeof(Vxi11Impl));
    if (!impl) { free(t); return NULL; }

    impl->sock          = OV_INVALID_SOCKET;
    impl->lid           = -1;
    impl->max_recv_size = 65536u;

    t->impl    = impl;
    t->open    = vxi11_open;
    t->close   = vxi11_close;
    t->read    = vxi11_read;
    t->write   = vxi11_write;
    t->readSTB = vxi11_readSTB;
    t->clear   = vxi11_clear;

    return t;
}
