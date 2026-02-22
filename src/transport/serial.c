/*
 * OpenVISA - Serial (ASRL) Transport
 *
 * Handles ASRL{n}::INSTR resource strings.
 * Maps to COMn on Windows, /dev/ttyS{n-1} or /dev/ttyUSB{n-1} on Linux/macOS.
 *
 * Defaults: 9600 baud, 8N1, no flow control, 2000 ms read timeout.
 */

#include "../core/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

/* ========== Platform includes ========== */

#ifdef OPENVISA_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>

    typedef HANDLE ov_serial_t;
    #define OV_INVALID_SERIAL  INVALID_HANDLE_VALUE

#else  /* POSIX */
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <termios.h>
    #include <sys/select.h>
    #include <sys/time.h>

    typedef int ov_serial_t;
    #define OV_INVALID_SERIAL  (-1)
#endif

/* ========== Serial parameters ========== */

typedef struct {
    ov_serial_t fd;
    char        devPath[64];   /* e.g. "COM3" or "/dev/ttyUSB0" */
    ViUInt32    baud;
    ViUInt8     dataBits;      /* 5-8 */
    ViUInt8     stopBits;      /* 10 = 1, 15 = 1.5, 20 = 2  (×10 to avoid float) */
    ViUInt8     parity;        /* 0=none, 1=odd, 2=even, 3=mark, 4=space */
    ViUInt8     flowControl;   /* 0=none, 1=XON/XOFF, 2=RTS/CTS */
} SerialImpl;

/* ========== Port name resolution ========== */

/*
 * ASRL{n}::INSTR → port number n
 * Windows  → "COM{n}"
 * Linux    → "/dev/ttyS{n-1}"   (n=1 → ttyS0)
 * macOS    → "/dev/tty.serial{n-1}"  (best-effort)
 *
 * The caller may also override devPath before open().
 */
static void serial_build_path(int portNum, char *out, size_t outLen) {
#ifdef OPENVISA_WINDOWS
    /* Windows: COM ports > 9 require the \\.\COMn form */
    if (portNum > 9)
        snprintf(out, outLen, "\\\\.\\COM%d", portNum);
    else
        snprintf(out, outLen, "COM%d", portNum);
#elif defined(__APPLE__)
    snprintf(out, outLen, "/dev/tty.serial%d", portNum - 1);
#else  /* Linux */
    snprintf(out, outLen, "/dev/ttyS%d", portNum - 1);
#endif
}

/* ========== Windows implementation ========== */

#ifdef OPENVISA_WINDOWS

static ViStatus serial_platform_open(SerialImpl *impl, ViUInt32 timeout_ms) {
    impl->fd = CreateFileA(
        impl->devPath,
        GENERIC_READ | GENERIC_WRITE,
        0,          /* exclusive */
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (impl->fd == INVALID_HANDLE_VALUE)
        return VI_ERROR_RSRC_NFOUND;

    /* Set queue sizes */
    SetupComm(impl->fd, 4096, 4096);

    /* Flush */
    PurgeComm(impl->fd, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    /* Build DCB */
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(impl->fd, &dcb)) {
        CloseHandle(impl->fd);
        impl->fd = INVALID_HANDLE_VALUE;
        return VI_ERROR_SYSTEM_ERROR;
    }

    dcb.BaudRate = impl->baud;
    dcb.ByteSize = impl->dataBits;

    switch (impl->parity) {
        case 1:  dcb.Parity = ODDPARITY;   dcb.fParity = TRUE; break;
        case 2:  dcb.Parity = EVENPARITY;  dcb.fParity = TRUE; break;
        case 3:  dcb.Parity = MARKPARITY;  dcb.fParity = TRUE; break;
        case 4:  dcb.Parity = SPACEPARITY; dcb.fParity = TRUE; break;
        default: dcb.Parity = NOPARITY;    dcb.fParity = FALSE; break;
    }

    switch (impl->stopBits) {
        case 15: dcb.StopBits = ONE5STOPBITS; break;
        case 20: dcb.StopBits = TWOSTOPBITS;  break;
        default: dcb.StopBits = ONESTOPBIT;   break;
    }

    dcb.fBinary = TRUE;

    switch (impl->flowControl) {
        case 1: /* XON/XOFF */
            dcb.fOutX = TRUE; dcb.fInX = TRUE;
            dcb.fRtsControl = RTS_CONTROL_DISABLE;
            dcb.fOutxCtsFlow = FALSE;
            break;
        case 2: /* RTS/CTS */
            dcb.fOutX = FALSE; dcb.fInX = FALSE;
            dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
            dcb.fOutxCtsFlow = TRUE;
            break;
        default: /* none */
            dcb.fOutX = FALSE; dcb.fInX = FALSE;
            dcb.fRtsControl = RTS_CONTROL_DISABLE;
            dcb.fOutxCtsFlow = FALSE;
            break;
    }

    if (!SetCommState(impl->fd, &dcb)) {
        CloseHandle(impl->fd);
        impl->fd = INVALID_HANDLE_VALUE;
        return VI_ERROR_SYSTEM_ERROR;
    }

    /* Timeouts */
    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout         = MAXDWORD;      /* return immediately if no data */
    timeouts.ReadTotalTimeoutMultiplier  = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant    = timeout_ms;    /* wait up to timeout_ms */
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = timeout_ms;

    SetCommTimeouts(impl->fd, &timeouts);

    return VI_SUCCESS;
}

static void serial_platform_close(SerialImpl *impl) {
    if (impl->fd != INVALID_HANDLE_VALUE) {
        CloseHandle(impl->fd);
        impl->fd = INVALID_HANDLE_VALUE;
    }
}

static ViStatus serial_platform_write(SerialImpl *impl, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    DWORD written = 0;
    BOOL ok = WriteFile(impl->fd, buf, (DWORD)count, &written, NULL);
    if (!ok) return VI_ERROR_IO;
    if (retCount) *retCount = (ViUInt32)written;
    return VI_SUCCESS;
}

static ViStatus serial_platform_read(SerialImpl *impl, ViBuf buf, ViUInt32 count,
                                     ViUInt32 *retCount, ViUInt32 timeout_ms) {
    /* Update timeout for this read */
    COMMTIMEOUTS timeouts;
    timeouts.ReadIntervalTimeout         = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier  = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant    = timeout_ms;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = timeout_ms;
    SetCommTimeouts(impl->fd, &timeouts);

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(impl->fd, buf, (DWORD)count, &bytesRead, NULL);
    if (!ok) return VI_ERROR_IO;
    if (bytesRead == 0) return VI_ERROR_TMO;

    if (retCount) *retCount = (ViUInt32)bytesRead;

    /* Detect termchar (newline) */
    if (bytesRead > 0 && ((char*)buf)[bytesRead - 1] == '\n')
        return VI_SUCCESS_TERM_CHAR;

    return VI_SUCCESS;
}

#else  /* ========== POSIX implementation ========== */

static speed_t baud_to_speed(ViUInt32 baud) {
    switch (baud) {
        case 50:      return B50;
        case 75:      return B75;
        case 110:     return B110;
        case 134:     return B134;
        case 150:     return B150;
        case 200:     return B200;
        case 300:     return B300;
        case 600:     return B600;
        case 1200:    return B1200;
        case 1800:    return B1800;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
#ifdef B460800
        case 460800:  return B460800;
#endif
#ifdef B921600
        case 921600:  return B921600;
#endif
        default:      return B9600;
    }
}

static ViStatus serial_platform_open(SerialImpl *impl, ViUInt32 timeout_ms) {
    impl->fd = open(impl->devPath, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (impl->fd < 0)
        return VI_ERROR_RSRC_NFOUND;

    /* Restore blocking mode */
    int flags = fcntl(impl->fd, F_GETFL, 0);
    fcntl(impl->fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio;
    if (tcgetattr(impl->fd, &tio) < 0) {
        close(impl->fd);
        impl->fd = -1;
        return VI_ERROR_SYSTEM_ERROR;
    }

    /* Raw mode */
    cfmakeraw(&tio);

    /* Baud rate */
    speed_t spd = baud_to_speed(impl->baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    /* Data bits */
    tio.c_cflag &= ~CSIZE;
    switch (impl->dataBits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }

    /* Stop bits */
    if (impl->stopBits == 20)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    /* Parity */
    switch (impl->parity) {
        case 1: /* odd */
            tio.c_cflag |= (PARENB | PARODD);
            tio.c_iflag |= INPCK;
            break;
        case 2: /* even */
            tio.c_cflag |= PARENB;
            tio.c_cflag &= ~PARODD;
            tio.c_iflag |= INPCK;
            break;
        default: /* none */
            tio.c_cflag &= ~PARENB;
            tio.c_iflag &= ~INPCK;
            break;
    }

    /* Flow control */
    switch (impl->flowControl) {
        case 1: /* XON/XOFF */
            tio.c_iflag |= (IXON | IXOFF);
            tio.c_cflag &= ~CRTSCTS;
            break;
        case 2: /* RTS/CTS */
            tio.c_cflag |= CRTSCTS;
            tio.c_iflag &= ~(IXON | IXOFF);
            break;
        default: /* none */
            tio.c_cflag &= ~CRTSCTS;
            tio.c_iflag &= ~(IXON | IXOFF);
            break;
    }

    tio.c_cflag |= CREAD | CLOCAL;

    /*
     * VMIN=0, VTIME=0 → non-blocking (we use select for timeout).
     * Actual timeout is handled in serial_platform_read() via select().
     */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(impl->fd, TCSANOW, &tio) < 0) {
        close(impl->fd);
        impl->fd = -1;
        return VI_ERROR_SYSTEM_ERROR;
    }

    /* Flush */
    tcflush(impl->fd, TCIOFLUSH);

    (void)timeout_ms;
    return VI_SUCCESS;
}

static void serial_platform_close(SerialImpl *impl) {
    if (impl->fd >= 0) {
        close(impl->fd);
        impl->fd = -1;
    }
}

static ViStatus serial_platform_write(SerialImpl *impl, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    ssize_t written = write(impl->fd, buf, count);
    if (written < 0) return VI_ERROR_IO;
    if (retCount) *retCount = (ViUInt32)written;
    return VI_SUCCESS;
}

static ViStatus serial_platform_read(SerialImpl *impl, ViBuf buf, ViUInt32 count,
                                     ViUInt32 *retCount, ViUInt32 timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(impl->fd, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(impl->fd + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0)  return VI_ERROR_IO;
    if (rc == 0) return VI_ERROR_TMO;

    ssize_t bytesRead = read(impl->fd, buf, count);
    if (bytesRead < 0)  return VI_ERROR_IO;
    if (bytesRead == 0) return VI_ERROR_TMO;

    if (retCount) *retCount = (ViUInt32)bytesRead;

    if (bytesRead > 0 && ((char*)buf)[bytesRead - 1] == '\n')
        return VI_SUCCESS_TERM_CHAR;

    return VI_SUCCESS;
}

#endif  /* POSIX */

/* ========== Transport vtable operations ========== */

static ViStatus serial_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    SerialImpl *impl = (SerialImpl*)self->impl;

    /* Build device path from comPort if not already set */
    if (impl->devPath[0] == '\0')
        serial_build_path(rsrc->comPort, impl->devPath, sizeof(impl->devPath));

    return serial_platform_open(impl, timeout);
}

static ViStatus serial_close(OvTransport *self) {
    SerialImpl *impl = (SerialImpl*)self->impl;
    serial_platform_close(impl);
    return VI_SUCCESS;
}

static ViStatus serial_write(OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    SerialImpl *impl = (SerialImpl*)self->impl;
    if (impl->fd == OV_INVALID_SERIAL) return VI_ERROR_CONN_LOST;
    return serial_platform_write(impl, buf, count, retCount);
}

static ViStatus serial_read(OvTransport *self, ViBuf buf, ViUInt32 count,
                            ViUInt32 *retCount, ViUInt32 timeout) {
    SerialImpl *impl = (SerialImpl*)self->impl;
    if (impl->fd == OV_INVALID_SERIAL) return VI_ERROR_CONN_LOST;
    return serial_platform_read(impl, buf, count, retCount, timeout);
}

static ViStatus serial_readSTB(OvTransport *self, ViUInt16 *stb) {
    /* Serial instruments typically don't have SRQ in the same sense.
     * Send *STB? and read the response. */
    const char *cmd = "*STB?\n";
    ViUInt32 retCount = 0;
    ViStatus st = serial_write(self, (ViBuf)(uintptr_t)cmd, 6, &retCount);
    if (st != VI_SUCCESS) return st;

    char buf[64];
    st = serial_read(self, (ViBuf)buf, sizeof(buf) - 1, &retCount, 2000);
    if (st != VI_SUCCESS && st != VI_SUCCESS_TERM_CHAR) return st;

    buf[retCount] = '\0';
    if (stb) *stb = (ViUInt16)atoi(buf);
    return VI_SUCCESS;
}

static ViStatus serial_clear(OvTransport *self) {
    /* Send *CLS to clear instrument status */
    const char *cmd = "*CLS\n";
    ViUInt32 retCount = 0;
    return serial_write(self, (ViBuf)(uintptr_t)cmd, 5, &retCount);
}

/* ========== Factory ========== */

OvTransport* ov_transport_serial_create(void) {
    OvTransport *t = (OvTransport*)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    SerialImpl *impl = (SerialImpl*)calloc(1, sizeof(SerialImpl));
    if (!impl) { free(t); return NULL; }

    /* Defaults: 9600 baud, 8N1, no flow control */
    impl->fd          = OV_INVALID_SERIAL;
    impl->baud        = 9600;
    impl->dataBits    = 8;
    impl->stopBits    = 10;   /* 1 stop bit */
    impl->parity      = 0;   /* none */
    impl->flowControl = 0;   /* none */

    t->impl     = impl;
    t->open     = serial_open;
    t->close    = serial_close;
    t->read     = serial_read;
    t->write    = serial_write;
    t->readSTB  = serial_readSTB;
    t->clear    = serial_clear;

    return t;
}
