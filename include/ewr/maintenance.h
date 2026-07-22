#pragma once
#include <vector>

// Printer maintenance print jobs (nozzle check, head cleaning, test patterns,
// paper feed), shared by every transport. Unlike the waste-counter reset (which
// writes model-specific values to the EEPROM control channel), these are plain,
// mostly model-independent ESC/P2 print streams. The same bytes are sent to the
// USB print channel (usb.h SendUsbPrintJob) or to a network printer over LPR
// (snmp.h LanSendPrintJob).
//
// The byte sequences mirror the proven ones in Ircama's epson_escp2 encoder
// (epson_escp2/epson_encode.py) used by epson_print_conf. This header pulls in
// no platform headers, so it can be included from both usb.h and snmp.h without
// dragging <windows.h> ahead of <winsock2.h>.

namespace ewr {

    // Simple, parameter-free maintenance actions.
    enum class MaintenanceAction
    {
        NozzleCheck,    // Standard nozzle-check pattern (ESC/P2 remote "NC", data 0x00).
        NozzleCheckAlt, // Alternative nozzle-check pattern (NC data 0x10).
    };

    // Nozzle-cleaning group, mirrors epson_print_conf's clean dialog.
    enum class CleanGroup
    {
        All = 0,   // Clean all nozzles
        Black = 1, // Clean the black ink nozzle
        Color = 2, // Clean the color ink nozzles
        Alt = 3,   // Head cleaning (alternative mode)
    };

    // Builds the raw ESC/P2 byte stream for a parameter-free action.
    std::vector<unsigned char> BuildMaintenanceSequence(MaintenanceAction action);

    // Head-cleaning routine (remote "CH"). powerClean requests a deeper cycle that
    // uses much more ink and fills the waste pad faster.
    std::vector<unsigned char> BuildCleanNozzles(CleanGroup group, bool powerClean);

    // One-page colour/quality test pattern (XP-200/205/410 oriented). useBlack23
    // adds the black2/black3 columns where the printer exposes them.
    std::vector<unsigned char> BuildColorTestPattern(bool useBlack23);

    // Advance the loaded sheet forward by 'lines' line feeds without printing.
    std::vector<unsigned char> BuildAdvancePaper(int lines);

    // Pass 'sheets' sheets through the printer without printing.
    std::vector<unsigned char> BuildFeedSheets(int sheets);

} // namespace ewr
