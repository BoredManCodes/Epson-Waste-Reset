#pragma once
#include "ewr/payload.h"
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
