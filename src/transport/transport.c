/*
 * OpenVISA - Transport factory
 */

#include "../core/session.h"
#include <stdlib.h>

/* Forward declarations for transport constructors */
extern OvTransport* ov_transport_tcpip_raw_create(void);
extern OvTransport* ov_transport_tcpip_vxi11_create(void);
extern OvTransport* ov_transport_tcpip_hislip_create(void);
extern OvTransport* ov_transport_usbtmc_create(void);
extern OvTransport* ov_transport_serial_create(void);
extern OvTransport* ov_transport_gpib_create(void);

/*
 * ov_transport_create_for_rsrc
 *
 * Resource-aware factory: selects the appropriate transport implementation
 * based on both the interface type and protocol-specific flags in the parsed
 * resource descriptor.
 */
OvTransport* ov_transport_create_for_rsrc(const OvResource *rsrc) {
    if (!rsrc) return NULL;

    switch (rsrc->intfType) {
        case OV_INTF_TCPIP:
            if (rsrc->isHiSLIP)
                return ov_transport_tcpip_hislip_create();
            if (rsrc->isSocket)
                return ov_transport_tcpip_raw_create();
            /* Default INSTR mode → VXI-11 (standard VISA behavior) */
            return ov_transport_tcpip_vxi11_create();

        case OV_INTF_USB:
            return ov_transport_usbtmc_create();

        case OV_INTF_ASRL:
            return ov_transport_serial_create();

        case OV_INTF_GPIB:
            return ov_transport_gpib_create();

        default:
            return NULL;
    }
}

/*
 * ov_transport_create  (legacy, type-only variant)
 */
OvTransport* ov_transport_create(OvIntfType type) {
    switch (type) {
        case OV_INTF_TCPIP:  return ov_transport_tcpip_vxi11_create();
        case OV_INTF_USB:    return ov_transport_usbtmc_create();
        case OV_INTF_ASRL:   return ov_transport_serial_create();
        case OV_INTF_GPIB:   return ov_transport_gpib_create();
        default:             return NULL;
    }
}
