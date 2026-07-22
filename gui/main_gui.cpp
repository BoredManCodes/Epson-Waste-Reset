// TPW Epson Tool - Native Win32 GUI front-end for the ewr_core library.
//
// Two reset transports share one window:
//   * USB  - the direct IEEE 1284.4 EEPROM path (generator + usb_windows).
//   * LAN  - SNMP EEPROM writes over the network (snmp_windows).
//
// Core library progress output (std::cout / std::cerr) is captured via a custom
// streambuf and mirrored into the log pane, so the core needs no changes.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Vista+, for TaskDialogIndirect (About dialog)
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "ewr/generator.h"
#include "ewr/parser.h"
#include "ewr/payload.h"
#include "ewr/snmp.h"
#include "ewr/usb.h"

namespace {

    constexpr UINT WM_APP_LOG           = WM_APP + 1; // lParam = std::string* (owned)
    constexpr UINT WM_APP_INIT_DONE     = WM_APP + 2; // lParam = std::vector<MenuOption>* (owned)
    constexpr UINT WM_APP_RESET_DONE    = WM_APP + 3; // wParam = 1 on success
    constexpr UINT WM_APP_DISCOVER_DONE = WM_APP + 4; // lParam = std::vector<LanPrinter>* (owned)

    constexpr int IDC_SEARCH     = 101;
    constexpr int IDC_LIST       = 102;
    constexpr int IDC_RESET      = 103;
    constexpr int IDC_KILL       = 104;
    constexpr int IDC_KILL_FIRST = 105;
    constexpr int IDC_MODE_USB   = 106;
    constexpr int IDC_MODE_LAN   = 107;
    constexpr int IDC_IP         = 108;
    constexpr int IDC_DETECT     = 109;
    constexpr int IDM_ABOUT      = 110;

    enum class Mode { Usb, Lan };

    struct MenuOption
    {
        std::string displayName;
        bool isReplay;
        ewr::PrinterModel replayModel;
        ewr::DbPrinterModel smartModel;
    };

    HWND g_hwndMain = nullptr;
    HWND g_hwndSearchLabel = nullptr;
    HWND g_hwndSearch = nullptr;
    HWND g_hwndModeUsb = nullptr;
    HWND g_hwndModeLan = nullptr;
    HWND g_hwndIpLabel = nullptr;
    HWND g_hwndIp = nullptr;
    HWND g_hwndDetect = nullptr;
    HWND g_hwndList = nullptr;
    HWND g_hwndLog = nullptr;
    HWND g_hwndReset = nullptr;
    HWND g_hwndKill = nullptr;
    HWND g_hwndKillFirst = nullptr;
    HWND g_hwndStatus = nullptr;

    HFONT g_uiFont = nullptr;
    HFONT g_logFont = nullptr;

    ewr::UniversalGenerator g_generator;
    std::vector<MenuOption> g_options;   // USB models
    std::vector<ewr::LanModel> g_lanDb;  // LAN models
    std::vector<size_t> g_filtered;      // listbox row -> index into the active source
    std::atomic<bool> g_busy{ false };
    Mode g_mode = Mode::Usb;

    std::string ToLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    // Redirects std::cout / std::cerr line-by-line into the log pane. Worker
    // threads write freely; completed lines are posted to the UI thread.
    class GuiLogBuf : public std::streambuf
    {
    public:
        int_type overflow(int_type ch) override
        {
            if (ch == traits_type::eof())
                return 0;

            std::lock_guard<std::mutex> lock(mutex);
            if (ch == '\n')
                FlushLine();
            else
                line.push_back(static_cast<char>(ch));
            return ch;
        }

        std::streamsize xsputn(const char* s, std::streamsize n) override
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::streamsize i = 0; i < n; ++i)
            {
                if (s[i] == '\n')
                    FlushLine();
                else
                    line.push_back(s[i]);
            }
            return n;
        }

    private:
        void FlushLine()
        {
            PostMessage(g_hwndMain, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(new std::string(line)));
            line.clear();
        }

        std::string line;
        std::mutex mutex;
    };

    GuiLogBuf g_logBuf;

    void AppendLogLine(const std::string& lineText)
    {
        int len = GetWindowTextLengthA(g_hwndLog);
        SendMessageA(g_hwndLog, EM_SETSEL, len, len);
        std::string text = lineText + "\r\n";
        SendMessageA(g_hwndLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    }

    void SetStatus(const std::string& text)
    {
        SendMessageA(g_hwndStatus, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }

    std::string GetEditText(HWND edit)
    {
        int len = GetWindowTextLengthA(edit);
        std::string buf(len, '\0');
        GetWindowTextA(edit, buf.data(), len + 1);
        return buf;
    }

    size_t ActiveSourceSize()
    {
        return g_mode == Mode::Usb ? g_options.size() : g_lanDb.size();
    }

    std::string SourceDisplayName(size_t index)
    {
        return g_mode == Mode::Usb ? g_options[index].displayName : g_lanDb[index].name;
    }

    void RefreshList()
    {
        std::string query = ToLower(GetEditText(g_hwndSearch));

        SendMessage(g_hwndList, WM_SETREDRAW, FALSE, 0);
        SendMessage(g_hwndList, LB_RESETCONTENT, 0, 0);
        g_filtered.clear();

        for (size_t i = 0; i < ActiveSourceSize(); ++i)
        {
            std::string name = SourceDisplayName(i);
            if (query.empty() || ToLower(name).find(query) != std::string::npos)
            {
                SendMessageA(g_hwndList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                g_filtered.push_back(i);
            }
        }

        SendMessage(g_hwndList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hwndList, nullptr, TRUE);

        std::ostringstream oss;
        oss << "Showing " << g_filtered.size() << " of " << ActiveSourceSize() << " "
            << (g_mode == Mode::Usb ? "USB" : "network") << " models.";
        SetStatus(oss.str());
    }

    // Enable/disable transport-specific controls for the current mode and busy state.
    void UpdateModeControls()
    {
        bool busy = g_busy;
        bool lan = (g_mode == Mode::Lan);

        EnableWindow(g_hwndIpLabel, lan && !busy);
        EnableWindow(g_hwndIp, lan && !busy);
        EnableWindow(g_hwndDetect, lan && !busy);
        EnableWindow(g_hwndKill, !lan && !busy);
        EnableWindow(g_hwndKillFirst, !lan && !busy);

        EnableWindow(g_hwndReset, !busy);
        EnableWindow(g_hwndList, !busy);
        EnableWindow(g_hwndSearch, !busy);
        EnableWindow(g_hwndModeUsb, !busy);
        EnableWindow(g_hwndModeLan, !busy);
    }

    void SetBusy(bool busy)
    {
        g_busy = busy;
        UpdateModeControls();
    }

    void SwitchMode(Mode mode)
    {
        if (g_mode == mode)
            return;
        g_mode = mode;
        UpdateModeControls();
        RefreshList();
    }

    // Select the list row whose LAN model matches 'modelPtr', if visible.
    void SelectLanModel(const ewr::LanModel* modelPtr)
    {
        if (!modelPtr || g_lanDb.empty())
            return;
        size_t target = static_cast<size_t>(modelPtr - g_lanDb.data());
        for (size_t row = 0; row < g_filtered.size(); ++row)
        {
            if (g_filtered[row] == target)
            {
                SendMessage(g_hwndList, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
                return;
            }
        }
    }

    // ---- Worker threads -------------------------------------------------------

    void InitWorker()
    {
        std::cout << "[i] Checking for OTA database updates..." << std::endl;

        if (g_generator.SyncDatabaseOTA())
            std::cout << "[i] Database sync: SUCCESS." << std::endl;
        else
            std::cout << "[i] Database sync: OFFLINE (using local cache)." << std::endl;

        g_generator.LoadDatabase("database.json");
        auto replayModels = ewr::ScanModelsFolder("models");
        auto smartModels = g_generator.GetAvailableModels();

        std::cout << "[i] Loaded " << smartModels.size() << " Smart Protocol payloads." << std::endl;
        std::cout << "[i] Loaded " << replayModels.size() << " Custom payloads." << std::endl;

        if (ewr::LoadLanDatabase("lan_database.json", g_lanDb))
            std::cout << "[i] Loaded " << g_lanDb.size() << " network (SNMP) models." << std::endl;
        else
            std::cout << "[i] No network model database found (lan_database.json missing)." << std::endl;

        auto* options = new std::vector<MenuOption>();

        for (const auto& sm : smartModels)
            options->push_back({ sm.name + " (Smart Protocol)", false, {}, sm });

        for (const auto& lm : replayModels)
            options->push_back({ lm.name + " (Replay)", true, lm, {} });

        std::sort(options->begin(), options->end(), [](const MenuOption& a, const MenuOption& b)
            {
                return a.displayName < b.displayName;
            });

        PostMessage(g_hwndMain, WM_APP_INIT_DONE, 0, reinterpret_cast<LPARAM>(options));
    }

    void UsbResetWorker(MenuOption selected, bool killFirst)
    {
        if (killFirst)
        {
            auto processes = ewr::ListEpsonProcesses();
            if (!processes.empty())
            {
                std::cout << "[i] Closing " << processes.size() << " Epson background process(es)..." << std::endl;
                int killed = ewr::KillEpsonProcesses();
                std::cout << "[i] Closed " << killed << " of " << processes.size() << " process(es)." << std::endl;
            }
        }

        std::vector<std::vector<unsigned char>> executionSequence;

        if (selected.isReplay)
        {
            std::cout << "\n[!] Parsing replay Wireshark dump for " << selected.displayName << "..." << std::endl;
            executionSequence = ewr::ParseWiresharkDump(selected.replayModel.filepath);
        }
        else
        {
            std::cout << "\n[*] Generating safe Smart Protocol R/W sequence for " << selected.displayName << "..." << std::endl;
            executionSequence = g_generator.GenerateSequence(selected.smartModel);
        }

        if (executionSequence.empty())
        {
            std::cerr << "[-] Failed to construct payload." << std::endl;
            PostMessage(g_hwndMain, WM_APP_RESET_DONE, 0, 0);
            return;
        }

        std::cout << "Scanning USB ports for Epson device..." << std::endl;
        ewr::EwrDeviceHandle hPrinter = ewr::AutoConnectEpsonPrinter();

        if (!hPrinter)
        {
            std::cerr << "[ERROR] Could not find an Epson printer, or the connection was refused." << std::endl;
            std::cerr << "[!] Check the USB cable and printer power, close Epson background processes" << std::endl;
            std::cerr << "    (button below), then press Reset Waste Counter to try again." << std::endl;
            PostMessage(g_hwndMain, WM_APP_RESET_DONE, 0, 0);
            return;
        }

        bool success = ewr::ExecutePayloadSequence(hPrinter, executionSequence);
        ewr::DisconnectPrinter(hPrinter);

        PostMessage(g_hwndMain, WM_APP_RESET_DONE, success ? 1 : 0, 0);
    }

    void LanResetWorker(ewr::LanModel model, std::string host)
    {
        bool success = ewr::SnmpWasteReset(host, model);
        PostMessage(g_hwndMain, WM_APP_RESET_DONE, success ? 1 : 0, 0);
    }

    void DiscoverWorker()
    {
        auto* printers = new std::vector<ewr::LanPrinter>(ewr::DiscoverLanPrinters());
        PostMessage(g_hwndMain, WM_APP_DISCOVER_DONE, 0, reinterpret_cast<LPARAM>(printers));
    }

    // ---- UI actions -----------------------------------------------------------

    int SelectedRow()
    {
        int sel = static_cast<int>(SendMessage(g_hwndList, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR || static_cast<size_t>(sel) >= g_filtered.size())
            return -1;
        return sel;
    }

    void StartUsbReset(int row)
    {
        MenuOption selected = g_options[g_filtered[row]];

        std::string msg = "Send the waste counter reset sequence over USB to:\n\n    " + selected.displayName +
            "\n\nMake sure the printer is powered on and connected via USB.";
        if (MessageBoxA(g_hwndMain, msg.c_str(), "Confirm USB Reset", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;

        bool killFirst = SendMessage(g_hwndKillFirst, BM_GETCHECK, 0, 0) == BST_CHECKED;

        SetBusy(true);
        SetStatus("Resetting " + selected.displayName + " over USB...");
        std::thread(UsbResetWorker, selected, killFirst).detach();
    }

    void StartLanReset(int row)
    {
        ewr::LanModel model = g_lanDb[g_filtered[row]];

        std::string host = GetEditText(g_hwndIp);
        host.erase(std::remove_if(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c); }), host.end());
        if (host.empty())
        {
            MessageBoxA(g_hwndMain, "Enter the printer's IP address, or use Detect Printers to find it.",
                "TPW Epson Tool", MB_OK | MB_ICONINFORMATION);
            return;
        }

        std::string msg = "Permanently reset the waste ink counter over the network:\n\n    Model:  " +
            model.name + "\n    IP:     " + host +
            "\n\nThis writes directly to the printer's EEPROM via SNMP. Make sure the model "
            "matches your printer; a wrong match can misconfigure it.\n\nProceed?";
        if (MessageBoxA(g_hwndMain, msg.c_str(), "Confirm Network Reset", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return;

        SetBusy(true);
        SetStatus("Resetting " + model.name + " at " + host + " over the network...");
        std::thread(LanResetWorker, model, host).detach();
    }

    void StartReset()
    {
        if (g_busy)
            return;

        int row = SelectedRow();
        if (row < 0)
        {
            MessageBoxA(g_hwndMain, "Select your printer model from the list first.", "TPW Epson Tool", MB_OK | MB_ICONINFORMATION);
            return;
        }

        if (g_mode == Mode::Usb)
            StartUsbReset(row);
        else
            StartLanReset(row);
    }

    void StartDetect()
    {
        if (g_busy)
            return;
        SetBusy(true);
        SetStatus("Scanning the local network for Epson printers...");
        std::cout << "\n[*] Detecting network printers (this can take a few seconds)..." << std::endl;
        std::thread(DiscoverWorker).detach();
    }

    void OnKillProcesses()
    {
        auto processes = ewr::ListEpsonProcesses();

        if (processes.empty())
        {
            std::cout << "[i] No running Epson background processes were found." << std::endl;
            return;
        }

        std::ostringstream oss;
        oss << "Found " << processes.size() << " Epson-related process(es) that may hold the printer port open:\n\n";
        for (const auto& p : processes)
            oss << "    " << p.name << " (PID " << p.pid << ")\n";
        oss << "\nClose them now?";

        if (MessageBoxA(g_hwndMain, oss.str().c_str(), "Epson Process Cleanup", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            int killed = ewr::KillEpsonProcesses();
            std::cout << "[i] Closed " << killed << " of " << processes.size() << " Epson process(es)." << std::endl;
        }
    }

    // ---- Layout / creation ----------------------------------------------------

    void LayoutControls(int w, int h)
    {
        SendMessage(g_hwndStatus, WM_SIZE, 0, 0);

        RECT sbRect = { 0 };
        GetWindowRect(g_hwndStatus, &sbRect);
        int statusH = sbRect.bottom - sbRect.top;

        const int m = 10;
        const int rowH = 24;
        const int labelW = 96;
        const int btnH = 30;
        const int btnW = 170;

        int y0 = m;                       // search row
        int y1 = y0 + rowH + 6;           // mode / ip row
        int listTop = y1 + rowH + 8;

        int bottomRowTop = h - statusH - m - btnH;
        int listH = bottomRowTop - m - listTop;
        int listW = (w - 3 * m) * 2 / 5;

        MoveWindow(g_hwndSearchLabel, m, y0 + 4, labelW, 18, TRUE);
        MoveWindow(g_hwndSearch, m + labelW, y0, w - 2 * m - labelW, rowH, TRUE);

        MoveWindow(g_hwndModeUsb, m, y1 + 2, 60, 20, TRUE);
        MoveWindow(g_hwndModeLan, m + 66, y1 + 2, 150, 20, TRUE);
        MoveWindow(g_hwndIpLabel, m + 226, y1 + 4, 26, 18, TRUE);
        MoveWindow(g_hwndIp, m + 254, y1, 170, rowH, TRUE);
        MoveWindow(g_hwndDetect, m + 434, y1 - 1, 140, rowH + 2, TRUE);

        MoveWindow(g_hwndList, m, listTop, listW, listH, TRUE);
        MoveWindow(g_hwndLog, 2 * m + listW, listTop, w - 3 * m - listW, listH, TRUE);

        MoveWindow(g_hwndKillFirst, m, bottomRowTop + 6, 280, 20, TRUE);
        MoveWindow(g_hwndKill, w - m - 2 * btnW - 8, bottomRowTop, btnW, btnH, TRUE);
        MoveWindow(g_hwndReset, w - m - btnW, bottomRowTop, btnW, btnH, TRUE);
    }

    void CreateControls(HWND hwnd)
    {
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtr(hwnd, GWLP_HINSTANCE));

        g_hwndSearchLabel = CreateWindowExA(0, "STATIC", "Model search:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

        g_hwndSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)), hInst, nullptr);

        g_hwndModeUsb = CreateWindowExA(0, "BUTTON", "USB",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODE_USB)), hInst, nullptr);
        SendMessage(g_hwndModeUsb, BM_SETCHECK, BST_CHECKED, 0);

        g_hwndModeLan = CreateWindowExA(0, "BUTTON", "LAN / Wi-Fi (SNMP)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODE_LAN)), hInst, nullptr);

        g_hwndIpLabel = CreateWindowExA(0, "STATIC", "IP:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

        g_hwndIp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_IP)), hInst, nullptr);

        g_hwndDetect = CreateWindowExA(0, "BUTTON", "Detect Printers",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DETECT)), hInst, nullptr);

        g_hwndList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);

        g_hwndLog = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);
        SendMessage(g_hwndLog, EM_SETLIMITTEXT, 0, 0);

        g_hwndKillFirst = CreateWindowExA(0, "BUTTON", "Close Epson background processes first",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_KILL_FIRST)), hInst, nullptr);
        SendMessage(g_hwndKillFirst, BM_SETCHECK, BST_CHECKED, 0);

        g_hwndKill = CreateWindowExA(0, "BUTTON", "Close Epson Processes",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_KILL)), hInst, nullptr);

        g_hwndReset = CreateWindowExA(0, "BUTTON", "Reset Waste Counter",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESET)), hInst, nullptr);

        g_hwndStatus = CreateWindowExA(0, STATUSCLASSNAMEA, "",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

        g_uiFont = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        g_logFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

        HWND uiControls[] = { g_hwndSearchLabel, g_hwndSearch, g_hwndModeUsb, g_hwndModeLan,
            g_hwndIpLabel, g_hwndIp, g_hwndDetect, g_hwndList, g_hwndKillFirst,
            g_hwndKill, g_hwndReset, g_hwndStatus };
        for (HWND ctrl : uiControls)
            SendMessage(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

        SendMessage(g_hwndLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_logFont), TRUE);
    }

    // Opens clicked hyperlinks in the About dialog with the default browser.
    HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR)
    {
        if (msg == TDN_HYPERLINK_CLICKED)
        {
            const wchar_t* url = reinterpret_cast<const wchar_t*>(lParam);
            ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    }

    void ShowAboutDialog(HWND owner)
    {
        TASKDIALOGCONFIG cfg = { sizeof(cfg) };
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        cfg.dwCommonButtons = TDCBF_OK_BUTTON;
        cfg.pszWindowTitle = L"About TPW Epson Tool";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"TPW Epson Tool";
        cfg.pszContent =
            L"Written by Trent Buckley.\n\n"
            L"Resets the Epson waste ink pad counter over USB or over the network (SNMP), "
            L"so a printer locked up on a full waste pad can keep working.\n\n"
            L"This builds on two open-source projects:\n\n"
            L"• <a href=\"https://github.com/RxNaison/Epson-Waste-Reset\">EWR (Epson Waste Reset)</a>: the USB reset engine.\n"
            L"• <a href=\"https://github.com/Ircama/epson_print_conf\">epson_print_conf</a> by Ircama: the LAN / SNMP reset method and printer database.\n\n"
            L"Source code: <a href=\"https://github.com/BoredManCodes/Epson-Waste-Reset\">github.com/BoredManCodes/Epson-Waste-Reset</a>";
        cfg.pfCallback = AboutCallback;
        TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
    }

    void CreateMenuBar(HWND hwnd)
    {
        HMENU menu = CreateMenu();
        AppendMenuA(menu, MF_STRING, IDM_ABOUT, "&About");
        SetMenu(hwnd, menu);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
            g_hwndMain = hwnd;
            CreateMenuBar(hwnd);
            CreateControls(hwnd);
            return 0;

        case WM_SIZE:
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 720;
            mmi->ptMinTrackSize.y = 460;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_SEARCH:
                if (HIWORD(wParam) == EN_CHANGE)
                    RefreshList();
                return 0;
            case IDC_LIST:
                if (HIWORD(wParam) == LBN_DBLCLK)
                    StartReset();
                return 0;
            case IDC_RESET:
                if (HIWORD(wParam) == BN_CLICKED)
                    StartReset();
                return 0;
            case IDC_KILL:
                if (HIWORD(wParam) == BN_CLICKED)
                    OnKillProcesses();
                return 0;
            case IDC_DETECT:
                if (HIWORD(wParam) == BN_CLICKED)
                    StartDetect();
                return 0;
            case IDC_MODE_USB:
                if (HIWORD(wParam) == BN_CLICKED)
                    SwitchMode(Mode::Usb);
                return 0;
            case IDC_MODE_LAN:
                if (HIWORD(wParam) == BN_CLICKED)
                    SwitchMode(Mode::Lan);
                return 0;
            case IDM_ABOUT:
                ShowAboutDialog(hwnd);
                return 0;
            }
            break;

        case WM_APP_LOG:
        {
            std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lParam));
            AppendLogLine(*line);
            return 0;
        }

        case WM_APP_INIT_DONE:
        {
            std::unique_ptr<std::vector<MenuOption>> options(reinterpret_cast<std::vector<MenuOption>*>(lParam));
            g_options = std::move(*options);
            RefreshList();
            SetBusy(false);

            if (g_options.empty() && g_lanDb.empty())
            {
                SetStatus("No payloads found.");
                MessageBoxA(hwnd,
                    "No payloads found.\n\nTPW Epson Tool needs internet access on first run to download the "
                    "printer databases, or local database files next to the executable.",
                    "TPW Epson Tool", MB_OK | MB_ICONERROR);
            }
            return 0;
        }

        case WM_APP_DISCOVER_DONE:
        {
            std::unique_ptr<std::vector<ewr::LanPrinter>> printers(
                reinterpret_cast<std::vector<ewr::LanPrinter>*>(lParam));
            SetBusy(false);

            if (printers->empty())
            {
                SetStatus("No network printers found.");
                MessageBoxA(hwnd,
                    "No Epson printers answered on the local network.\n\n"
                    "Check that the printer is powered on, connected to the same network, "
                    "and that SNMP is enabled. You can also type the IP address in manually.",
                    "TPW Epson Tool - Detect Printers", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            const ewr::LanPrinter& first = printers->front();
            SetWindowTextA(g_hwndIp, first.ip.c_str());

            const ewr::LanModel* match = ewr::MatchLanModel(g_lanDb, first.modelName);
            if (match)
            {
                SetWindowTextA(g_hwndSearch, "");
                RefreshList();
                SelectLanModel(match);
                SetStatus("Found " + first.modelName + " at " + first.ip + " (" + match->name + ").");
            }
            else
            {
                SetStatus("Found " + first.modelName + " at " + first.ip + " (no DB match; pick a model).");
                std::cout << "[!] '" << first.modelName << "' is not in the network database. "
                          << "Select the closest matching model manually." << std::endl;
            }
            return 0;
        }

        case WM_APP_RESET_DONE:
            SetBusy(false);
            if (wParam == 1)
            {
                SetStatus("Reset complete. Power-cycle the printer.");
                MessageBoxA(hwnd,
                    "SUCCESS!\n\nTurn the printer OFF, then ON using its physical power button "
                    "to commit the changes.",
                    "TPW Epson Tool - Reset Complete", MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                SetStatus("Reset failed. See the log for details.");
            }
            return 0;

        case WM_CLOSE:
            if (g_busy)
            {
                int r = MessageBoxA(hwnd,
                    "An operation is still running. Closing now may leave the printer "
                    "mid-sequence.\n\nClose anyway?",
                    "TPW Epson Tool", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (r != IDYES)
                    return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void SetWorkingDirToExeDir()
    {
        char exePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH))
        {
            std::string dir(exePath);
            size_t pos = dir.find_last_of("\\/");
            if (pos != std::string::npos)
                SetCurrentDirectoryA(dir.substr(0, pos).c_str());
        }
    }

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // database.json, lan_database.json and models/ live next to the executable;
    // the CWD after an elevated relaunch is not guaranteed to be the exe dir.
    SetWorkingDirToExeDir();

    if (!ewr::IsRunningElevated())
    {
        int r = MessageBoxA(nullptr,
            "TPW Epson Tool is not running as Administrator.\n\n"
            "Direct USB access to the printer's maintenance interface usually requires "
            "elevated privileges on Windows. Network (SNMP) resets do not need "
            "administrator rights.\n\n"
            "Relaunch EWR as Administrator now?",
            "TPW Epson Tool - Administrator Rights", MB_YESNO | MB_ICONWARNING);

        if (r == IDYES && ewr::RelaunchElevated(0, nullptr))
            return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "EwrMainWindow";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "EwrMainWindow", "TPW Epson Tool",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
        return 1;

    // Route core library output into the log pane. Installed after the window
    // exists so posted log lines always have a valid target.
    std::cout.rdbuf(&g_logBuf);
    std::cerr.rdbuf(&g_logBuf);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetFocus(g_hwndSearch);

    SetBusy(true);
    SetStatus("Syncing printer database...");
    std::thread(InitWorker).detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessage(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}
