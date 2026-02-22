/*
 * OpenVISA - Resource string parser tests
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "visa.h"
#include "core/session.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  %-50s", name);
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_tcpip_socket(void) {
    TEST("TCPIP::host::port::SOCKET");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP::192.168.1.50::5025::SOCKET", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_TCPIP) { FAIL("wrong type"); return; }
    if (strcmp(r.host, "192.168.1.50") != 0) { FAIL("wrong host"); return; }
    if (r.port != 5025) { FAIL("wrong port"); return; }
    if (!r.isSocket) { FAIL("not socket"); return; }
    PASS();
}

void test_tcpip_instr(void) {
    TEST("TCPIP::host::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP::192.168.1.50::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_TCPIP) { FAIL("wrong type"); return; }
    if (strcmp(r.host, "192.168.1.50") != 0) { FAIL("wrong host"); return; }
    if (r.isSocket) { FAIL("should not be socket"); return; }
    PASS();
}

void test_tcpip_host_only(void) {
    TEST("TCPIP::hostname");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP::myoscilloscope.local", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (strcmp(r.host, "myoscilloscope.local") != 0) { FAIL("wrong host"); return; }
    if (strcmp(r.deviceName, "inst0") != 0) { FAIL("wrong device name"); return; }
    PASS();
}

void test_tcpip_with_board(void) {
    TEST("TCPIP2::host::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP2::10.0.0.1::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfNum != 2) { FAIL("wrong board num"); return; }
    if (strcmp(r.host, "10.0.0.1") != 0) { FAIL("wrong host"); return; }
    PASS();
}

void test_tcpip_hislip(void) {
    TEST("TCPIP::host::hislip0::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP::192.168.1.50::hislip0", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (!r.isHiSLIP) { FAIL("not HiSLIP"); return; }
    if (r.port != 4880) { FAIL("wrong port"); return; }
    if (strcmp(r.deviceName, "hislip0") != 0) { FAIL("wrong device name"); return; }
    PASS();
}

void test_tcpip_device_name(void) {
    TEST("TCPIP::host::inst0::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("TCPIP::192.168.1.50::inst0::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (strcmp(r.deviceName, "inst0") != 0) { FAIL("wrong device name"); return; }
    PASS();
}

void test_usb(void) {
    TEST("USB::0x1234::0x5678::SERIAL::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("USB::0x1234::0x5678::MY_SERIAL::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_USB) { FAIL("wrong type"); return; }
    if (r.usbVid != 0x1234) { FAIL("wrong VID"); return; }
    if (r.usbPid != 0x5678) { FAIL("wrong PID"); return; }
    if (strcmp(r.usbSerial, "MY_SERIAL") != 0) { FAIL("wrong serial"); return; }
    PASS();
}

void test_asrl(void) {
    TEST("ASRL3::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("ASRL3::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_ASRL) { FAIL("wrong type"); return; }
    if (r.comPort != 3) { FAIL("wrong COM port"); return; }
    PASS();
}

void test_gpib(void) {
    TEST("GPIB0::22::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("GPIB0::22::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_GPIB) { FAIL("wrong type"); return; }
    if (r.intfNum != 0) { FAIL("wrong board"); return; }
    if (r.gpibAddr != 22) { FAIL("wrong address"); return; }
    if (r.gpibSecAddr != -1) { FAIL("should be no secondary"); return; }
    PASS();
}

void test_gpib_secondary(void) {
    TEST("GPIB::1::2::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("GPIB::1::2::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.gpibAddr != 1) { FAIL("wrong primary addr"); return; }
    if (r.gpibSecAddr != 2) { FAIL("wrong secondary addr"); return; }
    PASS();
}

void test_invalid(void) {
    TEST("Invalid resource string");
    OvResource r;
    ViStatus st = ov_parse_rsrc("FOOBAR::something", &r);
    if (st == VI_SUCCESS) { FAIL("should have failed"); return; }
    PASS();
}

void test_case_insensitive(void) {
    TEST("Case insensitive: tcpip::host::INSTR");
    OvResource r;
    ViStatus st = ov_parse_rsrc("tcpip::192.168.1.1::INSTR", &r);
    if (st != VI_SUCCESS) { FAIL("parse failed"); return; }
    if (r.intfType != OV_INTF_TCPIP) { FAIL("wrong type"); return; }
    PASS();
}

int main(void) {
    printf("\n=== OpenVISA Resource Parser Tests ===\n\n");

    test_tcpip_socket();
    test_tcpip_instr();
    test_tcpip_host_only();
    test_tcpip_with_board();
    test_tcpip_hislip();
    test_tcpip_device_name();
    test_usb();
    test_asrl();
    test_gpib();
    test_gpib_secondary();
    test_invalid();
    test_case_insensitive();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
