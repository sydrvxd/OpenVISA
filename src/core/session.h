/*
 * OpenVISA - Internal session management
 */

#ifndef OPENVISA_SESSION_H
#define OPENVISA_SESSION_H

#include "visa.h"
#include <stdbool.h>

/* Maximum concurrent sessions */
#define OV_MAX_SESSIONS     256
#define OV_MAX_FIND_LISTS   32
#define OV_DESC_SIZE        256
#define OV_BUF_SIZE         65536

/* Interface type for resource string parsing */
typedef enum {
    OV_INTF_TCPIP = VI_INTF_TCPIP,
    OV_INTF_USB   = VI_INTF_USB,
    OV_INTF_ASRL  = VI_INTF_ASRL,
    OV_INTF_GPIB  = VI_INTF_GPIB,
} OvIntfType;

/* Parsed resource descriptor */
typedef struct {
    OvIntfType  intfType;
    ViUInt16    intfNum;            /* board number (usually 0) */
    char        host[256];          /* TCPIP: hostname/IP */
    ViUInt16    port;               /* TCPIP: port (VXI-11=111, HiSLIP=4880, raw=5025) */
    char        deviceName[256];    /* TCPIP: LAN device name (inst0, hislip0, ...) */
    ViUInt16    usbVid;             /* USB: vendor ID */
    ViUInt16    usbPid;             /* USB: product ID */
    char        usbSerial[128];     /* USB: serial number */
    ViUInt16    usbIntfNum;         /* USB: interface number */
    int         comPort;            /* ASRL: COM port number */
    int         gpibAddr;           /* GPIB: primary address */
    int         gpibSecAddr;        /* GPIB: secondary address (-1 = none) */
    bool        isSocket;           /* TCPIP::host::port::SOCKET */
    bool        isHiSLIP;           /* TCPIP: hislip protocol */
    char        raw[512];           /* original resource string */
} OvResource;

/* Transport operations vtable */
typedef struct OvTransport {
    ViStatus (*open)(struct OvTransport *self, const OvResource *rsrc, ViUInt32 timeout);
    ViStatus (*close)(struct OvTransport *self);
    ViStatus (*read)(struct OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount, ViUInt32 timeout);
    ViStatus (*write)(struct OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount);
    ViStatus (*readSTB)(struct OvTransport *self, ViUInt16 *status);
    ViStatus (*clear)(struct OvTransport *self);
    void *impl;     /* transport-specific data */
} OvTransport;

/* Session object */
typedef struct {
    bool        active;
    bool        isRM;               /* true if this is the Resource Manager session */
    ViSession   handle;
    OvResource  resource;
    OvTransport *transport;
    /* Attributes */
    ViUInt32    timeout;            /* VI_ATTR_TMO_VALUE */
    ViChar      termChar;           /* VI_ATTR_TERMCHAR */
    bool        termCharEn;         /* VI_ATTR_TERMCHAR_EN */
    bool        sendEndEn;          /* VI_ATTR_SEND_END_EN */
} OvSession;

/* Find list for viFindRsrc */
typedef struct {
    bool        active;
    ViFindList  handle;
    char        descriptors[128][OV_DESC_SIZE];
    ViUInt32    count;
    ViUInt32    current;
} OvFindList;

/* Global state */
typedef struct {
    bool        initialized;
    OvSession   sessions[OV_MAX_SESSIONS];
    OvFindList  findLists[OV_MAX_FIND_LISTS];
    ViUInt32    nextHandle;
} OvState;

/* Internal functions */
OvState*    ov_state_get(void);
OvSession*  ov_session_alloc(void);
OvSession*  ov_session_find(ViSession handle);
void        ov_session_free(OvSession *sess);
OvFindList* ov_findlist_alloc(void);
OvFindList* ov_findlist_find(ViFindList handle);
void        ov_findlist_free(OvFindList *fl);

/* Resource string parser */
ViStatus    ov_parse_rsrc(const char *rsrcName, OvResource *rsrc);

/* Transport factory */
OvTransport* ov_transport_create(OvIntfType type);

#endif /* OPENVISA_SESSION_H */
