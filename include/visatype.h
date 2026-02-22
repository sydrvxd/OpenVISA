/*
 * OpenVISA - visatype.h
 * VISA type definitions (IVI Foundation compatible)
 * Apache 2.0 License
 */

#ifndef __VISATYPE_H__
#define __VISATYPE_H__

#include <stdint.h>
#include <stdarg.h>

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
    #define OPENVISA_WINDOWS
    #ifdef OPENVISA_EXPORTS
        #define _VI_FUNC __declspec(dllexport) __cdecl
        #define _VI_FUNCH __declspec(dllexport) __cdecl
    #else
        #define _VI_FUNC __declspec(dllimport) __cdecl
        #define _VI_FUNCH __declspec(dllimport) __cdecl
    #endif
#else
    #define _VI_FUNC
    #define _VI_FUNCH
#endif

/* Fundamental VISA types */
typedef uint32_t    ViUInt32;
typedef int32_t     ViInt32;
typedef uint16_t    ViUInt16;
typedef int16_t     ViInt16;
typedef uint8_t     ViUInt8;
typedef int8_t      ViInt8;
typedef uint64_t    ViUInt64;
typedef int64_t     ViInt64;
typedef float       ViReal32;
typedef double      ViReal64;
typedef char        ViChar;
typedef ViChar*     ViString;
typedef const ViChar* ViConstString;
typedef ViUInt8*    ViBuf;
typedef const ViUInt8* ViConstBuf;
typedef void*       ViAddr;
typedef unsigned char ViByte;
typedef int         ViBoolean;

/* Session and object types */
typedef ViUInt32    ViObject;
typedef ViObject    ViSession;
typedef ViSession   ViFindList;
typedef ViString    ViRsrc;
typedef ViConstString ViConstRsrc;
typedef ViString    ViKeyId;
typedef ViString    ViAccessMode_str;

/* Status and attribute types */
typedef ViInt32     ViStatus;
typedef ViUInt32    ViAttr;
typedef ViUInt32    ViAttrState;
typedef ViUInt32    ViEventType;
typedef ViObject    ViEvent;
typedef ViUInt32    ViEventFilter;
typedef ViUInt32    ViAccessMode;
typedef ViUInt32    ViBusAddress;
typedef ViUInt64    ViBusAddress64;
typedef ViUInt32    ViBusSize;
typedef ViUInt16    ViJobId;

/* Boolean values */
#define VI_TRUE     (1)
#define VI_FALSE    (0)
#define VI_NULL     (0)

/* Access modes */
#define VI_NO_LOCK             (0)
#define VI_EXCLUSIVE_LOCK      (1)
#define VI_SHARED_LOCK         (2)
#define VI_LOAD_CONFIG         (4)

/* Timeout values */
#define VI_TMO_IMMEDIATE       (0)
#define VI_TMO_INFINITE        (0xFFFFFFFF)

/* Status codes */
#define VI_SUCCESS                   (0x00000000L)
#define VI_SUCCESS_EVENT_EN          (0x3FFF0002L)
#define VI_SUCCESS_EVENT_DIS         (0x3FFF0003L)
#define VI_SUCCESS_QUEUE_EMPTY       (0x3FFF0004L)
#define VI_SUCCESS_TERM_CHAR         (0x3FFF0005L)
#define VI_SUCCESS_MAX_CNT           (0x3FFF0006L)
#define VI_SUCCESS_DEV_NPRESENT      (0x3FFF007DL)
#define VI_SUCCESS_TRIG_MAPPED       (0x3FFF007EL)
#define VI_SUCCESS_QUEUE_NEMPTY      (0x3FFF0080L)
#define VI_SUCCESS_NCHAIN            (0x3FFF0098L)
#define VI_SUCCESS_NESTED_SHARED     (0x3FFF0099L)
#define VI_SUCCESS_NESTED_EXCLUSIVE  (0x3FFF009AL)
#define VI_SUCCESS_SYNC              (0x3FFF009BL)

#define VI_WARN_QUEUE_OVERFLOW       (0x3FFF000CL)
#define VI_WARN_CONFIG_NLOADED       (0x3FFF0077L)
#define VI_WARN_NULL_OBJECT          (0x3FFF0082L)
#define VI_WARN_NSUP_ATTR_STATE      (0x3FFF0084L)
#define VI_WARN_UNKNOWN_STATUS       (0x3FFF0085L)
#define VI_WARN_NSUP_BUF             (0x3FFF0088L)
#define VI_WARN_EXT_FUNC_NIMPL       (0x3FFF00A9L)

#define VI_ERROR_SYSTEM_ERROR        (0xBFFF0000L)
#define VI_ERROR_INV_OBJECT          (0xBFFF000EL)
#define VI_ERROR_RSRC_LOCKED         (0xBFFF000FL)
#define VI_ERROR_INV_EXPR            (0xBFFF0010L)
#define VI_ERROR_RSRC_NFOUND         (0xBFFF0011L)
#define VI_ERROR_INV_RSRC_NAME       (0xBFFF0012L)
#define VI_ERROR_INV_ACC_MODE        (0xBFFF0013L)
#define VI_ERROR_TMO                 (0xBFFF0015L)
#define VI_ERROR_CLOSING_FAILED      (0xBFFF0016L)
#define VI_ERROR_INV_DEGREE          (0xBFFF001BL)
#define VI_ERROR_INV_JOB_ID          (0xBFFF001CL)
#define VI_ERROR_NSUP_ATTR           (0xBFFF001DL)
#define VI_ERROR_NSUP_ATTR_STATE     (0xBFFF001EL)
#define VI_ERROR_ATTR_READONLY       (0xBFFF001FL)
#define VI_ERROR_INV_LOCK_TYPE       (0xBFFF0020L)
#define VI_ERROR_INV_ACCESS_KEY      (0xBFFF0021L)
#define VI_ERROR_INV_EVENT           (0xBFFF0026L)
#define VI_ERROR_INV_MECH            (0xBFFF0027L)
#define VI_ERROR_HNDLR_NINSTALLED    (0xBFFF0028L)
#define VI_ERROR_INV_HNDLR_REF       (0xBFFF0029L)
#define VI_ERROR_INV_CONTEXT         (0xBFFF002AL)
#define VI_ERROR_QUEUE_OVERFLOW      (0xBFFF002DL)
#define VI_ERROR_NENABLED            (0xBFFF002FL)
#define VI_ERROR_ABORT               (0xBFFF0030L)
#define VI_ERROR_RAW_WR_PROT_VIOL    (0xBFFF0034L)
#define VI_ERROR_RAW_RD_PROT_VIOL    (0xBFFF0035L)
#define VI_ERROR_OUTP_PROT_VIOL      (0xBFFF0036L)
#define VI_ERROR_INP_PROT_VIOL       (0xBFFF0037L)
#define VI_ERROR_BERR                (0xBFFF0038L)
#define VI_ERROR_IN_PROGRESS         (0xBFFF0039L)
#define VI_ERROR_INV_SETUP           (0xBFFF003AL)
#define VI_ERROR_QUEUE_ERROR         (0xBFFF003BL)
#define VI_ERROR_ALLOC               (0xBFFF003CL)
#define VI_ERROR_INV_MASK            (0xBFFF003DL)
#define VI_ERROR_IO                  (0xBFFF003EL)
#define VI_ERROR_INV_FMT             (0xBFFF003FL)
#define VI_ERROR_NSUP_FMT            (0xBFFF0041L)
#define VI_ERROR_LINE_IN_USE         (0xBFFF0042L)
#define VI_ERROR_LINE_NRESERVED      (0xBFFF0043L)
#define VI_ERROR_NSUP_MODE           (0xBFFF0046L)
#define VI_ERROR_SRQ_NOCCURRED       (0xBFFF004AL)
#define VI_ERROR_INV_SPACE           (0xBFFF004EL)
#define VI_ERROR_INV_OFFSET          (0xBFFF0051L)
#define VI_ERROR_INV_WIDTH           (0xBFFF0052L)
#define VI_ERROR_NSUP_OFFSET         (0xBFFF0054L)
#define VI_ERROR_NSUP_VAR_WIDTH      (0xBFFF0055L)
#define VI_ERROR_WINDOW_NMAPPED      (0xBFFF0057L)
#define VI_ERROR_RESP_PENDING        (0xBFFF0059L)
#define VI_ERROR_NLISTENERS          (0xBFFF005FL)
#define VI_ERROR_NCIC                (0xBFFF0060L)
#define VI_ERROR_NSYS_CNTLR          (0xBFFF0061L)
#define VI_ERROR_NSUP_OPER           (0xBFFF0067L)
#define VI_ERROR_INTR_PENDING        (0xBFFF0068L)
#define VI_ERROR_ASRL_PARITY         (0xBFFF006AL)
#define VI_ERROR_ASRL_FRAMING        (0xBFFF006BL)
#define VI_ERROR_ASRL_OVERRUN        (0xBFFF006CL)
#define VI_ERROR_CONN_LOST           (0xBFFF006DL)
#define VI_ERROR_INV_PROT            (0xBFFF006EL)
#define VI_ERROR_INV_SIZE            (0xBFFF006FL)

/* Attribute IDs */
#define VI_ATTR_RSRC_CLASS           (0xBFFF0001L)
#define VI_ATTR_RSRC_NAME            (0xBFFF0002L)
#define VI_ATTR_RSRC_IMPL_VERSION    (0x3FFF0003L)
#define VI_ATTR_RSRC_LOCK_STATE      (0x3FFF0004L)
#define VI_ATTR_MAX_QUEUE_LENGTH     (0x3FFF0005L)
#define VI_ATTR_USER_DATA            (0x3FFF0007L)
#define VI_ATTR_FDC_CHNL             (0x3FFF000DL)
#define VI_ATTR_FDC_MODE             (0x3FFF000FL)
#define VI_ATTR_FDC_GEN_SIGNAL_EN    (0x3FFF0011L)
#define VI_ATTR_FDC_USE_PAIR         (0x3FFF0013L)
#define VI_ATTR_SEND_END_EN          (0x3FFF0016L)
#define VI_ATTR_TERMCHAR             (0x3FFF0018L)
#define VI_ATTR_TMO_VALUE            (0x3FFF001AL)
#define VI_ATTR_INTF_TYPE            (0x3FFF0171L)
#define VI_ATTR_INTF_NUM             (0x3FFF0176L)
#define VI_ATTR_TCPIP_ADDR           (0xBFFF0195L)
#define VI_ATTR_TCPIP_HOSTNAME       (0xBFFF0196L)
#define VI_ATTR_TCPIP_PORT           (0x3FFF0197L)
#define VI_ATTR_TCPIP_DEVICE_NAME    (0xBFFF0199L)
#define VI_ATTR_MANF_NAME            (0xBFFF0072L)
#define VI_ATTR_MODEL_NAME           (0xBFFF0077L)
#define VI_ATTR_USB_SERIAL_NUM       (0xBFFF01A0L)
#define VI_ATTR_USB_INTFC_NUM        (0x3FFF01A1L)
#define VI_ATTR_TERMCHAR_EN          (0x3FFF0038L)
#define VI_ATTR_RSRC_MANF_NAME       (0xBFFF0172L)
#define VI_ATTR_RSRC_MANF_ID         (0x3FFF0175L)

/* Interface types */
#define VI_INTF_GPIB                 (1)
#define VI_INTF_VXI                  (2)
#define VI_INTF_GPIB_VXI             (3)
#define VI_INTF_ASRL                 (4)
#define VI_INTF_PXI                  (5)
#define VI_INTF_TCPIP                (6)
#define VI_INTF_USB                  (7)

/* Event types */
#define VI_EVENT_SERVICE_REQ         (0x3FFF200BL)
#define VI_EVENT_IO_COMPLETION       (0x3FFF2009L)

/* Event mechanisms */
#define VI_QUEUE                     (1)
#define VI_HNDLR                     (2)
#define VI_SUSPEND_HNDLR             (4)
#define VI_ALL_MECH                  (0xFFFF)

/* Read/Write termination */
#define VI_ASRL_END_TERMCHAR         (2)

/* Util macro */
#define VI_SPEC_VERSION              (0x00700200L)  /* VISA 7.2 */

#endif /* __VISATYPE_H__ */
