/*
 * OpenVISA - visa.h
 * Main VISA API header (IVI Foundation compatible)
 * Apache 2.0 License
 */

#ifndef __VISA_H__
#define __VISA_H__

#include "visatype.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Resource Manager ========== */

ViStatus _VI_FUNC viOpenDefaultRM(ViSession *vi);

ViStatus _VI_FUNC viFindRsrc(
    ViSession sesn, ViString expr,
    ViFindList *findList, ViUInt32 *retcnt, ViChar desc[]);

ViStatus _VI_FUNC viFindNext(ViFindList findList, ViChar desc[]);

ViStatus _VI_FUNC viParseRsrc(
    ViSession sesn, ViRsrc rsrcName,
    ViUInt16 *intfType, ViUInt16 *intfNum);

ViStatus _VI_FUNC viParseRsrcEx(
    ViSession sesn, ViRsrc rsrcName,
    ViUInt16 *intfType, ViUInt16 *intfNum,
    ViChar rsrcClass[], ViChar expandedUnaliasedName[],
    ViChar aliasIfExists[]);

ViStatus _VI_FUNC viOpen(
    ViSession sesn, ViRsrc rsrcName,
    ViAccessMode accessMode, ViUInt32 openTimeout,
    ViSession *vi);

ViStatus _VI_FUNC viClose(ViObject vi);

/* ========== Basic I/O ========== */

ViStatus _VI_FUNC viRead(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViUInt32 *retCount);

ViStatus _VI_FUNC viReadAsync(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViJobId *jobId);

ViStatus _VI_FUNC viReadToFile(
    ViSession vi, ViConstString filename,
    ViUInt32 count, ViUInt32 *retCount);

ViStatus _VI_FUNC viWrite(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViUInt32 *retCount);

ViStatus _VI_FUNC viWriteAsync(
    ViSession vi, ViBuf buf,
    ViUInt32 count, ViJobId *jobId);

ViStatus _VI_FUNC viWriteFromFile(
    ViSession vi, ViConstString filename,
    ViUInt32 count, ViUInt32 *retCount);

ViStatus _VI_FUNC viAssertTrigger(
    ViSession vi, ViUInt16 protocol);

ViStatus _VI_FUNC viReadSTB(
    ViSession vi, ViUInt16 *status);

ViStatus _VI_FUNC viClear(ViSession vi);

/* ========== Formatted I/O ========== */

ViStatus _VI_FUNCH viPrintf(ViSession vi, ViString writeFmt, ...);
ViStatus _VI_FUNCH viSPrintf(ViSession vi, ViBuf buf, ViString writeFmt, ...);
ViStatus _VI_FUNCH viScanf(ViSession vi, ViString readFmt, ...);
ViStatus _VI_FUNCH viSScanf(ViSession vi, ViBuf buf, ViString readFmt, ...);
ViStatus _VI_FUNCH viQueryf(ViSession vi, ViString writeFmt, ViString readFmt, ...);

ViStatus _VI_FUNC viVPrintf(ViSession vi, ViString writeFmt, va_list params);
ViStatus _VI_FUNC viVSPrintf(ViSession vi, ViBuf buf, ViString writeFmt, va_list params);
ViStatus _VI_FUNC viVScanf(ViSession vi, ViString readFmt, va_list params);
ViStatus _VI_FUNC viVSScanf(ViSession vi, ViBuf buf, ViString readFmt, va_list params);
ViStatus _VI_FUNC viVQueryf(ViSession vi, ViString writeFmt, ViString readFmt, va_list params);

ViStatus _VI_FUNC viFlush(ViSession vi, ViUInt16 mask);
ViStatus _VI_FUNC viSetBuf(ViSession vi, ViUInt16 mask, ViUInt32 size);

/* ========== Attributes ========== */

ViStatus _VI_FUNC viGetAttribute(
    ViSession vi, ViAttr attribute, void *attrState);

ViStatus _VI_FUNC viSetAttribute(
    ViSession vi, ViAttr attribute, ViAttrState attrState);

/* ========== Events ========== */

ViStatus _VI_FUNC viEnableEvent(
    ViSession vi, ViEventType eventType,
    ViUInt16 mechanism, ViEventFilter context);

ViStatus _VI_FUNC viDisableEvent(
    ViSession vi, ViEventType eventType,
    ViUInt16 mechanism);

ViStatus _VI_FUNC viDiscardEvents(
    ViSession vi, ViEventType eventType,
    ViUInt16 mechanism);

ViStatus _VI_FUNC viWaitOnEvent(
    ViSession vi, ViEventType inEventType,
    ViUInt32 timeout, ViEventType *outEventType,
    ViEvent *outContext);

typedef ViStatus (_VI_FUNCH *ViHndlr)(
    ViSession vi, ViEventType eventType,
    ViEvent context, ViAddr userHandle);

ViStatus _VI_FUNC viInstallHandler(
    ViSession vi, ViEventType eventType,
    ViHndlr handler, ViAddr userHandle);

ViStatus _VI_FUNC viUninstallHandler(
    ViSession vi, ViEventType eventType,
    ViHndlr handler, ViAddr userHandle);

/* ========== Locking ========== */

ViStatus _VI_FUNC viLock(
    ViSession vi, ViAccessMode lockType,
    ViUInt32 timeout, ViKeyId requestedKey,
    ViChar accessKey[]);

ViStatus _VI_FUNC viUnlock(ViSession vi);

/* ========== Utility ========== */

ViStatus _VI_FUNC viStatusDesc(
    ViSession vi, ViStatus status, ViChar desc[]);

ViStatus _VI_FUNC viTerminate(
    ViSession vi, ViUInt16 degree, ViJobId jobId);

/* ========== Memory / Register (low-level, stubs for compatibility) ========== */

ViStatus _VI_FUNC viMapAddress(
    ViSession vi, ViUInt16 mapSpace,
    ViBusAddress mapOffset, ViBusSize mapSize,
    ViBoolean access, ViAddr suggested, ViAddr *address);

ViStatus _VI_FUNC viUnmapAddress(ViSession vi);

ViStatus _VI_FUNC viMoveIn8(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt8 *buf8);
ViStatus _VI_FUNC viMoveIn16(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt16 *buf16);
ViStatus _VI_FUNC viMoveIn32(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt32 *buf32);
ViStatus _VI_FUNC viMoveOut8(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt8 *buf8);
ViStatus _VI_FUNC viMoveOut16(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt16 *buf16);
ViStatus _VI_FUNC viMoveOut32(ViSession vi, ViUInt16 space, ViBusAddress offset, ViBusSize length, ViUInt32 *buf32);

#ifdef __cplusplus
}
#endif

#endif /* __VISA_H__ */
