#pragma once
#include "ewr/maintenance.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Native LAN / Wi-Fi waste-ink reset over SNMP (Epson EPSON-CTRL EEPROM writes).
//
// This is the network counterpart to the USB path in usb.h. It speaks SNMP v1
// (community "public", UDP/161) directly, building the exact EEPROM-write OIDs
// documented by the epson_print_conf project. Progress is reported via
// std::cout / std::cerr so it flows into the GUI log pane and any CLI use,
// matching the convention in usb_windows.cpp.

namespace ewr {

    // One resettable model from lan_database.json.
    struct LanModel
    {
        std::string name;
        uint8_t read_key[2] = { 0, 0 };
        std::vector<uint8_t> write_key;
        // Ordered EEPROM write sequence: (address, value) pairs.
        std::vector<std::pair<uint16_t, uint8_t>> reset;
    };

    // A printer discovered on the local network.
    struct LanPrinter
    {
        std::string ip;
        std::string modelName; // SNMP-reported model string, e.g. "EPSON XP-205 207 Series"
    };

    // Load the LAN model database (JSON). Returns false if the file is missing
    // or malformed; 'out' holds the parsed models on success.
    bool LoadLanDatabase(const std::string& filepath, std::vector<LanModel>& out);

    // Match an SNMP-reported model string (e.g. "EPSON XP-205 207 Series") to a
    // database entry. Returns nullptr if no confident match is found.
    const LanModel* MatchLanModel(const std::vector<LanModel>& db, const std::string& reportedModel);

    // Query a single host's SNMP "Model" OID. Returns "" on timeout/error.
    std::string SnmpGetModel(const std::string& host, int timeoutMs = 1000);

    // Scan local IPv4 subnets for Epson printers that answer the model OID.
    std::vector<LanPrinter> DiscoverLanPrinters();

    // Replay the model's waste-reset write sequence over SNMP. Returns true only
    // if every EEPROM write is acknowledged with ":OK;".
    bool SnmpWasteReset(const std::string& host, const LanModel& model);

    // Send a raw ESC/P2 print job (a maintenance sequence built in maintenance.h)
    // to a network printer over LPR (RFC 1179, TCP 515), matching how
    // epson_print_conf delivers these. This is NOT SNMP: a maintenance job is print
    // data, not an EEPROM write, so it does not travel on the EPSON-CTRL OID
    // channel, and needs no model data. 'label' names the LPR job. Returns true if
    // the printer acknowledged the job.
    bool LanSendPrintJob(const std::string& host, const std::vector<unsigned char>& job, const std::string& label);

} // namespace ewr
