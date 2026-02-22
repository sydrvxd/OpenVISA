/*
 * OpenVISA Example: Query *IDN? from a SCPI instrument
 *
 * Usage: ./example_idn TCPIP::192.168.1.50::5025::SOCKET
 *        ./example_idn TCPIP::192.168.1.50::INSTR
 */

#include <stdio.h>
#include <string.h>
#include "visa.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <VISA resource string>\n", argv[0]);
        printf("  e.g. %s TCPIP::192.168.1.50::5025::SOCKET\n", argv[0]);
        return 1;
    }

    ViSession rm, instr;
    ViStatus status;
    char buf[1024];
    ViUInt32 retCount;

    /* Open Resource Manager */
    status = viOpenDefaultRM(&rm);
    if (status != VI_SUCCESS) {
        printf("Failed to open Resource Manager: 0x%08X\n", (unsigned)status);
        return 1;
    }

    /* Open instrument */
    printf("Connecting to: %s\n", argv[1]);
    status = viOpen(rm, argv[1], VI_NO_LOCK, 5000, &instr);
    if (status != VI_SUCCESS) {
        char desc[256];
        viStatusDesc(rm, status, desc);
        printf("Failed to open instrument: %s (0x%08X)\n", desc, (unsigned)status);
        viClose(rm);
        return 1;
    }

    /* Set timeout */
    viSetAttribute(instr, VI_ATTR_TMO_VALUE, 5000);

    /* Send *IDN? query */
    const char *cmd = "*IDN?\n";
    status = viWrite(instr, (ViBuf)cmd, (ViUInt32)strlen(cmd), &retCount);
    if (status != VI_SUCCESS) {
        printf("Write failed: 0x%08X\n", (unsigned)status);
        viClose(instr);
        viClose(rm);
        return 1;
    }

    /* Read response */
    memset(buf, 0, sizeof(buf));
    status = viRead(instr, (ViBuf)buf, sizeof(buf) - 1, &retCount);
    if (status == VI_SUCCESS || status == VI_SUCCESS_TERM_CHAR || status == VI_SUCCESS_MAX_CNT) {
        buf[retCount] = '\0';
        /* Trim trailing newline */
        while (retCount > 0 && (buf[retCount-1] == '\n' || buf[retCount-1] == '\r'))
            buf[--retCount] = '\0';
        printf("Instrument ID: %s\n", buf);
    } else {
        printf("Read failed: 0x%08X\n", (unsigned)status);
    }

    /* Cleanup */
    viClose(instr);
    viClose(rm);

    printf("Done.\n");
    return 0;
}
