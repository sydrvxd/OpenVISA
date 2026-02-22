/*
 * OpenVISA - USBTMC Transport (USB Test & Measurement Class)
 *
 * USB Class 0xFE, Subclass 0x03
 * Bulk-OUT for commands (DEV_DEP_MSG_OUT),
 * Bulk-IN  for responses (REQUEST_DEV_DEP_MSG_IN → DEV_DEP_MSG_IN)
 * Control transfers for readSTB (USB488) and clear (INITIATE_CLEAR).
 *
 * Requires libusb-1.0. When not available, all functions return
 * VI_ERROR_NSUP_OPER (stub mode).
 */

#include "../core/session.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* =========================================================================
 * USBTMC / USB488 constants
 * ========================================================================= */

/* USBTMC MsgID values */
#define USBTMC_MSGID_DEV_DEP_MSG_OUT            1
#define USBTMC_MSGID_REQUEST_DEV_DEP_MSG_IN     2
#define USBTMC_MSGID_DEV_DEP_MSG_IN             2
#define USBTMC_MSGID_VENDOR_SPECIFIC_OUT        126
#define USBTMC_MSGID_REQUEST_VENDOR_SPECIFIC_IN 127
#define USBTMC_MSGID_VENDOR_SPECIFIC_IN         127

/* bmTransferAttributes flags */
#define USBTMC_TRANSFER_EOM         0x01  /* DEV_DEP_MSG_OUT: End-of-Message */
#define USBTMC_TRANSFER_TERMCHAREN  0x02  /* REQUEST_DEV_DEP_MSG_IN: TermChar enabled */

/* USBTMC class-specific control request codes */
#define USBTMC_REQ_INITIATE_ABORT_BULK_OUT      1
#define USBTMC_REQ_CHECK_ABORT_BULK_OUT_STATUS  2
#define USBTMC_REQ_INITIATE_ABORT_BULK_IN       3
#define USBTMC_REQ_CHECK_ABORT_BULK_IN_STATUS   4
#define USBTMC_REQ_INITIATE_CLEAR               5
#define USBTMC_REQ_CHECK_CLEAR_STATUS           6
#define USBTMC_REQ_GET_CAPABILITIES             7
#define USBTMC_REQ_INDICATOR_PULSE              64

/* USB488 class-specific control request codes */
#define USB488_REQ_READ_STATUS_BYTE             128  /* 0x80 */

/* USBTMC status codes returned in control response byte 0 */
#define USBTMC_STATUS_SUCCESS                   0x01
#define USBTMC_STATUS_PENDING                   0x02
#define USBTMC_STATUS_FAILED                    0x80
#define USBTMC_STATUS_TRANSFER_NOT_IN_PROGRESS  0x81
#define USBTMC_STATUS_SPLIT_NOT_IN_PROGRESS     0x82
#define USBTMC_STATUS_SPLIT_IN_PROGRESS         0x83

/* bmRequestType values */
#define USBTMC_REQTYPE_CLASS_INTF_H2D  0x21   /* Class | Interface | Host-to-Device */
#define USBTMC_REQTYPE_CLASS_INTF_D2H  0xA1   /* Class | Interface | Device-to-Host */

/* Bulk transfer header size (fixed 12 bytes) */
#define USBTMC_HEADER_SIZE   12

/* Maximum time (ms) to poll CHECK_CLEAR_STATUS */
#define USBTMC_CLEAR_TIMEOUT_MS  5000
#define USBTMC_CLEAR_POLL_MS      200

/* Default bulk transfer timeout (ms) used when caller provides 0 */
#define USBTMC_DEFAULT_TIMEOUT_MS 5000


/* =========================================================================
 * libusb integration — everything below is inside #ifdef HAVE_LIBUSB
 * ========================================================================= */

#ifdef HAVE_LIBUSB

#include <libusb-1.0/libusb.h>

/* -------------------------------------------------------------------------
 * USBTMC bulk message header (12 bytes, little-endian)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t  MsgID;
    uint8_t  bTag;
    uint8_t  bTagInverse;
    uint8_t  reserved1;          /* must be 0x00 */
    uint8_t  TransferSize[4];    /* LE uint32 */
    uint8_t  bmTransferAttributes;
    uint8_t  TermChar;
    uint8_t  reserved2[2];       /* must be 0x00 */
} UsbtmcHeader;

/* Helper: encode / decode little-endian 32-bit */
static void usbtmc_put32le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

static uint32_t usbtmc_get32le(const uint8_t *src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

/* -------------------------------------------------------------------------
 * Per-session implementation data
 * ------------------------------------------------------------------------- */
typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *dev;

    uint8_t  intf_num;       /* claimed USBTMC interface number */
    uint8_t  ep_bulk_out;    /* Bulk-OUT endpoint address */
    uint8_t  ep_bulk_in;     /* Bulk-IN  endpoint address */

    uint8_t  bTag;           /* current tag: 1–255, wraps (never 0) */

    /* capabilities (from GET_CAPABILITIES response) */
    uint8_t  usb488_if;      /* non-zero if USB488 subclass supported */
    uint8_t  ren_control;    /* USB488: REN_CONTROL supported */
    uint8_t  trigger;        /* USB488: TRIGGER supported */
    uint8_t  read_stb_cap;   /* USB488: READ_STATUS_BYTE supported */
} UsbtmcImpl;

/* -------------------------------------------------------------------------
 * bTag management
 * ------------------------------------------------------------------------- */
static uint8_t usbtmc_next_tag(UsbtmcImpl *impl) {
    impl->bTag++;
    if (impl->bTag == 0)
        impl->bTag = 1;   /* skip 0 — reserved */
    return impl->bTag;
}

/* -------------------------------------------------------------------------
 * Build a USBTMC bulk message header into a 12-byte buffer
 * ------------------------------------------------------------------------- */
static void usbtmc_build_header(UsbtmcHeader *hdr,
                                uint8_t msgid,
                                uint8_t tag,
                                uint32_t transfer_size,
                                uint8_t attributes,
                                uint8_t term_char)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->MsgID              = msgid;
    hdr->bTag               = tag;
    hdr->bTagInverse        = (uint8_t)(~tag);
    hdr->reserved1          = 0x00;
    usbtmc_put32le(hdr->TransferSize, transfer_size);
    hdr->bmTransferAttributes = attributes;
    hdr->TermChar           = term_char;
    /* reserved2 already zeroed by memset */
}

/* -------------------------------------------------------------------------
 * Device enumeration helpers
 * ------------------------------------------------------------------------- */

/* Return non-zero if the interface descriptor matches USBTMC class */
static int is_usbtmc_interface(const struct libusb_interface_descriptor *altsetting) {
    return (altsetting->bInterfaceClass    == 0xFE &&
            altsetting->bInterfaceSubClass == 0x03);
}

/* Scan endpoints of a USBTMC interface and fill ep_bulk_out / ep_bulk_in */
static int find_bulk_endpoints(const struct libusb_interface_descriptor *altsetting,
                               uint8_t *ep_out, uint8_t *ep_in)
{
    *ep_out = 0;
    *ep_in  = 0;

    for (int i = 0; i < altsetting->bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor *ep = &altsetting->endpoint[i];
        uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        if (type != LIBUSB_TRANSFER_TYPE_BULK)
            continue;

        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
            *ep_out = ep->bEndpointAddress;
        else
            *ep_in  = ep->bEndpointAddress;
    }

    return (*ep_out != 0 && *ep_in != 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * GET_CAPABILITIES control transfer (optional, best-effort)
 * ------------------------------------------------------------------------- */
static void usbtmc_get_capabilities(UsbtmcImpl *impl) {
    uint8_t buf[24] = {0};
    int rc = libusb_control_transfer(
        impl->dev,
        USBTMC_REQTYPE_CLASS_INTF_D2H,          /* bmRequestType */
        USBTMC_REQ_GET_CAPABILITIES,             /* bRequest */
        0,                                       /* wValue */
        impl->intf_num,                          /* wIndex */
        buf, sizeof(buf),
        2000);

    if (rc < 4) return;  /* ignore failures */

    /* buf[0] = USBTMC_STATUS, buf[1] = reserved, buf[2..3] = bcdUSBTMC
     * For USB488: buf[4] = interface capabilities, buf[5] = device capabilities */
    if (rc >= 6) {
        impl->usb488_if    = buf[4] & 0x04;  /* bit 2: is USB488 interface */
        impl->ren_control  = buf[4] & 0x02;
        impl->read_stb_cap = buf[5] & 0x04;
        impl->trigger      = buf[5] & 0x01;
    }
}

/* =========================================================================
 * Transport Operations
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * open — find USB device by VID/PID/serial, claim USBTMC interface
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;
    (void)timeout;  /* libusb enumeration has no meaningful timeout concept */

    int rc = libusb_init(&impl->ctx);
    if (rc < 0) return VI_ERROR_SYSTEM_ERROR;

    /* Enumerate all devices */
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(impl->ctx, &list);
    if (count < 0) {
        libusb_exit(impl->ctx);
        impl->ctx = NULL;
        return VI_ERROR_SYSTEM_ERROR;
    }

    libusb_device_handle *found = NULL;
    uint8_t found_intf  = 0;
    uint8_t found_epout = 0;
    uint8_t found_epin  = 0;

    for (ssize_t i = 0; i < count && !found; i++) {
        libusb_device *dev = list[i];
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(dev, &desc) < 0)
            continue;

        /* Match VID / PID */
        if (desc.idVendor  != rsrc->usbVid ||
            desc.idProduct != rsrc->usbPid)
            continue;

        /* Open to read serial if required */
        libusb_device_handle *h = NULL;
        if (libusb_open(dev, &h) < 0)
            continue;

        /* Match serial number (if specified) */
        if (rsrc->usbSerial[0] != '\0') {
            char serial_buf[256] = {0};
            if (desc.iSerialNumber) {
                libusb_get_string_descriptor_ascii(
                    h, desc.iSerialNumber,
                    (unsigned char *)serial_buf, sizeof(serial_buf));
            }
            if (strcmp(serial_buf, rsrc->usbSerial) != 0) {
                libusb_close(h);
                continue;
            }
        }

        /* Scan configurations for a USBTMC interface */
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
            libusb_close(h);
            continue;
        }

        int intf_found = 0;
        for (int ci = 0; ci < cfg->bNumInterfaces && !intf_found; ci++) {
            for (int ai = 0; ai < cfg->interface[ci].num_altsetting && !intf_found; ai++) {
                const struct libusb_interface_descriptor *alt =
                    &cfg->interface[ci].altsetting[ai];

                if (!is_usbtmc_interface(alt))
                    continue;

                /* If caller specified an interface number, honour it */
                if (rsrc->usbIntfNum != 0 &&
                    alt->bInterfaceNumber != rsrc->usbIntfNum)
                    continue;

                uint8_t epout = 0, epin = 0;
                if (find_bulk_endpoints(alt, &epout, &epin) < 0)
                    continue;

                found_intf  = alt->bInterfaceNumber;
                found_epout = epout;
                found_epin  = epin;
                intf_found  = 1;
            }
        }
        libusb_free_config_descriptor(cfg);

        if (!intf_found) {
            libusb_close(h);
            continue;
        }

        found = h;
    }

    libusb_free_device_list(list, 1);

    if (!found) {
        libusb_exit(impl->ctx);
        impl->ctx = NULL;
        return VI_ERROR_RSRC_NFOUND;
    }

    impl->dev       = found;
    impl->intf_num  = found_intf;
    impl->ep_bulk_out = found_epout;
    impl->ep_bulk_in  = found_epin;
    impl->bTag      = 0;   /* first call to usbtmc_next_tag() yields 1 */

    /* Detach kernel driver if active (Linux) */
    if (libusb_kernel_driver_active(impl->dev, impl->intf_num) == 1)
        libusb_detach_kernel_driver(impl->dev, impl->intf_num);

    rc = libusb_claim_interface(impl->dev, impl->intf_num);
    if (rc < 0) {
        libusb_close(impl->dev);
        impl->dev = NULL;
        libusb_exit(impl->ctx);
        impl->ctx = NULL;
        return VI_ERROR_RSRC_LOCKED;
    }

    /* Read capabilities (best-effort, ignore error) */
    usbtmc_get_capabilities(impl);

    return VI_SUCCESS;
}

/* -------------------------------------------------------------------------
 * close
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_close(OvTransport *self) {
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;

    if (impl->dev) {
        libusb_release_interface(impl->dev, impl->intf_num);
        libusb_close(impl->dev);
        impl->dev = NULL;
    }
    if (impl->ctx) {
        libusb_exit(impl->ctx);
        impl->ctx = NULL;
    }
    return VI_SUCCESS;
}

/* -------------------------------------------------------------------------
 * write — DEV_DEP_MSG_OUT Bulk-OUT transfer
 *
 * Payload must be padded to a 4-byte boundary per the USBTMC spec.
 * We allocate a single buffer: [12-byte header][payload][0-3 pad bytes].
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_write(OvTransport *self,
                             ViBuf buf, ViUInt32 count, ViUInt32 *retCount)
{
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;
    if (!impl->dev) return VI_ERROR_CONN_LOST;

    /* Calculate padded payload length (must be multiple of 4) */
    uint32_t padded = (count + 3) & ~3u;

    /* Allocate: header (12) + padded payload */
    size_t total = USBTMC_HEADER_SIZE + padded;
    uint8_t *pkt = (uint8_t *)calloc(1, total);
    if (!pkt) return VI_ERROR_ALLOC;

    uint8_t tag = usbtmc_next_tag(impl);

    UsbtmcHeader *hdr = (UsbtmcHeader *)pkt;
    usbtmc_build_header(hdr,
                        USBTMC_MSGID_DEV_DEP_MSG_OUT,
                        tag,
                        count,                      /* TransferSize = actual count */
                        USBTMC_TRANSFER_EOM,        /* EOM set — single transfer */
                        0x00);                      /* TermChar unused for OUT */

    /* Copy payload right after header */
    memcpy(pkt + USBTMC_HEADER_SIZE, buf, count);
    /* Padding bytes already 0 from calloc */

    int transferred = 0;
    int rc = libusb_bulk_transfer(
        impl->dev,
        impl->ep_bulk_out,
        pkt, (int)total,
        &transferred,
        USBTMC_DEFAULT_TIMEOUT_MS);

    free(pkt);

    if (rc < 0) return VI_ERROR_IO;

    /* Report bytes of original payload written (minus header overhead) */
    if (retCount) {
        uint32_t payload_sent = (uint32_t)transferred > USBTMC_HEADER_SIZE
            ? (uint32_t)transferred - USBTMC_HEADER_SIZE
            : 0;
        /* Clamp to actual requested count */
        *retCount = payload_sent < count ? payload_sent : count;
    }

    return VI_SUCCESS;
}

/* -------------------------------------------------------------------------
 * read — REQUEST_DEV_DEP_MSG_IN (Bulk-OUT) → DEV_DEP_MSG_IN (Bulk-IN)
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_read(OvTransport *self,
                            ViBuf buf, ViUInt32 count,
                            ViUInt32 *retCount, ViUInt32 timeout)
{
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;
    if (!impl->dev) return VI_ERROR_CONN_LOST;

    uint32_t tmo = (timeout == 0) ? USBTMC_DEFAULT_TIMEOUT_MS : timeout;

    /* ---- Step 1: send REQUEST_DEV_DEP_MSG_IN (Bulk-OUT, 12-byte header) ---- */
    uint8_t req_hdr[USBTMC_HEADER_SIZE];
    uint8_t tag = usbtmc_next_tag(impl);

    UsbtmcHeader *hdr = (UsbtmcHeader *)req_hdr;
    usbtmc_build_header(hdr,
                        USBTMC_MSGID_REQUEST_DEV_DEP_MSG_IN,
                        tag,
                        count,                      /* max bytes device may return */
                        0x00,                       /* no TermChar for now */
                        0x00);

    int transferred = 0;
    int rc = libusb_bulk_transfer(
        impl->dev,
        impl->ep_bulk_out,
        req_hdr, USBTMC_HEADER_SIZE,
        &transferred,
        (unsigned int)tmo);

    if (rc < 0) return VI_ERROR_IO;

    /* ---- Step 2: receive DEV_DEP_MSG_IN (Bulk-IN, header + payload) ---- */
    /* Allocate buffer large enough for header + requested data */
    size_t recv_max = USBTMC_HEADER_SIZE + count;
    uint8_t *recv_buf = (uint8_t *)malloc(recv_max);
    if (!recv_buf) return VI_ERROR_ALLOC;

    int recv_len = 0;
    rc = libusb_bulk_transfer(
        impl->dev,
        impl->ep_bulk_in,
        recv_buf, (int)recv_max,
        &recv_len,
        (unsigned int)tmo);

    if (rc < 0 && rc != LIBUSB_ERROR_OVERFLOW) {
        free(recv_buf);
        if (rc == LIBUSB_ERROR_TIMEOUT) return VI_ERROR_TMO;
        return VI_ERROR_IO;
    }

    /* Parse response header */
    ViStatus st = VI_SUCCESS;
    if (recv_len < USBTMC_HEADER_SIZE) {
        free(recv_buf);
        return VI_ERROR_IO;
    }

    UsbtmcHeader *resp_hdr = (UsbtmcHeader *)recv_buf;

    /* Sanity checks */
    if (resp_hdr->MsgID   != USBTMC_MSGID_DEV_DEP_MSG_IN ||
        resp_hdr->bTag    != tag ||
        resp_hdr->bTagInverse != (uint8_t)(~tag))
    {
        free(recv_buf);
        return VI_ERROR_IO;
    }

    uint32_t data_len = usbtmc_get32le(resp_hdr->TransferSize);
    bool eom = (resp_hdr->bmTransferAttributes & USBTMC_TRANSFER_EOM) != 0;

    /* Copy payload to caller buffer */
    int payload_offset = USBTMC_HEADER_SIZE;
    int available = recv_len - payload_offset;
    if (available < 0) available = 0;

    uint32_t copy_len = (uint32_t)available < count ? (uint32_t)available : count;
    if (data_len < copy_len) copy_len = data_len;   /* respect device's TransferSize */

    memcpy(buf, recv_buf + payload_offset, copy_len);
    free(recv_buf);

    if (retCount) *retCount = copy_len;

    /* Determine return status */
    if (eom)
        st = VI_SUCCESS_TERM_CHAR;   /* EOM is the natural end-of-message indicator */
    else
        st = VI_SUCCESS;

    return st;
}

/* -------------------------------------------------------------------------
 * readSTB — USB488 READ_STATUS_BYTE control transfer
 *
 * Control request:
 *   bmRequestType = 0xA1  (Class | Interface | D2H)
 *   bRequest      = 128   (READ_STATUS_BYTE)
 *   wValue        = bTag  (current tag)
 *   wIndex        = interface number
 *   Data          = [USBTMC_status, STB] (2 bytes)
 *
 * Falls back to sending *STB? if device doesn't advertise USB488.
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_readSTB(OvTransport *self, ViUInt16 *status) {
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;
    if (!impl->dev) return VI_ERROR_CONN_LOST;

    uint8_t tag = usbtmc_next_tag(impl);
    uint8_t resp[3] = {0};

    int rc = libusb_control_transfer(
        impl->dev,
        USBTMC_REQTYPE_CLASS_INTF_D2H,    /* bmRequestType */
        USB488_REQ_READ_STATUS_BYTE,       /* bRequest      */
        (uint16_t)tag,                     /* wValue = bTag  */
        (uint16_t)impl->intf_num,          /* wIndex         */
        resp, sizeof(resp),
        2000);

    if (rc < 0) return VI_ERROR_IO;

    /* resp[0] = USBTMC_STATUS
     * resp[1] = bTag (echo)
     * resp[2] = STB  (status byte)  -- some devices use 2-byte response [status, STB]
     *                                   others [bTag, STB] in a 2-byte form
     */
    if (resp[0] != USBTMC_STATUS_SUCCESS)
        return VI_ERROR_IO;

    /* The USB488 spec (Table 11) returns 3 bytes:
     *   byte 0: USBTMC_STATUS
     *   byte 1: bTag
     *   byte 2: STB
     * But rc==2 means only [USBTMC_STATUS, STB] — handle both cases.
     */
    if (rc >= 3) {
        if (status) *status = resp[2];
    } else if (rc == 2) {
        if (status) *status = resp[1];
    } else {
        return VI_ERROR_IO;
    }

    return VI_SUCCESS;
}

/* -------------------------------------------------------------------------
 * clear — INITIATE_CLEAR + poll CHECK_CLEAR_STATUS
 * ------------------------------------------------------------------------- */
static ViStatus usbtmc_clear(OvTransport *self) {
    UsbtmcImpl *impl = (UsbtmcImpl *)self->impl;
    if (!impl->dev) return VI_ERROR_CONN_LOST;

    /* INITIATE_CLEAR */
    uint8_t status_byte[2] = {0};
    int rc = libusb_control_transfer(
        impl->dev,
        USBTMC_REQTYPE_CLASS_INTF_H2D,    /* bmRequestType */
        USBTMC_REQ_INITIATE_CLEAR,         /* bRequest      */
        0,                                 /* wValue        */
        (uint16_t)impl->intf_num,          /* wIndex        */
        NULL, 0,
        2000);

    if (rc < 0) return VI_ERROR_IO;

    /* Poll CHECK_CLEAR_STATUS until not PENDING or timeout */
    int elapsed_ms = 0;
    while (elapsed_ms < USBTMC_CLEAR_TIMEOUT_MS) {
        uint8_t resp[2] = {0};
        rc = libusb_control_transfer(
            impl->dev,
            USBTMC_REQTYPE_CLASS_INTF_D2H, /* bmRequestType */
            USBTMC_REQ_CHECK_CLEAR_STATUS,  /* bRequest      */
            0,                              /* wValue        */
            (uint16_t)impl->intf_num,       /* wIndex        */
            resp, sizeof(resp),
            2000);

        if (rc < 1) return VI_ERROR_IO;

        uint8_t st = resp[0];

        if (st == USBTMC_STATUS_SUCCESS)
            break;

        if (st != USBTMC_STATUS_PENDING)
            return VI_ERROR_IO;

        /* bmClear bit 0 = 1: must do a read on Bulk-IN to clear it */
        if (rc >= 2 && (resp[1] & 0x01)) {
            uint8_t discard[512];
            int dummy = 0;
            libusb_bulk_transfer(impl->dev, impl->ep_bulk_in,
                                 discard, sizeof(discard), &dummy, 500);
        }

        /* Simple sleep via libusb event handling */
        struct timeval tv = { 0, USBTMC_CLEAR_POLL_MS * 1000 };
        libusb_handle_events_timeout(impl->ctx, &tv);
        elapsed_ms += USBTMC_CLEAR_POLL_MS;
    }

    if (elapsed_ms >= USBTMC_CLEAR_TIMEOUT_MS)
        return VI_ERROR_TMO;

    /* Flush any remaining Bulk-IN data */
    uint8_t flush[512];
    int dummy = 0;
    libusb_bulk_transfer(impl->dev, impl->ep_bulk_in,
                         flush, sizeof(flush), &dummy, 200);

    (void)status_byte;  /* suppress unused warning */
    return VI_SUCCESS;
}

/* =========================================================================
 * Factory — libusb available
 * ========================================================================= */

OvTransport* ov_transport_usbtmc_create(void) {
    OvTransport *t = (OvTransport *)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    UsbtmcImpl *impl = (UsbtmcImpl *)calloc(1, sizeof(UsbtmcImpl));
    if (!impl) { free(t); return NULL; }

    t->impl    = impl;
    t->open    = usbtmc_open;
    t->close   = usbtmc_close;
    t->read    = usbtmc_read;
    t->write   = usbtmc_write;
    t->readSTB = usbtmc_readSTB;
    t->clear   = usbtmc_clear;

    return t;
}

#else  /* HAVE_LIBUSB not defined — stub implementation */

/* =========================================================================
 * Stub operations — return VI_ERROR_NSUP_OPER for every call
 * ========================================================================= */

static ViStatus usbtmc_stub_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    (void)self; (void)rsrc; (void)timeout;
    return VI_ERROR_NSUP_OPER;
}

static ViStatus usbtmc_stub_close(OvTransport *self) {
    (void)self;
    return VI_ERROR_NSUP_OPER;
}

static ViStatus usbtmc_stub_write(OvTransport *self,
                                  ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    (void)self; (void)buf; (void)count; (void)retCount;
    return VI_ERROR_NSUP_OPER;
}

static ViStatus usbtmc_stub_read(OvTransport *self,
                                 ViBuf buf, ViUInt32 count,
                                 ViUInt32 *retCount, ViUInt32 timeout) {
    (void)self; (void)buf; (void)count; (void)retCount; (void)timeout;
    return VI_ERROR_NSUP_OPER;
}

static ViStatus usbtmc_stub_readSTB(OvTransport *self, ViUInt16 *status) {
    (void)self; (void)status;
    return VI_ERROR_NSUP_OPER;
}

static ViStatus usbtmc_stub_clear(OvTransport *self) {
    (void)self;
    return VI_ERROR_NSUP_OPER;
}

/* =========================================================================
 * Factory — libusb NOT available
 * ========================================================================= */

OvTransport* ov_transport_usbtmc_create(void) {
    OvTransport *t = (OvTransport *)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    /* No impl data needed for stubs */
    t->impl    = NULL;
    t->open    = usbtmc_stub_open;
    t->close   = usbtmc_stub_close;
    t->read    = usbtmc_stub_read;
    t->write   = usbtmc_stub_write;
    t->readSTB = usbtmc_stub_readSTB;
    t->clear   = usbtmc_stub_clear;

    return t;
}

#endif  /* HAVE_LIBUSB */
