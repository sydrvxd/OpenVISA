/*
 * OpenVISA - Transport factory
 */

#include "../core/session.h"
#include <stdlib.h>

/* Forward declarations for transport constructors */
extern OvTransport* ov_transport_tcpip_raw_create(void);
/* TODO: extern OvTransport* ov_transport_tcpip_vxi11_create(void); */
/* TODO: extern OvTransport* ov_transport_tcpip_hislip_create(void); */
/* TODO: extern OvTransport* ov_transport_usbtmc_create(void); */
/* TODO: extern OvTransport* ov_transport_serial_create(void); */

OvTransport* ov_transport_create(OvIntfType type) {
    switch (type) {
        case OV_INTF_TCPIP:
            /* For now, all TCPIP uses raw socket transport.
             * TODO: select VXI-11 or HiSLIP based on resource string */
            return ov_transport_tcpip_raw_create();

        case OV_INTF_USB:
            /* TODO: USBTMC transport */
            return NULL;

        case OV_INTF_ASRL:
            /* TODO: Serial transport */
            return NULL;

        case OV_INTF_GPIB:
            /* TODO: GPIB transport */
            return NULL;

        default:
            return NULL;
    }
}
