# OpenVISA

**Open-source, vendor-free VISA (Virtual Instrument Software Architecture) implementation.**

Drop-in replacement for NI-VISA / Keysight IO Libraries. No vendor runtime needed.

## Why?

Every test engineer knows the pain: NI-VISA and Keysight VISA fight over `visa32.dll`, GPIB drivers conflict, and you need a 2GB installer just to send `*IDN?` over TCP. OpenVISA fixes this.

## Features

- **Drop-in compatible** — implements the standard VISA C API (`visa32.dll` / `visa64.dll`)
- **No vendor runtime** — pure implementation, zero dependencies on NI or Keysight
- **Transport backends:**
  - ✅ TCPIP (VXI-11 + HiSLIP + Raw Socket)
  - ✅ USB (USBTMC via libusb)
  - ✅ Serial (ASRL)
  - 🔜 GPIB (via linux-gpib or compatible controllers)
- **Cross-platform** — Windows (primary), Linux, macOS
- **Lightweight** — single DLL/SO, <1MB
- **Auto-discovery** — LXI/mDNS, USB enumeration
- **Apache 2.0 license**

## Quick Start

```c
#include <visa.h>

ViSession rm, instr;
char buf[256];
ViUInt32 retCount;

viOpenDefaultRM(&rm);
viOpen(rm, "TCPIP::192.168.1.50::INSTR", VI_NULL, VI_NULL, &instr);
viWrite(instr, "*IDN?\n", 6, &retCount);
viRead(instr, buf, sizeof(buf), &retCount);
printf("Instrument: %s\n", buf);
viClose(instr);
viClose(rm);
```

Existing VISA programs work without code changes — just swap the DLL.

## Architecture

```
┌─────────────────────────────────────────┐
│           Application (your code)        │
├─────────────────────────────────────────┤
│         OpenVISA API (visa.h)           │
│  viOpen · viRead · viWrite · viClose    │
├──────┬──────┬──────┬────────────────────┤
│ TCPIP│  USB │Serial│  GPIB (optional)   │
│      │      │      │                    │
│VXI-11│USBTMC│ OS   │  linux-gpib /      │
│HiSLIP│libusb│native│  NI-488.2          │
└──────┴──────┴──────┴────────────────────┘
```

## Building

### Windows (MSVC)
```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
# Output: build/Release/visa32.dll + visa64.dll
```

### Linux
```bash
cmake -B build
cmake --build build
# Output: build/libvisa.so
```

## Project Status

| Component | Status |
|-----------|--------|
| Core API (Resource Manager, Sessions) | ✅ Complete |
| TCPIP – Raw Socket (`TCPIP::host::port::SOCKET`) | ✅ Complete |
| TCPIP – VXI-11 (ONC RPC, `TCPIP::host::INSTR`) | ✅ Complete |
| TCPIP – HiSLIP (`TCPIP::host::hislip0`) | ✅ Complete |
| USB – USBTMC (via libusb, optional) | ✅ Complete |
| Serial (ASRL) | ✅ Complete |
| GPIB (via linux-gpib/NI-488.2, dynamic loading) | ✅ Complete |
| Auto-Discovery (mDNS/LXI + USB + Serial) | ✅ Complete |
| Formatted I/O (viPrintf/viQueryf) | ✅ Complete |
| Attributes (viGet/SetAttribute) | ✅ Complete |
| Resource String Parser (all types) | ✅ Complete (12/12 tests) |

## Contributing

PRs welcome! See [CONTRIBUTING.md](docs/CONTRIBUTING.md) for guidelines.

## License

Apache 2.0 — use it freely, commercially, wherever.
