#include "ewr/maintenance.h"
#include <ctime>
#include <string>

namespace ewr {

    // ESC/P2 remote-mode maintenance sequences, ported from Ircama's epson_escp2
    // encoder (epson_escp2/epson_encode.py) used by epson_print_conf. The same
    // bytes are delivered over USB (print channel) or over the network via LPR.
    //
    // Building blocks:
    //   INITIALIZE_PRINTER  1B 40
    //   EXIT_PACKET_MODE    00 00 00 1B 01 "@EJL 1284.4\n@EJL     \n"
    //   REMOTE_MODE         1B 28 52 08 00 00 "REMOTE1"      ESC (R, enter remote mode
    //   ENTER_REMOTE_MODE   ESC @ ESC @ REMOTE_MODE
    //   EXIT_REMOTE_MODE    1B 00 00 00
    //   NC                  4E 43 02 00 00 00 (std) / ...10 (alt)   nozzle check
    //   CH                  43 48 02 00 00 <grp>              head cleaning
    //   LD                  4C 44 00 00                       load defaults marker
    //   JOB_END             4A 45 01 00 00

    namespace {

        void AppendBytes(std::vector<unsigned char>& seq, const unsigned char* data, size_t n)
        {
            seq.insert(seq.end(), data, data + n);
        }

        void AppendStr(std::vector<unsigned char>& seq, const char* s)
        {
            for (; *s; ++s)
                seq.push_back(static_cast<unsigned char>(*s));
        }

        int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Append bytes decoded from a hex string (spaces ignored), 'count' times.
        void AppendHexN(std::vector<unsigned char>& seq, const char* hex, int count)
        {
            std::vector<unsigned char> bytes;
            int hi = -1;
            for (const char* p = hex; *p; ++p)
            {
                if (*p == ' ') continue;
                int v = HexVal(*p);
                if (v < 0) continue;
                if (hi < 0) { hi = v; }
                else { bytes.push_back(static_cast<unsigned char>((hi << 4) | v)); hi = -1; }
            }
            for (int i = 0; i < count; ++i)
                seq.insert(seq.end(), bytes.begin(), bytes.end());
        }

        void AppendHex(std::vector<unsigned char>& seq, const char* hex)
        {
            AppendHexN(seq, hex, 1);
        }

        // ---- Shared control sequences -------------------------------------------

        const unsigned char kExitPacketMode[] = {
            0x00, 0x00, 0x00, 0x1B, 0x01,
            '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4', '.', '4', '\n',
            '@', 'E', 'J', 'L', ' ', ' ', ' ', ' ', ' ', '\n'
        };
        const unsigned char kRemoteMode[]      = { 0x1B, 0x28, 0x52, 0x08, 0x00, 0x00, 'R', 'E', 'M', 'O', 'T', 'E', '1' };
        const unsigned char kEnterRemoteMode[] = { 0x1B, 0x40, 0x1B, 0x40, 0x1B, 0x28, 0x52, 0x08, 0x00, 0x00, 'R', 'E', 'M', 'O', 'T', 'E', '1' };
        const unsigned char kExitRemoteMode[]  = { 0x1B, 0x00, 0x00, 0x00 };
        const unsigned char kJobEnd[]          = { 'J', 'E', 0x01, 0x00, 0x00 };
        const unsigned char kInit[]            = { 0x1B, 0x40 };
        const unsigned char kLoadDefaults[]    = { 'L', 'D', 0x00, 0x00 };

        // remote "TI": sync the printer RTC to the local clock (used before cleaning).
        void AppendSetTimer(std::vector<unsigned char>& seq)
        {
            std::time_t t = std::time(nullptr);
            std::tm* lt = std::localtime(&t);
            int year = lt ? lt->tm_year + 1900 : 2000;
            unsigned char payload[8] = {
                0x00,
                static_cast<unsigned char>((year >> 8) & 0xFF),
                static_cast<unsigned char>(year & 0xFF),
                static_cast<unsigned char>(lt ? lt->tm_mon + 1 : 1),
                static_cast<unsigned char>(lt ? lt->tm_mday : 1),
                static_cast<unsigned char>(lt ? lt->tm_hour : 0),
                static_cast<unsigned char>(lt ? lt->tm_min : 0),
                static_cast<unsigned char>(lt ? lt->tm_sec : 0)
            };
            const unsigned char head[] = { 'T', 'I', 0x08, 0x00 };
            AppendBytes(seq, head, sizeof(head));
            AppendBytes(seq, payload, sizeof(payload));
        }

    } // namespace

    std::vector<unsigned char> BuildMaintenanceSequence(MaintenanceAction action)
    {
        std::vector<unsigned char> seq;

        // NC data byte: 0x00 standard pattern, 0x10 alternative pattern.
        unsigned char ncData = (action == MaintenanceAction::NozzleCheckAlt) ? 0x10 : 0x00;
        const unsigned char nozzle_check[] = { 'N', 'C', 0x02, 0x00, 0x00, ncData };

        AppendBytes(seq, kExitPacketMode, sizeof(kExitPacketMode));
        AppendBytes(seq, kEnterRemoteMode, sizeof(kEnterRemoteMode));
        AppendBytes(seq, nozzle_check, sizeof(nozzle_check));
        AppendBytes(seq, kExitRemoteMode, sizeof(kExitRemoteMode));
        AppendBytes(seq, kJobEnd, sizeof(kJobEnd));
        return seq;
    }

    std::vector<unsigned char> BuildCleanNozzles(CleanGroup group, bool powerClean)
    {
        std::vector<unsigned char> seq;

        if (group == CleanGroup::Alt)
        {
            // Alternative-mode head cleaning uses a fixed control sequence; power
            // clean issues the deeper "ink charge" variant (last byte 0x0A vs 0x02).
            AppendBytes(seq, kInit, sizeof(kInit));
            AppendHex(seq, powerClean ? "1B 7C 00 06 00 19 07 84 7B 42 0A"
                                      : "1B 7C 00 06 00 19 07 84 7B 42 02");
            return seq;
        }

        unsigned char grp = static_cast<unsigned char>(group); // 0=all, 1=black, 2=color
        if (powerClean)
            grp |= 0x10;

        const unsigned char ch[] = { 'C', 'H', 0x02, 0x00, 0x00, grp };

        AppendBytes(seq, kExitPacketMode, sizeof(kExitPacketMode));
        AppendBytes(seq, kEnterRemoteMode, sizeof(kEnterRemoteMode));
        AppendSetTimer(seq);
        AppendBytes(seq, ch, sizeof(ch));
        AppendBytes(seq, kExitRemoteMode, sizeof(kExitRemoteMode));
        AppendBytes(seq, kEnterRemoteMode, sizeof(kEnterRemoteMode));
        AppendBytes(seq, kJobEnd, sizeof(kJobEnd));
        AppendBytes(seq, kExitRemoteMode, sizeof(kExitRemoteMode));
        return seq;
    }

    std::vector<unsigned char> BuildAdvancePaper(int lines)
    {
        if (lines < 1) lines = 1;
        std::vector<unsigned char> seq;
        AppendHexN(seq, "0D 0A", lines); // CR LF per line
        return seq;
    }

    std::vector<unsigned char> BuildFeedSheets(int sheets)
    {
        if (sheets < 1) sheets = 1;
        std::vector<unsigned char> seq;
        // ESC @ CR LF FF ESC @ per sheet.
        AppendHexN(seq, "1B 40 0D 0A 0C 1B 40", sheets);
        return seq;
    }

    std::vector<unsigned char> BuildColorTestPattern(bool useBlack23)
    {
        // Transfer Raster image headers (ESC i) for each colour plane.
        const char* TRI_BLACK   = "1b6900010250008000";
        const char* TRI_MAGENTA = "1b6901010250002a00";
        const char* TRI_YELLOW  = "1b6904010250002a00";
        const char* TRI_CYAN    = "1b6902010250002a00";
        const char* TRI_BLACK2  = "1b6905010250002a00";
        const char* TRI_BLACK3  = "1b6906010250002a00";

        const char* SET_H_POS = "1b28240400";
        const char* SET_V_POS = "1b28760400";
        const char* USE_MONOCHROME = "1b284b02000001";
        const char* USE_COLOR      = "1b284b02000000";

        // 2 bits per pixel run-length patterns.
        const char* P_LARGE  = "d9ff";
        const char* P_MEDIUM = "d9aa";
        const char* P_SMALL  = "d955";
        const char* NO_DOTS  = "d900d900";
        const char* LARGE_ALT  = "d9ffd900d900d9ff";
        const char* MEDIUM_ALT = "d9aad900d900d9aa";
        const char* SMALL_ALT  = "d955d900d900d955";
        (void)NO_DOTS;

        struct Segment
        {
            const char* label;   // ASCII label text
            const char* prefix;  // control bytes before the label ("" = INIT, "X" = EXIT_REMOTE_MODE)
            const char* vsd;     // ink drop size code
            const char* alt;     // alternating pattern
            const char* solid;   // solid pattern
        };

        // prefix "X" -> EXIT_REMOTE_MODE, "@" -> INITIALIZE_PRINTER.
        const Segment segments[] = {
            { "\r\n\r\nEconomy\r\n",                            "X", "10", LARGE_ALT,  P_LARGE  },
            { "\r\n\n\n\nVSD1 - Medium dot size - Normal\r\n",  "@", "11", MEDIUM_ALT, P_MEDIUM },
            { "\r\n\n\n\nVSD2 - Medium dot size - Fine\r\n",    "@", "12", MEDIUM_ALT, P_MEDIUM },
            { "\r\n\n\n\nVSD3 - Large dot size - Super Fine\r\n", "@", "13", LARGE_ALT,  P_LARGE  },
            { "\r\n\n\n\nVSD3 - Medium dot size - Super Fine\r\n", "@", "13", MEDIUM_ALT, P_MEDIUM },
            { "\r\n\n\n\nVSD3 - Small dot size - Super Fine\r\n",  "@", "13", SMALL_ALT,  P_SMALL  },
        };

        std::vector<unsigned char> body;

        for (const auto& s : segments)
        {
            // Label
            if (s.prefix[0] == 'X')
                AppendBytes(body, kExitRemoteMode, sizeof(kExitRemoteMode));
            else
                AppendBytes(body, kInit, sizeof(kInit));
            AppendStr(body, s.label);

            // Per-segment initialization
            AppendHex(body, "1b2847010001");             // select graphics mode
            AppendHex(body, "1b28550500010101a005");     // 360 DPI resolution
            AppendHex(body, "1b28430400c6410000");       // page length 29.7cm
            AppendHex(body, "1b28630800ffffffffc6410000"); // page format
            AppendHex(body, "1b28530800822e0000c6410000"); // paper dimension A4
            AppendHex(body, "1b28440400");               // raster image resolution
            AppendHex(body, "68010301");
            AppendHex(body, "1b2865020000");             // select ink drop size
            AppendHex(body, s.vsd);
            AppendHex(body, "1b5502");                   // automatic print direction
            AppendHex(body, USE_MONOCHROME);
            AppendHex(body, SET_V_POS); AppendHex(body, "00010000");

            // First block: black alternating
            AppendHex(body, SET_H_POS); AppendHex(body, "00010000");
            AppendHex(body, TRI_BLACK);
            AppendHexN(body, s.alt, 64);

            // Second block: magenta/yellow/cyan alternating
            AppendHex(body, USE_COLOR); AppendHex(body, SET_H_POS); AppendHex(body, "80060000");
            AppendHex(body, TRI_MAGENTA); AppendHexN(body, s.alt, 21);
            AppendHex(body, SET_H_POS); AppendHex(body, "80060000");
            AppendHex(body, TRI_YELLOW); AppendHexN(body, s.alt, 21);
            AppendHex(body, SET_H_POS); AppendHex(body, "80060000");
            AppendHex(body, TRI_CYAN); AppendHexN(body, s.alt, 21);

            // Third block: black solid
            AppendHex(body, USE_MONOCHROME); AppendHex(body, SET_H_POS); AppendHex(body, "000c0000");
            AppendHex(body, TRI_BLACK); AppendHexN(body, s.solid, 256);

            // Fourth block: magenta/yellow/cyan solid
            AppendHex(body, USE_COLOR); AppendHex(body, SET_H_POS); AppendHex(body, "80110000");
            AppendHex(body, TRI_MAGENTA); AppendHexN(body, s.solid, 84);
            AppendHex(body, SET_H_POS); AppendHex(body, "80110000");
            AppendHex(body, TRI_YELLOW); AppendHexN(body, s.solid, 84);
            AppendHex(body, SET_H_POS); AppendHex(body, "80110000");
            AppendHex(body, TRI_CYAN); AppendHexN(body, s.solid, 84);

            // Fifth block: black/black2/black3 solid (optional)
            if (useBlack23)
            {
                AppendHex(body, USE_COLOR); AppendHex(body, SET_H_POS); AppendHex(body, "00170000");
                AppendHex(body, TRI_BLACK); AppendHexN(body, s.solid, 84);
                AppendHex(body, SET_H_POS); AppendHex(body, "00170000");
                AppendHex(body, TRI_BLACK2); AppendHexN(body, s.solid, 84);
                AppendHex(body, SET_H_POS); AppendHex(body, "00170000");
                AppendHex(body, TRI_BLACK3); AppendHexN(body, s.solid, 84);
            }

            AppendHex(body, SET_V_POS); AppendHex(body, "00030000");
        }

        AppendBytes(body, kInit, sizeof(kInit));
        AppendStr(body, "\r\n\n\n\nEpson Printer Configuration - Print Test Patterns\r\n");

        // Wrap the body in the job framing.
        std::vector<unsigned char> seq;
        AppendBytes(seq, kInit, sizeof(kInit));
        AppendBytes(seq, kRemoteMode, sizeof(kRemoteMode));
        AppendHex(seq, "4e4302000000"); // PRINT_NOZZLE_CHECK

        seq.insert(seq.end(), body.begin(), body.end());

        AppendBytes(seq, kInit, sizeof(kInit));
        seq.push_back('\r');
        seq.push_back(0x0C); // FF
        AppendBytes(seq, kInit, sizeof(kInit));
        AppendBytes(seq, kRemoteMode, sizeof(kRemoteMode));
        AppendBytes(seq, kLoadDefaults, sizeof(kLoadDefaults));
        AppendBytes(seq, kExitRemoteMode, sizeof(kExitRemoteMode));
        AppendBytes(seq, kInit, sizeof(kInit));
        AppendBytes(seq, kRemoteMode, sizeof(kRemoteMode));
        AppendBytes(seq, kLoadDefaults, sizeof(kLoadDefaults));
        AppendBytes(seq, kJobEnd, sizeof(kJobEnd));
        AppendBytes(seq, kExitRemoteMode, sizeof(kExitRemoteMode));
        return seq;
    }

} // namespace ewr
