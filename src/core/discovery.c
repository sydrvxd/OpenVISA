/*
 * OpenVISA - Resource Discovery  (viFindRsrc / ov_discover)
 *
 * Discovery strategies implemented:
 *  1. mDNS/DNS-SD  — multicast query for _lxi._tcp.local + _hislip._tcp.local
 *                    → TCPIP resource strings
 *  2. USB          — libusb enumeration of USBTMC devices (Class 0xFE / Subclass 0x03)
 *                    → USB resource strings
 *  3. Serial       — /dev/ttyS*, /dev/ttyUSB*, /dev/ttyACM* (Linux)
 *                     or HKLM\HARDWARE\DEVICEMAP\SERIALCOMM (Windows)
 *                    → ASRL resource strings
 *
 * Export:
 *   ViStatus ov_discover(ViString expr, OvFindList *fl)
 *   ViStatus viFindRsrc(ViSession rm, ViString expr, ViFindList *fl,
 *                       ViUInt32 *cnt, ViChar desc[])
 */

#include "session.h"
#include "visa.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

/* ========== Platform includes ========== */

#ifdef OPENVISA_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "Iphlpapi.lib")
    typedef SOCKET ov_sock_t;
    #define OV_SOCK_INVALID  INVALID_SOCKET
    #define ov_sock_close    closesocket
    #define ov_sock_errno()  WSAGetLastError()
    typedef int socklen_t_w;
    #define socklen_t socklen_t_w
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <dirent.h>
    #include <dlfcn.h>
    typedef int ov_sock_t;
    #define OV_SOCK_INVALID  (-1)
    #define ov_sock_close    close
    #define ov_sock_errno()  errno
#endif

/* ========== OvFindList helpers ========== */

static bool fl_add(OvFindList *fl, const char *rsrc) {
    if (fl->count >= 128) return false;

    /* Avoid duplicates */
    for (ViUInt32 i = 0; i < fl->count; i++) {
        if (strcmp(fl->descriptors[i], rsrc) == 0)
            return false;
    }

    strncpy(fl->descriptors[fl->count], rsrc, OV_DESC_SIZE - 1);
    fl->descriptors[fl->count][OV_DESC_SIZE - 1] = '\0';
    fl->count++;
    return true;
}

/* Simple glob/wildcard match: supports * and ? */
static bool glob_match(const char *pattern, const char *str) {
    /* Case-insensitive where sensible for VISA */
    const char *p = pattern, *s = str;
    const char *star_p = NULL, *star_s = NULL;

    while (*s) {
        if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
            p++; s++;
        } else if (*p == '*') {
            star_p = p++;
            star_s = s;
        } else if (star_p) {
            p = star_p + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* ========== mDNS / DNS-SD Discovery ========== */

/*
 * Minimal mDNS query sender + DNS response parser.
 * We send a PTR query for each service type and collect
 * PTR → SRV → A records from the multicast responses.
 *
 * DNS wire-format basics:
 *   Header: 12 bytes
 *   Question: QNAME (labels) + QTYPE (2) + QCLASS (2)
 *   Answer / Additional RRs: NAME (2) + TYPE (2) + CLASS (2) + TTL (4) + RDLENGTH (2) + RDATA
 */

#define MDNS_ADDR   "224.0.0.251"
#define MDNS_PORT   5353
#define MDNS_BUFSIZE 4096
#define MDNS_TIMEOUT_MS 2500   /* listen window per service */

/* DNS record types */
#define DNS_TYPE_A    1
#define DNS_TYPE_PTR  12
#define DNS_TYPE_SRV  33
#define DNS_TYPE_AAAA 28

/* Build a DNS PTR query packet for `service` (e.g. "_lxi._tcp.local") */
static int mdns_build_query(const char *service, uint8_t *buf, int buflen) {
    if (buflen < 32) return -1;

    /* Header: QR=0, OPCODE=0, AA=0, TC=0, RD=0, QDCOUNT=1 */
    buf[0] = 0x00; buf[1] = 0x00;   /* Transaction ID (0 for mDNS) */
    buf[2] = 0x00; buf[3] = 0x00;   /* Flags */
    buf[4] = 0x00; buf[5] = 0x01;   /* QDCOUNT = 1 */
    buf[6] = 0x00; buf[7] = 0x00;   /* ANCOUNT */
    buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT */
    buf[10]= 0x00; buf[11]= 0x00;   /* ARCOUNT */

    int pos = 12;

    /* Encode QNAME: split `service` by '.' and write length+label */
    char tmp[256];
    strncpy(tmp, service, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *tok = strtok(tmp, ".");
    while (tok && pos < buflen - 5) {
        int len = (int)strlen(tok);
        buf[pos++] = (uint8_t)len;
        memcpy(buf + pos, tok, len);
        pos += len;
        tok = strtok(NULL, ".");
    }
    buf[pos++] = 0x00;   /* root label */

    /* QTYPE = PTR (12), QCLASS = IN (1) with unicast-response bit */
    buf[pos++] = 0x00; buf[pos++] = 0x0C;   /* TYPE PTR */
    buf[pos++] = 0x00; buf[pos++] = 0x01;   /* CLASS IN */

    return pos;
}

/* Parse a DNS name from `buf` at `offset` into `out` (up to `outlen`).
 * Returns the new offset after the name (handles pointers). */
static int dns_parse_name(const uint8_t *buf, int buflen, int offset,
                          char *out, int outlen) {
    int opos = 0;
    int followed_ptr = 0;
    int orig_offset = -1;

    while (offset < buflen) {
        uint8_t len = buf[offset];
        if (len == 0) {
            /* End of name */
            offset++;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            /* Pointer */
            if (offset + 1 >= buflen) return -1;
            int ptr = ((len & 0x3F) << 8) | buf[offset + 1];
            if (!followed_ptr) orig_offset = offset + 2;
            followed_ptr = 1;
            offset = ptr;
            continue;
        }
        /* Label */
        offset++;
        if (opos > 0 && opos < outlen - 1) out[opos++] = '.';
        int copy = len;
        if (opos + copy >= outlen) copy = outlen - opos - 1;
        memcpy(out + opos, buf + offset, copy);
        opos += copy;
        offset += len;
    }
    if (opos < outlen) out[opos] = '\0';
    return followed_ptr ? orig_offset : offset;
}

/* Internal structure to collect SRV / A record information */
typedef struct {
    char instance[256];   /* instance name (PTR target) */
    char host[256];       /* SRV target hostname */
    char ipv4[64];        /* resolved A record IP */
    uint16_t port;
} MdnsRecord;

#define MDNS_MAX_RECORDS 64

typedef struct {
    MdnsRecord records[MDNS_MAX_RECORDS];
    int count;
} MdnsContext;

static MdnsRecord *mdns_find_or_alloc(MdnsContext *ctx, const char *instance) {
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->records[i].instance, instance) == 0)
            return &ctx->records[i];
    }
    if (ctx->count >= MDNS_MAX_RECORDS) return NULL;
    MdnsRecord *r = &ctx->records[ctx->count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->instance, instance, sizeof(r->instance) - 1);
    return r;
}

/* Parse DNS answers/additionals from a mDNS response */
static void mdns_parse_response(const uint8_t *buf, int len, const char *service,
                                MdnsContext *ctx) {
    if (len < 12) return;

    /* Header counts */
    int qdcount = (buf[4] << 8) | buf[5];
    int ancount = (buf[6] << 8) | buf[7];
    int arcount = (buf[10] << 8) | buf[11];
    int total   = ancount + arcount;

    int pos = 12;

    /* Skip questions */
    for (int i = 0; i < qdcount && pos < len; i++) {
        char tmp[256] = {0};
        pos = dns_parse_name(buf, len, pos, tmp, sizeof(tmp));
        if (pos < 0) return;
        pos += 4; /* QTYPE + QCLASS */
    }

    char rname[256], rdata_name[256];

    for (int rr = 0; rr < total && pos < len; rr++) {
        /* NAME */
        int new_pos = dns_parse_name(buf, len, pos, rname, sizeof(rname));
        if (new_pos < 0) break;
        pos = new_pos;

        if (pos + 10 > len) break;
        uint16_t type     = (buf[pos] << 8)   | buf[pos + 1];
        /* uint16_t class_ = (buf[pos+2] << 8) | buf[pos+3]; */
        /* uint32_t ttl   = ... */
        uint16_t rdlen    = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;

        if (pos + rdlen > len) break;

        if (type == DNS_TYPE_PTR) {
            /* PTR: points from service type → instance name */
            int np = dns_parse_name(buf, len, pos, rdata_name, sizeof(rdata_name));
            (void)np;
            /* rdata_name = "<instance>._lxi._tcp.local" */
            /* Strip service suffix to get pure instance name */
            MdnsRecord *rec = mdns_find_or_alloc(ctx, rdata_name);
            (void)rec;
        } else if (type == DNS_TYPE_SRV) {
            /* SRV: priority(2) weight(2) port(2) target */
            if (rdlen >= 7) {
                uint16_t port = (buf[pos + 4] << 8) | buf[pos + 5];
                dns_parse_name(buf, len, pos + 6, rdata_name, sizeof(rdata_name));
                /* Associate SRV with instance */
                MdnsRecord *rec = mdns_find_or_alloc(ctx, rname);
                if (rec) {
                    rec->port = port;
                    strncpy(rec->host, rdata_name, sizeof(rec->host) - 1);
                }
            }
        } else if (type == DNS_TYPE_A) {
            /* A record: 4 bytes IPv4 */
            if (rdlen == 4) {
                /* Store IP, then try to match to a record by hostname */
                char ipstr[64];
                snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                         buf[pos], buf[pos+1], buf[pos+2], buf[pos+3]);
                for (int i = 0; i < ctx->count; i++) {
                    if (strcasecmp(ctx->records[i].host, rname) == 0 ||
                        strcasecmp(ctx->records[i].instance, rname) == 0) {
                        strncpy(ctx->records[i].ipv4, ipstr,
                                sizeof(ctx->records[i].ipv4) - 1);
                    }
                }
                /* Also keep a standalone A entry for later matching */
                MdnsRecord *rec = mdns_find_or_alloc(ctx, rname);
                if (rec && rec->ipv4[0] == '\0')
                    strncpy(rec->ipv4, ipstr, sizeof(rec->ipv4) - 1);
            }
        }

        pos += rdlen;
    }
}

/* Perform mDNS discovery for one service type.
 * Adds TCPIP resource strings to fl. */
static void mdns_discover_service(const char *service, bool isHiSLIP,
                                  OvFindList *fl) {
#ifdef OPENVISA_WINDOWS
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    ov_sock_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == OV_SOCK_INVALID) return;

    /* Allow multiple sockets on port 5353 */
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    /* Bind to MDNS_PORT on INADDR_ANY */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(MDNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        ov_sock_close(sock);
        return;
    }

    /* Join multicast group */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    /* Set TTL=255 for multicast */
    unsigned char ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    /* Disable multicast loopback (we don't want our own query) */
    unsigned char loop = 0;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));

    /* Build query */
    uint8_t qbuf[256];
    int qlen = mdns_build_query(service, qbuf, sizeof(qbuf));
    if (qlen <= 0) { ov_sock_close(sock); return; }

    /* Send query to 224.0.0.251:5353 */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(MDNS_PORT);
    dest.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    sendto(sock, (const char*)qbuf, qlen, 0,
           (struct sockaddr*)&dest, sizeof(dest));

    /* Set receive timeout */
#ifdef OPENVISA_WINDOWS
    DWORD tv_ms = MDNS_TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv;
    tv.tv_sec  = MDNS_TIMEOUT_MS / 1000;
    tv.tv_usec = (MDNS_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    MdnsContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Receive responses */
    uint8_t rbuf[MDNS_BUFSIZE];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    for (;;) {
        int rlen = (int)recvfrom(sock, (char*)rbuf, sizeof(rbuf), 0,
                                 (struct sockaddr*)&from, &fromlen);
        if (rlen <= 0) break;   /* timeout or error */

        mdns_parse_response(rbuf, rlen, service, &ctx);
    }

    /* Drop membership and close */
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
    ov_sock_close(sock);

    /* Convert collected records to VISA resource strings */
    for (int i = 0; i < ctx.count; i++) {
        MdnsRecord *r = &ctx.records[i];
        if (r->ipv4[0] == '\0') continue; /* no IP resolved */

        char rsrc[OV_DESC_SIZE];
        if (isHiSLIP) {
            /* TCPIP0::192.168.1.x::hislip0::INSTR */
            snprintf(rsrc, sizeof(rsrc), "TCPIP0::%s::hislip0::INSTR", r->ipv4);
        } else {
            /* LXI: TCPIP0::192.168.1.x::inst0::INSTR */
            snprintf(rsrc, sizeof(rsrc), "TCPIP0::%s::inst0::INSTR", r->ipv4);
        }
        fl_add(fl, rsrc);

        /* Also add a raw SOCKET variant if we have a port */
        if (r->port > 0 && !isHiSLIP) {
            snprintf(rsrc, sizeof(rsrc), "TCPIP0::%s::%u::SOCKET",
                     r->ipv4, r->port);
            fl_add(fl, rsrc);
        }
    }
}

/* ========== USB / USBTMC Discovery ========== */

/*
 * USBTMC: USB class 0xFE, subclass 0x03.
 * We try to dynamically load libusb-1.0 at runtime so the code builds
 * even without libusb headers.
 */

#ifndef OPENVISA_WINDOWS
/* libusb-1.0 minimal type definitions (enough for our purposes) */
typedef struct libusb_context  libusb_context;
typedef struct libusb_device   libusb_device;

struct libusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

struct libusb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
    const void *endpoint;
    const void *extra;
    int extra_length;
};

struct libusb_interface {
    const struct libusb_interface_descriptor *altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
    const struct libusb_interface *interface;
    const unsigned char *extra;
    int extra_length;
};

typedef int  (*fn_libusb_init)(libusb_context **ctx);
typedef void (*fn_libusb_exit)(libusb_context *ctx);
typedef int  (*fn_libusb_get_device_list)(libusb_context *ctx, libusb_device ***list);
typedef void (*fn_libusb_free_device_list)(libusb_device **list, int unref);
typedef int  (*fn_libusb_get_device_descriptor)(libusb_device *dev,
                  struct libusb_device_descriptor *desc);
typedef int  (*fn_libusb_get_config_descriptor)(libusb_device *dev, uint8_t idx,
                  struct libusb_config_descriptor **config);
typedef void (*fn_libusb_free_config_descriptor)(struct libusb_config_descriptor *cfg);
typedef int  (*fn_libusb_open)(libusb_device *dev, void **handle);
typedef void (*fn_libusb_close)(void *handle);
typedef int  (*fn_libusb_get_string_descriptor_ascii)(void *handle,
                  uint8_t idx, unsigned char *data, int length);

static void usb_discover(OvFindList *fl) {
    void *lib = dlopen("libusb-1.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libusb-1.0.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return;

    fn_libusb_init                     p_init   = dlsym(lib, "libusb_init");
    fn_libusb_exit                     p_exit   = dlsym(lib, "libusb_exit");
    fn_libusb_get_device_list          p_list   = dlsym(lib, "libusb_get_device_list");
    fn_libusb_free_device_list         p_flist  = dlsym(lib, "libusb_free_device_list");
    fn_libusb_get_device_descriptor    p_ddesc  = dlsym(lib, "libusb_get_device_descriptor");
    fn_libusb_get_config_descriptor    p_cdesc  = dlsym(lib, "libusb_get_config_descriptor");
    fn_libusb_free_config_descriptor   p_fcdesc = dlsym(lib, "libusb_free_config_descriptor");
    fn_libusb_open                     p_open   = dlsym(lib, "libusb_open");
    fn_libusb_close                    p_close  = dlsym(lib, "libusb_close");
    fn_libusb_get_string_descriptor_ascii p_str = dlsym(lib, "libusb_get_string_descriptor_ascii");

    if (!p_init || !p_list || !p_ddesc || !p_cdesc) {
        dlclose(lib);
        return;
    }

    libusb_context *ctx = NULL;
    if (p_init(&ctx) != 0) { dlclose(lib); return; }

    libusb_device **devlist = NULL;
    int ndev = (int)p_list(ctx, &devlist);
    if (ndev < 0) goto usb_done;

    for (int i = 0; i < ndev; i++) {
        libusb_device *dev = devlist[i];
        struct libusb_device_descriptor ddesc;
        if (p_ddesc(dev, &ddesc) != 0) continue;

        struct libusb_config_descriptor *cfg = NULL;
        bool is_usbtmc = false;
        uint8_t intf_num = 0;

        /* Check all configurations */
        for (uint8_t ci = 0; ci < ddesc.bNumConfigurations && !is_usbtmc; ci++) {
            if (p_cdesc(dev, ci, &cfg) != 0) continue;
            for (uint8_t ii = 0; ii < cfg->bNumInterfaces && !is_usbtmc; ii++) {
                const struct libusb_interface *ifc = &cfg->interface[ii];
                for (int ai = 0; ai < ifc->num_altsetting && !is_usbtmc; ai++) {
                    const struct libusb_interface_descriptor *alt = &ifc->altsetting[ai];
                    if (alt->bInterfaceClass    == 0xFE &&
                        alt->bInterfaceSubClass == 0x03) {
                        is_usbtmc  = true;
                        intf_num   = alt->bInterfaceNumber;
                    }
                }
            }
            if (cfg) p_fcdesc(cfg);
        }

        if (!is_usbtmc) continue;

        /* Attempt to open to read string descriptors */
        char serial[128] = {0};
        void *handle = NULL;
        if (p_open && p_close && p_str && p_open(dev, &handle) == 0) {
            if (ddesc.iSerialNumber)
                p_str(handle, ddesc.iSerialNumber,
                      (unsigned char*)serial, sizeof(serial) - 1);
            p_close(handle);
        }

        /* Build USB resource string:
         * USB0::0x{VID}::0x{PID}::{serial}::{intf}::INSTR */
        char rsrc[OV_DESC_SIZE];
        if (serial[0]) {
            snprintf(rsrc, sizeof(rsrc),
                     "USB0::0x%04X::0x%04X::%s::%d::INSTR",
                     ddesc.idVendor, ddesc.idProduct, serial, intf_num);
        } else {
            snprintf(rsrc, sizeof(rsrc),
                     "USB0::0x%04X::0x%04X::::%d::INSTR",
                     ddesc.idVendor, ddesc.idProduct, intf_num);
        }
        fl_add(fl, rsrc);
    }

    p_flist(devlist, 1);

usb_done:
    p_exit(ctx);
    dlclose(lib);
}

#else  /* Windows USB discovery */

static void usb_discover(OvFindList *fl) {
    /* On Windows, USBTMC enumeration typically goes through WinUSB or
     * the NI-VISA / Keysight I/O Layer drivers.  Without those, we can
     * attempt libusb-1.0 (if installed via Zadig). */
    (void)fl;
    /* TODO: Windows libusb-1.0 via LoadLibrary("libusb-1.0.dll") — structure
     * mirrors the Linux version above but uses __cdecl calling convention. */
}

#endif  /* OPENVISA_WINDOWS */

/* ========== Serial Port Discovery ========== */

#ifdef OPENVISA_WINDOWS

static void serial_discover(OvFindList *fl) {
    /* Read HKLM\HARDWARE\DEVICEMAP\SERIALCOMM */
    HKEY hKey;
    LONG rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                            "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                            0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) return;

    DWORD idx = 0;
    char valName[256], valData[256];
    DWORD valNameLen, valDataLen, valType;

    while (1) {
        valNameLen = sizeof(valName);
        valDataLen = sizeof(valData);
        rc = RegEnumValueA(hKey, idx++, valName, &valNameLen,
                           NULL, &valType, (LPBYTE)valData, &valDataLen);
        if (rc != ERROR_SUCCESS) break;
        if (valType != REG_SZ) continue;

        /* valData = "COM3", "COM12", etc. */
        int portNum = 0;
        if (sscanf(valData, "COM%d", &portNum) == 1 && portNum > 0) {
            char rsrc[OV_DESC_SIZE];
            snprintf(rsrc, sizeof(rsrc), "ASRL%d::INSTR", portNum);
            fl_add(fl, rsrc);
        }
    }

    RegCloseKey(hKey);
}

#else  /* Linux/macOS serial discovery */

#include <sys/stat.h>

static void serial_discover(OvFindList *fl) {
    DIR *d = opendir("/dev");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Only consider tty* entries */
        if (strncmp(ent->d_name, "tty", 3) != 0) continue;

        /* Check against patterns */
        bool match = false;
        const char *suffix = ent->d_name + 3; /* skip "tty" */
        const char *patterns_suffix[] = {
            "S", "USB", "ACM",
#ifdef __APPLE__
            ".usbserial", ".usbmodem",
#endif
            NULL
        };
        for (int pi = 0; patterns_suffix[pi]; pi++) {
            if (strncmp(suffix, patterns_suffix[pi],
                        strlen(patterns_suffix[pi])) == 0) {
                match = true;
                break;
            }
        }
        if (!match) continue;

        char devpath[128];   /* /dev/ttyXXXn — max ~30 chars in practice */
        /* Use explicit width to avoid -Wformat-truncation (d_name is char[256]) */
        snprintf(devpath, sizeof(devpath), "/dev/%.*s",
                 (int)(sizeof(devpath) - 6), ent->d_name);

        /* Verify the node exists and is a character device */
        struct stat st;
        if (stat(devpath, &st) != 0) continue;
        if (!S_ISCHR(st.st_mode)) continue;

        /*
         * Map /dev/ttyS{n} → ASRL{n+1}::INSTR
         *     /dev/ttyUSB{n} → ASRL{n+1+100}::INSTR  (offset 101..)
         *     /dev/ttyACM{n} → ASRL{n+1+200}::INSTR  (offset 201..)
         *
         * For cross-platform tool compatibility, we use the path form too.
         * We emit: ASRL/dev/ttyUSB0::INSTR (canonical for POSIX)
         */
        char rsrc[OV_DESC_SIZE];
        snprintf(rsrc, sizeof(rsrc), "ASRL%s::INSTR", devpath);
        fl_add(fl, rsrc);

        /* Also emit numeric ASRL{n} for ttyS{n} */
        int n = -1;
        if (strncmp(suffix, "S", 1) == 0 && isdigit((unsigned char)suffix[1])) {
            n = atoi(suffix + 1);
            if (n >= 0) {
                snprintf(rsrc, sizeof(rsrc), "ASRL%d::INSTR", n + 1);
                fl_add(fl, rsrc);
            }
        }
    }

    closedir(d);
}

#endif  /* serial_discover */

/* ========== Main Discovery Entry Point ========== */

/*
 * ov_discover — fill OvFindList with all matching resources.
 *
 * expr: VISA find expression, e.g.
 *   "?*"             — all instruments
 *   "TCPIP?*"        — all TCPIP instruments
 *   "USB?*::INSTR"   — all USB instruments
 *   "ASRL?*::INSTR"  — all serial ports
 *   "GPIB?*::INSTR"  — all GPIB (not enumerable, returns nothing)
 */
ViStatus ov_discover(ViString expr, OvFindList *fl) {
    if (!fl) return VI_ERROR_INV_OBJECT;

    fl->count   = 0;
    fl->current = 0;

    /* Default expression: match everything */
    if (!expr || expr[0] == '\0') expr = "?*";

    bool want_tcpip = glob_match(expr, "TCPIP?*") ||
                      glob_match("TCPIP0::x::inst0::INSTR", expr) ||
                      strchr(expr, '*') != NULL || strchr(expr, '?') != NULL;
    bool want_usb   = glob_match(expr, "USB?*")  || strchr(expr, '*') != NULL ||
                      strchr(expr, '?') != NULL;
    bool want_asrl  = glob_match(expr, "ASRL?*") || strchr(expr, '*') != NULL ||
                      strchr(expr, '?') != NULL;

    /* Broad match: "?*" or "?*::INSTR" matches everything */
    if (strcmp(expr, "?*") == 0 || strcmp(expr, "?*::INSTR") == 0) {
        want_tcpip = want_usb = want_asrl = true;
    }

    if (want_tcpip) {
        mdns_discover_service("_lxi._tcp.local",    false, fl);
        mdns_discover_service("_hislip._tcp.local", true,  fl);
    }

    if (want_usb) {
        usb_discover(fl);
    }

    if (want_asrl) {
        serial_discover(fl);
    }

    /* Filter results against expr */
    ViUInt32 kept = 0;
    for (ViUInt32 i = 0; i < fl->count; i++) {
        if (glob_match(expr, fl->descriptors[i])) {
            if (i != kept)
                memcpy(fl->descriptors[kept], fl->descriptors[i], OV_DESC_SIZE);
            kept++;
        }
    }
    fl->count = kept;

    return (fl->count > 0) ? VI_SUCCESS : VI_ERROR_RSRC_NFOUND;
}

/* ========== viFindRsrc implementation ========== */

ViStatus _VI_FUNC viFindRsrc(ViSession rm, ViString expr,
                             ViFindList *findList, ViUInt32 *retcnt, ViChar desc[]) {
    /* Validate resource manager */
    OvState *state = ov_state_get();
    if (!state || !state->initialized) return VI_ERROR_SYSTEM_ERROR;

    OvSession *rmSess = ov_session_find(rm);
    if (!rmSess || !rmSess->isRM) return VI_ERROR_INV_OBJECT;

    /* Allocate a find list */
    OvFindList *fl = ov_findlist_alloc();
    if (!fl) return VI_ERROR_ALLOC;

    /* Run discovery */
    ViStatus st = ov_discover(expr, fl);
    if (st != VI_SUCCESS) {
        ov_findlist_free(fl);
        return st;
    }

    /* Return handle, count, first descriptor */
    if (findList) *findList = fl->handle;
    if (retcnt)   *retcnt   = fl->count;
    if (desc && fl->count > 0) {
        strncpy(desc, fl->descriptors[fl->current], OV_DESC_SIZE - 1);
        desc[OV_DESC_SIZE - 1] = '\0';
        fl->current++;
    }

    return VI_SUCCESS;
}

/* ========== viFindNext implementation ========== */

ViStatus _VI_FUNC viFindNext(ViFindList fl_handle, ViChar desc[]) {
    OvFindList *fl = ov_findlist_find(fl_handle);
    if (!fl) return VI_ERROR_INV_OBJECT;

    if (fl->current >= fl->count)
        return VI_ERROR_RSRC_NFOUND;

    if (desc) {
        strncpy(desc, fl->descriptors[fl->current], OV_DESC_SIZE - 1);
        desc[OV_DESC_SIZE - 1] = '\0';
    }
    fl->current++;

    return VI_SUCCESS;
}
