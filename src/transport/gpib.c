/*
 * OpenVISA - GPIB Transport
 *
 * Dynamically loads linux-gpib (libgpib.so) on Linux/macOS
 * or gpib-32.dll on Windows.
 *
 * If the library is not present, all operations return VI_ERROR_NSUP_OPER.
 *
 * Supported GPIB resource string:
 *   GPIB{board}::{pad}[::sad]::INSTR
 *   e.g. GPIB0::22::INSTR
 */

#include "../core/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

/* ========== Platform DL includes ========== */

#ifdef OPENVISA_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    typedef HMODULE ov_dl_t;
    #define ov_dlopen(n)    LoadLibraryA(n)
    #define ov_dlsym(h, s)  ((void*)GetProcAddress((HMODULE)(h), (s)))
    #define ov_dlclose(h)   FreeLibrary((HMODULE)(h))
    #define OV_GPIB_LIB     "gpib-32.dll"
#else
    #include <dlfcn.h>
    typedef void* ov_dl_t;
    #define ov_dlopen(n)    dlopen((n), RTLD_LAZY | RTLD_LOCAL)
    #define ov_dlsym(h, s)  dlsym((h), (s))
    #define ov_dlclose(h)   dlclose(h)
    #ifdef __APPLE__
        #define OV_GPIB_LIB "libgpib.dylib"
    #else
        #define OV_GPIB_LIB "libgpib.so.0"
    #endif
#endif

/* ========== linux-gpib API types ========== */

/*
 * These mirror the linux-gpib / NI-488.2 ABI.
 * We declare them here to avoid requiring the SDK headers at build time.
 */

typedef int Addr4882_t;     /* board / device descriptor */

/* ibsta bits */
#define GPIB_TIMO   0x4000

/* iberr codes */
#define EDVR   1  /* system error */
#define ETMO  11  /* timeout */

/* ibconfig request codes */
#define IbcTMO 3

/* Timeout constants (T1..T13 → ~1us..~1000s) */
#define TNONE  0
#define T1us   1
#define T3us   2
#define T10us  3
#define T30us  4
#define T100us 5
#define T300us 6
#define T1ms   7
#define T3ms   8
#define T10ms  9
#define T30ms  10
#define T100ms 11
#define T300ms 12
#define T1000ms 13

/* ibrd/ibwrt/etc status is returned in ibsta, iberr, ibcntl globals.
 * We declare these as pointers resolved at load time. */

/* Function pointer typedefs */
typedef int (*fn_ibdev)(int boardIndex, int pad, int sad, int tmo, int eot, int eos);
typedef int (*fn_ibwrt)(int ud, const void *buf, long cnt);
typedef int (*fn_ibrd) (int ud, void *buf, long cnt);
typedef int (*fn_ibrsp)(int ud, char *spr);
typedef int (*fn_ibclr)(int ud);
typedef int (*fn_ibonl)(int ud, int v);
typedef int (*fn_ibconfig)(int ud, int option, int v);
typedef int* (*fn_ThreadIbsta)(void);
typedef int* (*fn_ThreadIberr)(void);
typedef long* (*fn_ThreadIbcntl)(void);

/* ========== GPIB implementation struct ========== */

typedef struct {
    ov_dl_t     lib;
    int         ud;        /* device descriptor from ibdev() */
    int         board;     /* board index */
    int         pad;       /* primary address */
    int         sad;       /* secondary address (-1 = none) */

    /* Resolved function pointers */
    fn_ibdev    p_ibdev;
    fn_ibwrt    p_ibwrt;
    fn_ibrd     p_ibrd;
    fn_ibrsp    p_ibrsp;
    fn_ibclr    p_ibclr;
    fn_ibonl    p_ibonl;
    fn_ibconfig p_ibconfig;

    /* Thread-local stat accessors (linux-gpib ≥ 4.x) — may be NULL */
    fn_ThreadIbsta  p_ibsta;
    fn_ThreadIberr  p_iberr;
    fn_ThreadIbcntl p_ibcntl;

    /* Fallback global symbols (linux-gpib < 4 / NI) */
    int  *g_ibsta;
    int  *g_iberr;
    long *g_ibcntl;
} GpibImpl;

/* ========== Helpers ========== */

/* Convert ms timeout to linux-gpib T* constant */
static int ms_to_tmo(ViUInt32 ms) {
    if (ms == 0)        return TNONE;
    if (ms < 1)         return T1us;
    if (ms < 3)         return T3us;
    if (ms < 10)        return T10us;
    if (ms < 30)        return T30us;
    if (ms < 100)       return T100us;
    if (ms < 300)       return T300us;
    if (ms < 1000)      return T1ms;
    if (ms < 3000)      return T3ms;
    if (ms < 10000)     return T10ms;
    if (ms < 30000)     return T30ms;
    if (ms < 100000)    return T100ms;
    if (ms < 300000)    return T300ms;
    return T1000ms;
}

static int gpib_get_ibsta(GpibImpl *g) {
    if (g->p_ibsta) return *g->p_ibsta();
    if (g->g_ibsta) return *g->g_ibsta;
    return 0;
}

static int gpib_get_iberr(GpibImpl *g) {
    if (g->p_iberr) return *g->p_iberr();
    if (g->g_iberr) return *g->g_iberr;
    return 0;
}

static long gpib_get_ibcntl(GpibImpl *g) {
    if (g->p_ibcntl) return *g->p_ibcntl();
    if (g->g_ibcntl) return *g->g_ibcntl;
    return 0;
}

/* Map ibsta/iberr to ViStatus */
static ViStatus gpib_map_status(GpibImpl *g, int call_ret) {
    if (call_ret < 0) return VI_ERROR_SYSTEM_ERROR;
    int sta = gpib_get_ibsta(g);
    if (sta & GPIB_TIMO) {
        int err = gpib_get_iberr(g);
        if (err == ETMO) return VI_ERROR_TMO;
    }
    if (sta & 0x8000 /* ERR bit */) {
        int err = gpib_get_iberr(g);
        (void)err;
        return VI_ERROR_IO;
    }
    return VI_SUCCESS;
}

/* ========== Dynamic library loading ========== */

static bool gpib_load_lib(GpibImpl *g) {
    /* Try primary name first, then versioned variants */
    const char *names[] = {
        OV_GPIB_LIB,
#ifndef OPENVISA_WINDOWS
#ifdef __linux__
        "libgpib.so",
        "libgpib.so.0.0.0",
#endif
#endif
        NULL
    };

    for (int i = 0; names[i]; i++) {
        g->lib = ov_dlopen(names[i]);
        if (g->lib) break;
    }

    if (!g->lib) return false;

    /* Mandatory functions */
    g->p_ibdev   = (fn_ibdev)   ov_dlsym(g->lib, "ibdev");
    g->p_ibwrt   = (fn_ibwrt)   ov_dlsym(g->lib, "ibwrt");
    g->p_ibrd    = (fn_ibrd)    ov_dlsym(g->lib, "ibrd");
    g->p_ibrsp   = (fn_ibrsp)   ov_dlsym(g->lib, "ibrsp");
    g->p_ibclr   = (fn_ibclr)   ov_dlsym(g->lib, "ibclr");
    g->p_ibonl   = (fn_ibonl)   ov_dlsym(g->lib, "ibonl");
    g->p_ibconfig = (fn_ibconfig)ov_dlsym(g->lib, "ibconfig");

    if (!g->p_ibdev || !g->p_ibwrt || !g->p_ibrd ||
        !g->p_ibrsp || !g->p_ibclr || !g->p_ibonl) {
        ov_dlclose(g->lib);
        g->lib = NULL;
        return false;
    }

    /* Optional: thread-local status accessors (linux-gpib 4.x) */
    g->p_ibsta  = (fn_ThreadIbsta)  ov_dlsym(g->lib, "ThreadIbsta");
    g->p_iberr  = (fn_ThreadIberr)  ov_dlsym(g->lib, "ThreadIberr");
    g->p_ibcntl = (fn_ThreadIbcntl) ov_dlsym(g->lib, "ThreadIbcntl");

    /* Fallback: global symbols (linux-gpib 3.x / NI-488.2) */
    if (!g->p_ibsta) {
        g->g_ibsta  = (int*)  ov_dlsym(g->lib, "ibsta");
        g->g_iberr  = (int*)  ov_dlsym(g->lib, "iberr");
        g->g_ibcntl = (long*) ov_dlsym(g->lib, "ibcntl");
    }

    return true;
}

/* ========== Transport vtable operations ========== */

static ViStatus gpib_open(OvTransport *self, const OvResource *rsrc, ViUInt32 timeout) {
    GpibImpl *g = (GpibImpl*)self->impl;

    if (!g->lib) return VI_ERROR_NSUP_OPER;

    g->board = rsrc->intfNum;
    g->pad   = rsrc->gpibAddr;
    g->sad   = rsrc->gpibSecAddr;    /* -1 if none */

    int tmo = ms_to_tmo(timeout);
    int sad = (g->sad >= 0) ? (g->sad | 0x60) : 0;  /* linux-gpib: SAD needs 0x60 offset */

    g->ud = g->p_ibdev(g->board, g->pad, sad, tmo,
                       1 /* EOI enabled */,
                       0 /* no EOS */);

    if (g->ud < 0) return VI_ERROR_RSRC_NFOUND;

    int ibsta = gpib_get_ibsta(g);
    if (ibsta & 0x8000 /* ERR */) return VI_ERROR_RSRC_NFOUND;

    return VI_SUCCESS;
}

static ViStatus gpib_close(OvTransport *self) {
    GpibImpl *g = (GpibImpl*)self->impl;
    if (!g->lib) return VI_SUCCESS;
    if (g->ud >= 0) {
        g->p_ibonl(g->ud, 0);   /* take device offline */
        g->ud = -1;
    }
    return VI_SUCCESS;
}

static ViStatus gpib_write(OvTransport *self, ViBuf buf, ViUInt32 count, ViUInt32 *retCount) {
    GpibImpl *g = (GpibImpl*)self->impl;
    if (!g->lib) return VI_ERROR_NSUP_OPER;
    if (g->ud < 0) return VI_ERROR_CONN_LOST;

    int rc = g->p_ibwrt(g->ud, buf, (long)count);
    ViStatus st = gpib_map_status(g, rc);

    if (retCount) {
        if (st == VI_SUCCESS)
            *retCount = (ViUInt32)gpib_get_ibcntl(g);
        else
            *retCount = 0;
    }
    return st;
}

static ViStatus gpib_read(OvTransport *self, ViBuf buf, ViUInt32 count,
                          ViUInt32 *retCount, ViUInt32 timeout) {
    GpibImpl *g = (GpibImpl*)self->impl;
    if (!g->lib) return VI_ERROR_NSUP_OPER;
    if (g->ud < 0) return VI_ERROR_CONN_LOST;

    /* Apply timeout for this call */
    if (g->p_ibconfig)
        g->p_ibconfig(g->ud, IbcTMO, ms_to_tmo(timeout));

    int rc = g->p_ibrd(g->ud, buf, (long)count);
    ViStatus st = gpib_map_status(g, rc);

    long got = gpib_get_ibcntl(g);
    if (retCount) *retCount = (st == VI_SUCCESS || st == VI_SUCCESS_TERM_CHAR)
                              ? (ViUInt32)got : 0;

    /* Check END (EOI received) bit in ibsta — bit 9 */
    if (st == VI_SUCCESS) {
        int sta = gpib_get_ibsta(g);
        if (sta & 0x0200 /* END */) st = VI_SUCCESS_TERM_CHAR;
    }

    return st;
}

static ViStatus gpib_readSTB(OvTransport *self, ViUInt16 *stb) {
    GpibImpl *g = (GpibImpl*)self->impl;
    if (!g->lib) return VI_ERROR_NSUP_OPER;
    if (g->ud < 0) return VI_ERROR_CONN_LOST;

    char spr = 0;
    int rc = g->p_ibrsp(g->ud, &spr);
    ViStatus st = gpib_map_status(g, rc);
    if (st != VI_SUCCESS) return st;

    if (stb) *stb = (ViUInt16)(unsigned char)spr;
    return VI_SUCCESS;
}

static ViStatus gpib_clear(OvTransport *self) {
    GpibImpl *g = (GpibImpl*)self->impl;
    if (!g->lib) return VI_ERROR_NSUP_OPER;
    if (g->ud < 0) return VI_ERROR_CONN_LOST;

    int rc = g->p_ibclr(g->ud);
    return gpib_map_status(g, rc);
}

/* ========== Factory ========== */

OvTransport* ov_transport_gpib_create(void) {
    OvTransport *t = (OvTransport*)calloc(1, sizeof(OvTransport));
    if (!t) return NULL;

    GpibImpl *g = (GpibImpl*)calloc(1, sizeof(GpibImpl));
    if (!g) { free(t); return NULL; }

    g->ud    = -1;
    g->board = 0;
    g->pad   = 1;
    g->sad   = -1;

    /* Attempt to load the GPIB library now.
     * If it fails, all ops will return VI_ERROR_NSUP_OPER. */
    gpib_load_lib(g);

    t->impl    = g;
    t->open    = gpib_open;
    t->close   = gpib_close;
    t->read    = gpib_read;
    t->write   = gpib_write;
    t->readSTB = gpib_readSTB;
    t->clear   = gpib_clear;

    return t;
}
