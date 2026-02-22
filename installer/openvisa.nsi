; OpenVISA NSIS Installer
; Installs visa32.dll + visa64.dll to IVI Foundation standard paths
; and adds to system PATH

!include "MUI2.nsh"
!include "x64.nsh"

; ---- Configuration ----
!define PRODUCT_NAME "OpenVISA"
!define PRODUCT_VERSION "0.2.0"
!define PRODUCT_PUBLISHER "sydrvxd"
!define PRODUCT_WEB "https://github.com/sydrvxd/OpenVISA"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define IVI_PATH "$PROGRAMFILES64\IVI Foundation\VISA"
!define IVI_PATH32 "$PROGRAMFILES\IVI Foundation\VISA"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "..\dist\OpenVISA-${PRODUCT_VERSION}-setup-win64.exe"
InstallDir "${IVI_PATH}\Win64\Bin"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; ---- MUI Settings ----
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to OpenVISA ${PRODUCT_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This will install OpenVISA — an open-source, vendor-free VISA implementation.$\r$\n$\r$\nDrop-in replacement for NI-VISA / Keysight IO Libraries.$\r$\n$\r$\nInstalls:$\r$\n  $\u2022 visa64.dll (64-bit) $\u2192 System32 + IVI Foundation$\r$\n  $\u2022 visa32.dll (32-bit) $\u2192 SysWOW64 + IVI Foundation$\r$\n  $\u2022 Header files (visa.h, visatype.h)$\r$\n$\r$\nClick Next to continue."

; ---- Pages ----
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---- Helper: Add/Remove from PATH ----

Function AddToPath
    ; Read current system PATH
    ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
    ; Check if already contains our path
    StrCpy $1 "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin"
    Push $0
    Push $1
    Call StrContains
    Pop $2
    StrCmp $2 "" 0 +3
        ; Not found — append
        WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$0;$1"
        ; Notify Windows of environment change
        SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

Function un.RemoveFromPath
    ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
    StrCpy $1 "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin"
    ; Simple removal: replace ";$1" and "$1;" with ""
    Push $0
    Push ";$1"
    Push ""
    Call un.StrReplace
    Pop $0
    Push $0
    Push "$1;"
    Push ""
    Call un.StrReplace
    Pop $0
    Push $0
    Push "$1"
    Push ""
    Call un.StrReplace
    Pop $0
    WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$0"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

; ---- String helpers ----

Function StrContains
    Exch $R1 ; needle
    Exch
    Exch $R0 ; haystack
    Push $R2
    Push $R3
    Push $R4
    StrLen $R3 $R1
    StrCpy $R4 0
    loop:
        StrCpy $R2 $R0 $R3 $R4
        StrCmp $R2 "" notfound
        StrCmp $R2 $R1 found
        IntOp $R4 $R4 + 1
        Goto loop
    found:
        StrCpy $R0 $R1
        Goto done
    notfound:
        StrCpy $R0 ""
    done:
    Pop $R4
    Pop $R3
    Pop $R2
    Exch $R0
    Exch
    Pop $R1
FunctionEnd

Function un.StrReplace
    Exch $R2 ; replacement
    Exch
    Exch $R1 ; search
    Exch 2
    Exch $R0 ; string
    Push $R3
    Push $R4
    Push $R5
    StrLen $R3 $R1
    StrCpy $R5 ""
    StrCpy $R4 0
    loop:
        StrCpy $R6 $R0 $R3 $R4
        StrCmp $R6 "" done
        StrCmp $R6 $R1 found
        StrCpy $R6 $R0 1 $R4
        StrCpy $R5 "$R5$R6"
        IntOp $R4 $R4 + 1
        Goto loop
    found:
        StrCpy $R5 "$R5$R2"
        IntOp $R4 $R4 + $R3
        Goto loop
    done:
    StrCpy $R0 $R5
    Pop $R5
    Pop $R4
    Pop $R3
    Exch $R0
    Exch 2
    Pop $R1
    Pop $R2
FunctionEnd

; ---- Install Sections ----

Section "OpenVISA Core (required)" SecCore
    SectionIn RO

    ; === 64-bit DLL → IVI Foundation + System32 ===
    SetOutPath "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin"
    File "..\build-win64\visa64.dll"

    SetOutPath "$WINDIR\System32"
    File "..\build-win64\visa64.dll"

    ; === 32-bit DLL → IVI Foundation + SysWOW64 ===
    SetOutPath "$PROGRAMFILES\IVI Foundation\VISA\Win32\Bin"
    File "..\build-win32\visa32.dll"

    ${If} ${RunningX64}
        SetOutPath "$WINDIR\SysWOW64"
        File "..\build-win32\visa32.dll"
    ${EndIf}

    ; === Headers ===
    SetOutPath "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Include"
    File "..\include\visa.h"
    File "..\include\visatype.h"

    ; === Import libraries (for C/C++ developers) ===
    SetOutPath "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Lib_x64\msc"
    File "..\build-win64\libvisa64.dll.a"

    SetOutPath "$PROGRAMFILES\IVI Foundation\VISA\Win32\Lib\msc"
    File "..\build-win32\libvisa32.dll.a"

    ; === Add to PATH ===
    Call AddToPath

    ; === Registry: Uninstaller ===
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME} ${PRODUCT_VERSION}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" '"$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin\OpenVISA-uninstall.exe"'
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB}"
    WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1

    ; === Registry: IVI Foundation VISA implementation ===
    WriteRegStr HKLM "Software\IVI Foundation\VISA\CurrentVersion" "ProductName" "OpenVISA"
    WriteRegStr HKLM "Software\IVI Foundation\VISA\CurrentVersion" "ProductVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\IVI Foundation\VISA\CurrentVersion" "Path" "$PROGRAMFILES64\IVI Foundation\VISA"
    WriteRegStr HKLM "Software\IVI Foundation\VISA\CurrentVersion" "DllPath64" "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin\visa64.dll"
    WriteRegStr HKLM "Software\IVI Foundation\VISA\CurrentVersion" "DllPath32" "$PROGRAMFILES\IVI Foundation\VISA\Win32\Bin\visa32.dll"

    ; === Uninstaller ===
    SetOutPath "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin"
    WriteUninstaller "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin\OpenVISA-uninstall.exe"

SectionEnd

Section "Example programs" SecExamples
    SetOutPath "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Examples\OpenVISA"
    File "..\examples\idn_query.c"
    File "..\build-win64\example_idn.exe"
SectionEnd

; ---- Uninstall ----

Section "Uninstall"
    ; DLLs
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin\visa64.dll"
    Delete "$PROGRAMFILES\IVI Foundation\VISA\Win32\Bin\visa32.dll"
    Delete "$WINDIR\System32\visa64.dll"
    ${If} ${RunningX64}
        Delete "$WINDIR\SysWOW64\visa32.dll"
    ${EndIf}

    ; Headers
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Include\visa.h"
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Include\visatype.h"

    ; Import libs
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Lib_x64\msc\libvisa64.dll.a"
    Delete "$PROGRAMFILES\IVI Foundation\VISA\Win32\Lib\msc\libvisa32.dll.a"

    ; Examples
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Examples\OpenVISA\idn_query.c"
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Examples\OpenVISA\example_idn.exe"
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Examples\OpenVISA"

    ; Uninstaller
    Delete "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin\OpenVISA-uninstall.exe"

    ; Remove from PATH
    Call un.RemoveFromPath

    ; Registry
    DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
    DeleteRegKey HKLM "Software\IVI Foundation\VISA\CurrentVersion"

    ; Try removing empty dirs (won't delete if non-empty)
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Include"
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Lib_x64\msc"
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Lib_x64"
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64\Bin"
    RMDir "$PROGRAMFILES64\IVI Foundation\VISA\Win64"
    RMDir "$PROGRAMFILES\IVI Foundation\VISA\Win32\Lib\msc"
    RMDir "$PROGRAMFILES\IVI Foundation\VISA\Win32\Lib"
    RMDir "$PROGRAMFILES\IVI Foundation\VISA\Win32\Bin"
    RMDir "$PROGRAMFILES\IVI Foundation\VISA\Win32"
SectionEnd
