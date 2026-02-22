/*
 * OpenVISA - Session management and Resource Manager
 */

#include "session.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ========== Global State ========== */

static OvState g_state = { .initialized = false, .nextHandle = 1 };

OvState* ov_state_get(void) {
    return &g_state;
}

OvSession* ov_session_alloc(void) {
    OvState *s = &g_state;
    for (int i = 0; i < OV_MAX_SESSIONS; i++) {
        if (!s->sessions[i].active) {
            memset(&s->sessions[i], 0, sizeof(OvSession));
            s->sessions[i].active = true;
            s->sessions[i].handle = s->nextHandle++;
            s->sessions[i].timeout = 2000;          /* 2s default */
            s->sessions[i].termChar = '\n';
            s->sessions[i].termCharEn = false;
            s->sessions[i].sendEndEn = true;
            return &s->sessions[i];
        }
    }
    return NULL;
}

OvSession* ov_session_find(ViSession handle) {
    OvState *s = &g_state;
    for (int i = 0; i < OV_MAX_SESSIONS; i++) {
        if (s->sessions[i].active && s->sessions[i].handle == handle)
            return &s->sessions[i];
    }
    return NULL;
}

void ov_session_free(OvSession *sess) {
    if (sess) {
        if (sess->transport) {
            if (sess->transport->close)
                sess->transport->close(sess->transport);
            free(sess->transport);
        }
        memset(sess, 0, sizeof(OvSession));
    }
}

OvFindList* ov_findlist_alloc(void) {
    OvState *s = &g_state;
    for (int i = 0; i < OV_MAX_FIND_LISTS; i++) {
        if (!s->findLists[i].active) {
            memset(&s->findLists[i], 0, sizeof(OvFindList));
            s->findLists[i].active = true;
            s->findLists[i].handle = s->nextHandle++;
            return &s->findLists[i];
        }
    }
    return NULL;
}

OvFindList* ov_findlist_find(ViFindList handle) {
    OvState *s = &g_state;
    for (int i = 0; i < OV_MAX_FIND_LISTS; i++) {
        if (s->findLists[i].active && s->findLists[i].handle == handle)
            return &s->findLists[i];
    }
    return NULL;
}

void ov_findlist_free(OvFindList *fl) {
    if (fl) memset(fl, 0, sizeof(OvFindList));
}

/* ========== Resource String Parser ========== */

/* Helper: case-insensitive prefix match */
static bool starts_with_ci(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix))
            return false;
        str++;
        prefix++;
    }
    return true;
}

ViStatus ov_parse_rsrc(const char *rsrcName, OvResource *rsrc) {
    memset(rsrc, 0, sizeof(OvResource));
    strncpy(rsrc->raw, rsrcName, sizeof(rsrc->raw) - 1);
    rsrc->gpibSecAddr = -1;

    /* TCPIP[board]::host[::port]::INSTR
     * TCPIP[board]::host::port::SOCKET
     * TCPIP[board]::host[::device_name]::INSTR
     * TCPIP[board]::host::hislip0[::INSTR] */
    if (starts_with_ci(rsrcName, "TCPIP")) {
        rsrc->intfType = OV_INTF_TCPIP;
        const char *p = rsrcName + 5;

        /* optional board number */
        rsrc->intfNum = 0;
        if (*p >= '0' && *p <= '9') {
            rsrc->intfNum = (ViUInt16)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }

        if (strncmp(p, "::", 2) != 0)
            return VI_ERROR_INV_RSRC_NAME;
        p += 2;

        /* host (IP or hostname) */
        char *dst = rsrc->host;
        while (*p && strncmp(p, "::", 2) != 0) {
            *dst++ = *p++;
        }
        *dst = '\0';

        if (*p == '\0') {
            /* TCPIP::host — assume INSTR on VXI-11 port */
            strcpy(rsrc->deviceName, "inst0");
            rsrc->port = 111; /* VXI-11 portmapper */
            return VI_SUCCESS;
        }
        p += 2; /* skip :: */

        /* Next field: could be port, device_name, INSTR, or SOCKET */
        if (starts_with_ci(p, "INSTR")) {
            strcpy(rsrc->deviceName, "inst0");
            rsrc->port = 111;
            return VI_SUCCESS;
        }

        if (starts_with_ci(p, "hislip")) {
            rsrc->isHiSLIP = true;
            rsrc->port = 4880;
            dst = rsrc->deviceName;
            while (*p && strncmp(p, "::", 2) != 0) {
                *dst++ = *p++;
            }
            *dst = '\0';
            return VI_SUCCESS;
        }

        /* Could be numeric port (for SOCKET) or device name */
        char field[256] = {0};
        dst = field;
        while (*p && strncmp(p, "::", 2) != 0) {
            *dst++ = *p++;
        }
        *dst = '\0';

        if (*p == '\0' || starts_with_ci(p + 2, "INSTR")) {
            /* device name like "inst0" */
            strcpy(rsrc->deviceName, field);
            rsrc->port = 111;
            return VI_SUCCESS;
        }
        p += 2; /* skip :: */

        if (starts_with_ci(p, "SOCKET")) {
            rsrc->isSocket = true;
            rsrc->port = (ViUInt16)atoi(field);
            return VI_SUCCESS;
        }

        /* port::INSTR */
        rsrc->port = (ViUInt16)atoi(field);
        strcpy(rsrc->deviceName, "inst0");
        return VI_SUCCESS;
    }

    /* USB[board]::manfID::modelCode::serialNum[::intfNum]::INSTR */
    if (starts_with_ci(rsrcName, "USB")) {
        rsrc->intfType = OV_INTF_USB;
        const char *p = rsrcName + 3;

        rsrc->intfNum = 0;
        if (*p >= '0' && *p <= '9') {
            rsrc->intfNum = (ViUInt16)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }

        if (strncmp(p, "::", 2) != 0)
            return VI_ERROR_INV_RSRC_NAME;
        p += 2;

        /* VID */
        rsrc->usbVid = (ViUInt16)strtol(p, NULL, 0);
        while (*p && strncmp(p, "::", 2) != 0) p++;
        if (*p) p += 2;

        /* PID */
        rsrc->usbPid = (ViUInt16)strtol(p, NULL, 0);
        while (*p && strncmp(p, "::", 2) != 0) p++;
        if (*p) p += 2;

        /* Serial */
        char *dst = rsrc->usbSerial;
        while (*p && strncmp(p, "::", 2) != 0) {
            *dst++ = *p++;
        }
        *dst = '\0';
        /* skip ::INSTR or ::intfNum::INSTR */

        return VI_SUCCESS;
    }

    /* ASRL[board]::INSTR  (serial) */
    if (starts_with_ci(rsrcName, "ASRL")) {
        rsrc->intfType = OV_INTF_ASRL;
        const char *p = rsrcName + 4;
        rsrc->comPort = atoi(p);
        return VI_SUCCESS;
    }

    /* GPIB[board]::primary_addr[::secondary_addr]::INSTR */
    if (starts_with_ci(rsrcName, "GPIB")) {
        rsrc->intfType = OV_INTF_GPIB;
        const char *p = rsrcName + 4;

        rsrc->intfNum = 0;
        if (*p >= '0' && *p <= '9') {
            rsrc->intfNum = (ViUInt16)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }

        if (strncmp(p, "::", 2) != 0)
            return VI_ERROR_INV_RSRC_NAME;
        p += 2;

        rsrc->gpibAddr = atoi(p);
        while (*p >= '0' && *p <= '9') p++;

        if (strncmp(p, "::", 2) == 0) {
            p += 2;
            if (!starts_with_ci(p, "INSTR")) {
                rsrc->gpibSecAddr = atoi(p);
            }
        }
        return VI_SUCCESS;
    }

    return VI_ERROR_INV_RSRC_NAME;
}

/* ========== VISA API Implementation ========== */

ViStatus _VI_FUNC viOpenDefaultRM(ViSession *vi) {
    if (!vi) return VI_ERROR_INV_OBJECT;

    OvState *s = &g_state;
    if (!s->initialized) {
        memset(s, 0, sizeof(OvState));
        s->nextHandle = 1;
        s->initialized = true;
    }

    OvSession *sess = ov_session_alloc();
    if (!sess) return VI_ERROR_ALLOC;

    sess->isRM = true;
    *vi = sess->handle;
    return VI_SUCCESS;
}

ViStatus _VI_FUNC viOpen(
    ViSession sesn, ViRsrc rsrcName,
    ViAccessMode accessMode, ViUInt32 openTimeout,
    ViSession *vi)
{
    if (!vi || !rsrcName) return VI_ERROR_INV_OBJECT;

    OvSession *rm = ov_session_find(sesn);
    if (!rm || !rm->isRM) return VI_ERROR_INV_OBJECT;

    /* Parse resource string */
    OvResource rsrc;
    ViStatus st = ov_parse_rsrc(rsrcName, &rsrc);
    if (st != VI_SUCCESS) return st;

    /* Create session */
    OvSession *sess = ov_session_alloc();
    if (!sess) return VI_ERROR_ALLOC;

    sess->resource = rsrc;

    /* Create transport */
    sess->transport = ov_transport_create(rsrc.intfType);
    if (!sess->transport) {
        ov_session_free(sess);
        return VI_ERROR_RSRC_NFOUND;
    }

    /* Open transport */
    ViUInt32 tmo = (openTimeout == VI_NULL) ? 5000 : openTimeout;
    st = sess->transport->open(sess->transport, &rsrc, tmo);
    if (st != VI_SUCCESS) {
        ov_session_free(sess);
        return st;
    }

    *vi = sess->handle;
    return VI_SUCCESS;
}

ViStatus _VI_FUNC viClose(ViObject vi) {
    OvSession *sess = ov_session_find(vi);
    if (sess) {
        ov_session_free(sess);
        return VI_SUCCESS;
    }

    OvFindList *fl = ov_findlist_find(vi);
    if (fl) {
        ov_findlist_free(fl);
        return VI_SUCCESS;
    }

    return VI_ERROR_INV_OBJECT;
}

ViStatus _VI_FUNC viRead(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViUInt32 *retCount)
{
    OvSession *sess = ov_session_find(vi);
    if (!sess || !sess->transport || !sess->transport->read)
        return VI_ERROR_INV_OBJECT;

    return sess->transport->read(sess->transport, buf, count, retCount, sess->timeout);
}

ViStatus _VI_FUNC viWrite(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViUInt32 *retCount)
{
    OvSession *sess = ov_session_find(vi);
    if (!sess || !sess->transport || !sess->transport->write)
        return VI_ERROR_INV_OBJECT;

    return sess->transport->write(sess->transport, buf, count, retCount);
}

ViStatus _VI_FUNC viReadSTB(ViSession vi, ViUInt16 *status) {
    OvSession *sess = ov_session_find(vi);
    if (!sess || !sess->transport || !sess->transport->readSTB)
        return VI_ERROR_INV_OBJECT;

    return sess->transport->readSTB(sess->transport, status);
}

ViStatus _VI_FUNC viClear(ViSession vi) {
    OvSession *sess = ov_session_find(vi);
    if (!sess || !sess->transport || !sess->transport->clear)
        return VI_ERROR_INV_OBJECT;

    return sess->transport->clear(sess->transport);
}

ViStatus _VI_FUNC viGetAttribute(
    ViSession vi, ViAttr attribute, void *attrState)
{
    OvSession *sess = ov_session_find(vi);
    if (!sess) return VI_ERROR_INV_OBJECT;
    if (!attrState) return VI_ERROR_INV_OBJECT;

    switch (attribute) {
        case VI_ATTR_TMO_VALUE:
            *(ViUInt32*)attrState = sess->timeout;
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR:
            *(ViUInt8*)attrState = sess->termChar;
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR_EN:
            *(ViBoolean*)attrState = sess->termCharEn ? VI_TRUE : VI_FALSE;
            return VI_SUCCESS;
        case VI_ATTR_SEND_END_EN:
            *(ViBoolean*)attrState = sess->sendEndEn ? VI_TRUE : VI_FALSE;
            return VI_SUCCESS;
        case VI_ATTR_RSRC_NAME:
            strcpy((char*)attrState, sess->resource.raw);
            return VI_SUCCESS;
        case VI_ATTR_INTF_TYPE:
            *(ViUInt16*)attrState = (ViUInt16)sess->resource.intfType;
            return VI_SUCCESS;
        case VI_ATTR_INTF_NUM:
            *(ViUInt16*)attrState = sess->resource.intfNum;
            return VI_SUCCESS;
        case VI_ATTR_RSRC_MANF_NAME:
            strcpy((char*)attrState, "OpenVISA");
            return VI_SUCCESS;
        case VI_ATTR_RSRC_IMPL_VERSION:
            *(ViUInt32*)attrState = 0x00010000; /* 1.0.0 */
            return VI_SUCCESS;
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

ViStatus _VI_FUNC viSetAttribute(
    ViSession vi, ViAttr attribute, ViAttrState attrState)
{
    OvSession *sess = ov_session_find(vi);
    if (!sess) return VI_ERROR_INV_OBJECT;

    switch (attribute) {
        case VI_ATTR_TMO_VALUE:
            sess->timeout = (ViUInt32)attrState;
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR:
            sess->termChar = (ViChar)(attrState & 0xFF);
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR_EN:
            sess->termCharEn = (attrState != 0);
            return VI_SUCCESS;
        case VI_ATTR_SEND_END_EN:
            sess->sendEndEn = (attrState != 0);
            return VI_SUCCESS;
        default:
            return VI_ERROR_NSUP_ATTR;
    }
}

ViStatus _VI_FUNC viStatusDesc(
    ViSession vi, ViStatus status, ViChar desc[])
{
    if (!desc) return VI_ERROR_INV_OBJECT;

    switch (status) {
        case VI_SUCCESS:             strcpy(desc, "Operation completed successfully."); break;
        case VI_SUCCESS_TERM_CHAR:   strcpy(desc, "Read terminated by termination character."); break;
        case VI_SUCCESS_MAX_CNT:     strcpy(desc, "Read terminated by max count."); break;
        case VI_ERROR_SYSTEM_ERROR:  strcpy(desc, "Unknown system error."); break;
        case VI_ERROR_INV_OBJECT:    strcpy(desc, "Invalid session or object reference."); break;
        case VI_ERROR_RSRC_LOCKED:   strcpy(desc, "Resource is locked."); break;
        case VI_ERROR_INV_EXPR:      strcpy(desc, "Invalid expression for resource search."); break;
        case VI_ERROR_RSRC_NFOUND:   strcpy(desc, "Resource not found."); break;
        case VI_ERROR_INV_RSRC_NAME: strcpy(desc, "Invalid resource name."); break;
        case VI_ERROR_TMO:           strcpy(desc, "Timeout expired."); break;
        case VI_ERROR_IO:            strcpy(desc, "I/O error."); break;
        case VI_ERROR_CONN_LOST:     strcpy(desc, "Connection lost."); break;
        case VI_ERROR_ALLOC:         strcpy(desc, "Insufficient resources."); break;
        case VI_ERROR_NSUP_ATTR:     strcpy(desc, "Attribute not supported."); break;
        case VI_ERROR_NSUP_OPER:     strcpy(desc, "Operation not supported."); break;
        default: snprintf(desc, 256, "Unknown status code: 0x%08X", (unsigned int)status); break;
    }
    return VI_SUCCESS;
}

/* ========== Stubs for less common functions ========== */

ViStatus _VI_FUNC viParseRsrc(
    ViSession sesn, ViRsrc rsrcName,
    ViUInt16 *intfType, ViUInt16 *intfNum)
{
    OvResource rsrc;
    ViStatus st = ov_parse_rsrc(rsrcName, &rsrc);
    if (st != VI_SUCCESS) return st;
    if (intfType) *intfType = (ViUInt16)rsrc.intfType;
    if (intfNum) *intfNum = rsrc.intfNum;
    return VI_SUCCESS;
}

ViStatus _VI_FUNC viFindRsrc(
    ViSession sesn, ViString expr,
    ViFindList *findList, ViUInt32 *retcnt, ViChar desc[])
{
    /* TODO: Implement discovery (mDNS, USB enumeration, serial scan) */
    if (retcnt) *retcnt = 0;
    return VI_ERROR_RSRC_NFOUND;
}

ViStatus _VI_FUNC viFindNext(ViFindList findList, ViChar desc[]) {
    OvFindList *fl = ov_findlist_find(findList);
    if (!fl) return VI_ERROR_INV_OBJECT;
    if (fl->current >= fl->count) return VI_ERROR_RSRC_NFOUND;
    strcpy(desc, fl->descriptors[fl->current++]);
    return VI_SUCCESS;
}

/* Formatted I/O - basic implementation */
ViStatus _VI_FUNCH viPrintf(ViSession vi, ViString writeFmt, ...) {
    /* Simple: format and write */
    char buf[OV_BUF_SIZE];
    va_list args;
    va_start(args, writeFmt);
    int len = vsnprintf(buf, sizeof(buf), writeFmt, args);
    va_end(args);

    if (len < 0) return VI_ERROR_INV_FMT;

    ViUInt32 retCount;
    return viWrite(vi, (ViBuf)buf, (ViUInt32)len, &retCount);
}

ViStatus _VI_FUNCH viQueryf(ViSession vi, ViString writeFmt, ViString readFmt, ...) {
    /* Write the query */
    ViUInt32 retCount;
    ViStatus st = viWrite(vi, (ViBuf)writeFmt, (ViUInt32)strlen(writeFmt), &retCount);
    if (st != VI_SUCCESS) return st;

    /* Read response into user-provided buffer */
    va_list args;
    va_start(args, readFmt);
    /* Simple case: readFmt = "%s" or "%t" → read into next arg */
    if (strcmp(readFmt, "%s") == 0 || strcmp(readFmt, "%t") == 0 ||
        strcmp(readFmt, "%256s") == 0) {
        char *dest = va_arg(args, char*);
        st = viRead(vi, (ViBuf)dest, OV_BUF_SIZE - 1, &retCount);
        if (st == VI_SUCCESS || st == VI_SUCCESS_TERM_CHAR || st == VI_SUCCESS_MAX_CNT)
            dest[retCount] = '\0';
    }
    va_end(args);
    return st;
}

/* Event stubs */
ViStatus _VI_FUNC viEnableEvent(ViSession vi, ViEventType eventType, ViUInt16 mechanism, ViEventFilter context) {
    return VI_SUCCESS; /* stub */
}
ViStatus _VI_FUNC viDisableEvent(ViSession vi, ViEventType eventType, ViUInt16 mechanism) {
    return VI_SUCCESS;
}
ViStatus _VI_FUNC viDiscardEvents(ViSession vi, ViEventType eventType, ViUInt16 mechanism) {
    return VI_SUCCESS;
}
ViStatus _VI_FUNC viWaitOnEvent(ViSession vi, ViEventType inEventType, ViUInt32 timeout, ViEventType *outEventType, ViEvent *outContext) {
    return VI_ERROR_TMO;
}
ViStatus _VI_FUNC viLock(ViSession vi, ViAccessMode lockType, ViUInt32 timeout, ViKeyId requestedKey, ViChar accessKey[]) {
    return VI_SUCCESS; /* stub - no locking yet */
}
ViStatus _VI_FUNC viUnlock(ViSession vi) {
    return VI_SUCCESS;
}
ViStatus _VI_FUNC viTerminate(ViSession vi, ViUInt16 degree, ViJobId jobId) {
    return VI_SUCCESS;
}
