#pragma once
#include "ewr/payload.h"
#include "ewr/maintenance.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <string>
#include <vector>

namespace ewr {
    typedef void* EwrDeviceHandle;

    EwrDeviceHandle AutoConnectEpsonPrinter();
    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence);
    void DisconnectPrinter(EwrDeviceHandle hPrinter);

    // Sends a raw ESC/P2 print job (a maintenance sequence built in maintenance.h)
    // to a USB-connected printer's print channel. Unlike the waste-counter reset,
    // this goes straight to the print channel and returns no ACK, so success means
    // "the bytes were written", not "the printer confirmed it". 'label' names the
    // job for progress/log output. The network counterpart is LanSendPrintJob() in
    // snmp.h.
    bool SendUsbPrintJob(EwrDeviceHandle hPrinter, const std::vector<unsigned char>& job, const std::string& label);

    // Privilege management
    bool IsRunningElevated();
    // Relaunches the current executable with elevated privileges (Windows: UAC prompt).
    // Returns true if the relaunch was successfully requested (caller should exit).
    // Returns false if elevation isn't supported on this platform or the user declined.
    bool RelaunchElevated(int argc, char** argv);

    // Epson background process management (Status Monitor, scan services, etc.)
    struct EpsonProcessInfo
    {
        unsigned long pid;
        std::string name;
    };

    std::vector<EpsonProcessInfo> ListEpsonProcesses();
    // Attempts to terminate every process returned by ListEpsonProcesses().
    // Returns the number of processes successfully terminated.
    int KillEpsonProcesses();
}
