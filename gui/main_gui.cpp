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
#include <cmath>
#include <filesystem>
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
    constexpr UINT WM_APP_STAGE         = WM_APP + 5; // lParam = std::string* (owned) - overlay title
    constexpr UINT WM_APP_MAINT_DONE    = WM_APP + 6; // wParam = 1 on success (nozzle/feed)

    constexpr UINT_PTR OVERLAY_TIMER = 1;

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
    constexpr int IDC_ELEVATE    = 111;
    constexpr int IDC_CLEAN      = 112; // "Clean Print Head..." button
    constexpr int IDC_TESTS      = 113; // "Test Patterns..." button

    constexpr int IDI_APPICON     = 101; // TPW logo icon (gui/ewr-gui.rc)

    // Dialog resource IDs (must match gui/ewr-gui.rc).
    constexpr int IDD_CLEAN       = 200;
    constexpr int IDC_CLEAN_GROUP = 201;
    constexpr int IDC_CLEAN_POWER = 202;
    constexpr int IDD_TESTS       = 210;
    constexpr int IDC_TESTS_SEL   = 211;
    constexpr int IDC_TESTS_COUNT = 212;

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
    HWND g_hwndMaintLabel = nullptr;
    HWND g_hwndClean = nullptr;
    HWND g_hwndTests = nullptr;
    HWND g_hwndList = nullptr;
    HWND g_hwndLog = nullptr;
    HWND g_hwndReset = nullptr;
    HWND g_hwndKill = nullptr;
    HWND g_hwndKillFirst = nullptr;
    HWND g_hwndStatus = nullptr;
    HWND g_hwndOverlay = nullptr;
    HWND g_hwndAdminWarn = nullptr;
    HWND g_hwndAdminBtn = nullptr;

    HFONT g_uiFont = nullptr;
    HFONT g_logFont = nullptr;
    HFONT g_titleFont = nullptr;
    HFONT g_subFont = nullptr;
    HFONT g_bannerFont = nullptr;
    HBRUSH g_bannerBrush = nullptr;
    HICON g_appIcon = nullptr;   // TPW logo, large
    HICON g_appIconSm = nullptr; // TPW logo, small (title bar / taskbar)

    bool g_elevated = true; // set in WinMain; drives the not-admin warning banner

    std::string g_overlayTitle = "Working...";
    std::string g_overlaySubtitle;
    int g_spinPhase = 0;

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
        // Maintenance works over both transports: USB (print channel) and LAN
        // (LPR, port 515). Only gated by the busy state.
        EnableWindow(g_hwndMaintLabel, !busy);
        EnableWindow(g_hwndClean, !busy);
        EnableWindow(g_hwndTests, !busy);

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

    // ---- Loading overlay ------------------------------------------------------
    //
    // A borderless child window that covers the client area during long-running
    // work. It paints a rotating dot spinner, an indeterminate progress bar and
    // a big status headline, replacing the raw log scroll as the primary "we're
    // working" signal. The log pane stays underneath for anyone who wants detail.

    // Linear blend between two colours; t = 0 -> a, t = 1 -> b.
    COLORREF BlendColor(COLORREF a, COLORREF b, double t)
    {
        auto lerp = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
        return RGB(lerp(GetRValue(a), GetRValue(b)),
                   lerp(GetGValue(a), GetGValue(b)),
                   lerp(GetBValue(a), GetBValue(b)));
    }

    void DrawOverlay(HDC hdc, const RECT& rc)
    {
        const COLORREF kBackground = RGB(250, 250, 252);
        const COLORREF kAccent     = RGB(0, 120, 215);
        const COLORREF kTail       = RGB(224, 224, 230);
        const COLORREF kTitle      = RGB(28, 28, 32);
        const COLORREF kSubtitle   = RGB(120, 120, 128);

        HBRUSH bg = CreateSolidBrush(kBackground);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        const int cx = rc.right / 2;
        const int cy = rc.bottom / 2 - 40;

        // Rotating dot spinner: the "head" dot is the accent colour and each
        // dot behind it fades toward light grey, giving the illusion of motion.
        const int dots = 12;
        const int orbit = 26;
        const int dotR = 5;
        const int head = (g_spinPhase / 3) % dots;

        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
        for (int i = 0; i < dots; ++i)
        {
            double ang = (i * 2.0 * 3.14159265 / dots) - 3.14159265 / 2.0;
            int x = cx + static_cast<int>(orbit * std::cos(ang));
            int y = cy + static_cast<int>(orbit * std::sin(ang));

            int behind = (head - i + dots) % dots;
            double t = static_cast<double>(behind) / (dots - 1); // 0 at head
            HBRUSH dot = CreateSolidBrush(BlendColor(kAccent, kTail, t));
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, dot));
            Ellipse(hdc, x - dotR, y - dotR, x + dotR, y + dotR);
            SelectObject(hdc, oldBrush);
            DeleteObject(dot);
        }
        SelectObject(hdc, oldPen);

        SetBkMode(hdc, TRANSPARENT);

        RECT titleRect = rc;
        titleRect.top = cy + 64;
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_titleFont));
        SetTextColor(hdc, kTitle);
        DrawTextA(hdc, g_overlayTitle.c_str(), -1, &titleRect,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (!g_overlaySubtitle.empty())
        {
            RECT subRect = rc;
            subRect.left += 40;
            subRect.right -= 40;
            subRect.top = cy + 108;
            SelectObject(hdc, g_subFont);
            SetTextColor(hdc, kSubtitle);
            DrawTextA(hdc, g_overlaySubtitle.c_str(), -1, &subRect,
                      DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        SelectObject(hdc, oldFont);

        // Indeterminate progress bar: a highlight segment sweeps along a track.
        const int barW = 260;
        const int barH = 4;
        int bx = cx - barW / 2;
        int by = cy + 156;

        RECT track = { bx, by, bx + barW, by + barH };
        HBRUSH trackBrush = CreateSolidBrush(kTail);
        FillRect(hdc, &track, trackBrush);
        DeleteObject(trackBrush);

        const int segW = 72;
        int span = barW + segW;
        int pos = ((g_spinPhase * 4) % span) - segW; // -segW .. barW
        int sx = std::max(bx, bx + pos);
        int sx2 = std::min(bx + barW, bx + pos + segW);
        if (sx2 > sx)
        {
            RECT seg = { sx, by, sx2, by + barH };
            HBRUSH segBrush = CreateSolidBrush(kAccent);
            FillRect(hdc, &seg, segBrush);
            DeleteObject(segBrush);
        }
    }

    LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wParam == OVERLAY_TIMER)
            {
                g_spinPhase = (g_spinPhase + 1) % 100000;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1; // fully repainted in WM_PAINT via a back buffer

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP buf = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, buf));

            DrawOverlay(mem, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

            SelectObject(mem, oldBmp);
            DeleteObject(buf);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void ShowOverlay(const std::string& title, const std::string& subtitle)
    {
        g_overlayTitle = title;
        g_overlaySubtitle = subtitle;
        g_spinPhase = 0;

        RECT rc;
        GetClientRect(g_hwndMain, &rc);
        RECT sbRect = { 0 };
        GetWindowRect(g_hwndStatus, &sbRect);
        int statusH = sbRect.bottom - sbRect.top;

        SetWindowPos(g_hwndOverlay, HWND_TOP, 0, 0, rc.right, rc.bottom - statusH,
                     SWP_SHOWWINDOW);
        SetTimer(g_hwndOverlay, OVERLAY_TIMER, 30, nullptr);
        InvalidateRect(g_hwndOverlay, nullptr, FALSE);
    }

    // Update the headline while the overlay is already showing (thread-safe entry
    // point: posts to the UI thread rather than touching the window directly).
    void PostStage(const std::string& title)
    {
        PostMessage(g_hwndMain, WM_APP_STAGE, 0, reinterpret_cast<LPARAM>(new std::string(title)));
    }

    void HideOverlay()
    {
        KillTimer(g_hwndOverlay, OVERLAY_TIMER);
        ShowWindow(g_hwndOverlay, SW_HIDE);
    }

    // ---- Worker threads -------------------------------------------------------

    void InitWorker()
    {
        // Logged first so it is captured in any log screenshot or paste sent with
        // a "not working" report; USB failures almost always trace back to this.
        if (g_elevated)
            std::cout << "[i] Administrator rights: YES." << std::endl;
        else
            std::cout << "[!] Administrator rights: NO. USB resets need elevation and will "
                         "likely fail. Use 'Restart as Admin' in the yellow banner." << std::endl;

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

        if (!std::filesystem::exists("lan_database.json"))
        {
            std::cout << "[i] First run: downloading network model database..." << std::endl;
            if (ewr::SyncLanDatabaseOTA())
                std::cout << "[i] Network model database download: SUCCESS." << std::endl;
            else
                std::cout << "[i] Network model database download: FAILED (offline?)." << std::endl;
        }

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
                PostStage("Closing Epson background processes...");
                std::cout << "[i] Closing " << processes.size() << " Epson background process(es)..." << std::endl;
                int killed = ewr::KillEpsonProcesses();
                std::cout << "[i] Closed " << killed << " of " << processes.size() << " process(es)." << std::endl;
            }
        }

        std::vector<std::vector<unsigned char>> executionSequence;

        PostStage("Building the reset sequence...");
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

        PostStage("Connecting to the printer...");
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

        PostStage("Sending reset commands...");
        bool success = ewr::ExecutePayloadSequence(hPrinter, executionSequence);
        ewr::DisconnectPrinter(hPrinter);

        PostMessage(g_hwndMain, WM_APP_RESET_DONE, success ? 1 : 0, 0);
    }

    void LanResetWorker(ewr::LanModel model, std::string host)
    {
        bool success = ewr::SnmpWasteReset(host, model);
        PostMessage(g_hwndMain, WM_APP_RESET_DONE, success ? 1 : 0, 0);
    }

    void UsbJobWorker(std::vector<unsigned char> job, std::string label)
    {
        PostStage("Connecting to the printer...");
        std::cout << "Scanning USB ports for Epson device..." << std::endl;
        ewr::EwrDeviceHandle hPrinter = ewr::AutoConnectEpsonPrinter();

        if (!hPrinter)
        {
            std::cerr << "[ERROR] Could not find an Epson printer, or the connection was refused." << std::endl;
            std::cerr << "[!] Check the USB cable and printer power, close Epson background processes," << std::endl;
            std::cerr << "    then try again." << std::endl;
            PostMessage(g_hwndMain, WM_APP_MAINT_DONE, 0, 0);
            return;
        }

        PostStage("Sending " + label + "...");
        bool success = ewr::SendUsbPrintJob(hPrinter, job, label);
        ewr::DisconnectPrinter(hPrinter);

        PostMessage(g_hwndMain, WM_APP_MAINT_DONE, success ? 1 : 0, 0);
    }

    void LanJobWorker(std::vector<unsigned char> job, std::string host, std::string label)
    {
        PostStage("Sending " + label + " over LPR...");
        bool success = ewr::LanSendPrintJob(host, job, label);
        PostMessage(g_hwndMain, WM_APP_MAINT_DONE, success ? 1 : 0, 0);
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
        ShowOverlay("Preparing reset...", selected.displayName + "  •  USB");
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
        ShowOverlay("Sending reset commands...", model.name + "  •  " + host);
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
        ShowOverlay("Scanning the network...", "Looking for Epson printers on your LAN");
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

    // Dispatch a prebuilt maintenance job over the active transport (USB or LAN).
    void StartMaintenanceJob(std::vector<unsigned char> job, const std::string& label)
    {
        if (g_busy || job.empty())
            return;

        bool lan = (g_mode == Mode::Lan);
        const char* transport = lan ? "over the network (LPR)" : "over USB";

        // LAN needs the printer IP but no model selection (jobs are model-independent).
        std::string host;
        if (lan)
        {
            host = GetEditText(g_hwndIp);
            host.erase(std::remove_if(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c); }), host.end());
            if (host.empty())
            {
                MessageBoxA(g_hwndMain, "Enter the printer's IP address, or use Detect Printers to find it.",
                    "TPW Epson Tool", MB_OK | MB_ICONINFORMATION);
                return;
            }
        }

        std::string msg = "Send the " + label + " " + transport + "?\n\n"
            + (lan ? "Sent to " + host + " over LPR (port 515). " : "")
            + "Make sure paper is loaded and the printer is powered on.";
        if (MessageBoxA(g_hwndMain, msg.c_str(), "TPW Epson Tool - Maintenance", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;

        SetBusy(true);
        SetStatus("Sending " + label + " " + transport + "...");
        ShowOverlay("Sending " + label + "...", (lan ? host : std::string("USB")) + "  •  maintenance");

        if (lan)
            std::thread(LanJobWorker, std::move(job), host, label).detach();
        else
            std::thread(UsbJobWorker, std::move(job), label).detach();
    }

    // ---- Maintenance dialogs --------------------------------------------------

    // Results filled by the modal dialog procs below.
    int  g_cleanGroup = 0;
    bool g_cleanPower = false;
    int  g_testSel = 0;
    int  g_testCount = 1;

    INT_PTR CALLBACK CleanDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            HWND cb = GetDlgItem(hDlg, IDC_CLEAN_GROUP);
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("All nozzles"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Black ink nozzle"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Colour ink nozzles"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Head cleaning (alternative mode)"));
            SendMessage(cb, CB_SETCURSEL, 0, 0);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                g_cleanGroup = static_cast<int>(SendMessage(GetDlgItem(hDlg, IDC_CLEAN_GROUP), CB_GETCURSEL, 0, 0));
                g_cleanPower = SendMessage(GetDlgItem(hDlg, IDC_CLEAN_POWER), BM_GETCHECK, 0, 0) == BST_CHECKED;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
        return FALSE;
    }

    INT_PTR CALLBACK TestsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            HWND cb = GetDlgItem(hDlg, IDC_TESTS_SEL);
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Standard nozzle check"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Alternative nozzle check"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Colour test pattern"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Advance paper (lines)"));
            SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Feed sheets"));
            SendMessage(cb, CB_SETCURSEL, 0, 0);
            SetWindowTextA(GetDlgItem(hDlg, IDC_TESTS_COUNT), "1");
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                g_testSel = static_cast<int>(SendMessage(GetDlgItem(hDlg, IDC_TESTS_SEL), CB_GETCURSEL, 0, 0));
                char buf[16] = { 0 };
                GetWindowTextA(GetDlgItem(hDlg, IDC_TESTS_COUNT), buf, sizeof(buf));
                g_testCount = atoi(buf);
                if (g_testCount < 1)
                    g_testCount = 1;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
        return FALSE;
    }

    void OnCleanHeads()
    {
        if (g_busy)
            return;
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtr(g_hwndMain, GWLP_HINSTANCE));
        if (DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_CLEAN), g_hwndMain, CleanDlgProc, 0) != IDOK)
            return;

        ewr::CleanGroup group = static_cast<ewr::CleanGroup>(g_cleanGroup);
        std::string label = g_cleanPower ? "power clean" : "head cleaning";
        if (g_cleanPower &&
            MessageBoxA(g_hwndMain,
                "Power Clean uses a large amount of ink and fills the waste ink pad faster. "
                "Use it only when a normal clean has not cleared the nozzles.\n\nContinue?",
                "TPW Epson Tool - Power Clean", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return;

        StartMaintenanceJob(ewr::BuildCleanNozzles(group, g_cleanPower), label);
    }

    void OnTestPatterns()
    {
        if (g_busy)
            return;
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtr(g_hwndMain, GWLP_HINSTANCE));
        if (DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_TESTS), g_hwndMain, TestsDlgProc, 0) != IDOK)
            return;

        std::vector<unsigned char> job;
        std::string label;
        switch (g_testSel)
        {
        case 0: job = ewr::BuildMaintenanceSequence(ewr::MaintenanceAction::NozzleCheck);    label = "nozzle check"; break;
        case 1: job = ewr::BuildMaintenanceSequence(ewr::MaintenanceAction::NozzleCheckAlt); label = "alternative nozzle check"; break;
        case 2: job = ewr::BuildColorTestPattern(false);                                     label = "colour test pattern"; break;
        case 3: job = ewr::BuildAdvancePaper(g_testCount);                                   label = "advance paper"; break;
        case 4: job = ewr::BuildFeedSheets(g_testCount);                                     label = "paper feed test"; break;
        default: return;
        }
        StartMaintenanceJob(std::move(job), label);
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

        // A warning banner occupies the very top of the client area when the
        // app is not elevated; everything else shifts down below it.
        const int bannerH = g_elevated ? 0 : 34;
        if (!g_elevated)
        {
            const int elevBtnW = 150;
            MoveWindow(g_hwndAdminWarn, 0, 0, w, bannerH, TRUE);
            MoveWindow(g_hwndAdminBtn, w - elevBtnW - 6, 5, elevBtnW, bannerH - 10, TRUE);
        }

        int y0 = bannerH + m;             // search row
        int y1 = y0 + rowH + 6;           // mode / ip row
        int y2 = y1 + rowH + 8;           // USB maintenance row
        int listTop = y2 + rowH + 8;

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

        MoveWindow(g_hwndMaintLabel, m, y2 + 4, 96, 18, TRUE);
        MoveWindow(g_hwndClean, m + 100, y2, 150, rowH, TRUE);
        MoveWindow(g_hwndTests, m + 258, y2, 150, rowH, TRUE);

        MoveWindow(g_hwndList, m, listTop, listW, listH, TRUE);
        MoveWindow(g_hwndLog, 2 * m + listW, listTop, w - 3 * m - listW, listH, TRUE);

        MoveWindow(g_hwndKillFirst, m, bottomRowTop + 6, 280, 20, TRUE);
        MoveWindow(g_hwndKill, w - m - 2 * btnW - 8, bottomRowTop, btnW, btnH, TRUE);
        MoveWindow(g_hwndReset, w - m - btnW, bottomRowTop, btnW, btnH, TRUE);

        // Keep the loading overlay covering the client area (above the status bar).
        if (g_hwndOverlay)
            MoveWindow(g_hwndOverlay, 0, 0, w, h - statusH, TRUE);
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

        g_hwndMaintLabel = CreateWindowExA(0, "STATIC", "Maintenance:",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

        g_hwndClean = CreateWindowExA(0, "BUTTON", "Clean Print Head...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLEAN)), hInst, nullptr);

        g_hwndTests = CreateWindowExA(0, "BUTTON", "Test Patterns...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TESTS)), hInst, nullptr);

        g_hwndList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);

        g_hwndLog = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
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

        // Not-admin warning banner. Kept hidden when elevated; LayoutControls
        // gives it a band at the top of the window when g_elevated is false.
        DWORD bannerStyle = WS_CHILD | SS_CENTERIMAGE | SS_LEFT;
        if (!g_elevated)
            bannerStyle |= WS_VISIBLE;
        g_hwndAdminWarn = CreateWindowExA(0, "STATIC",
            "  Not running as Administrator  -  USB resets need elevation and will likely fail.",
            bannerStyle, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

        DWORD elevBtnStyle = WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON;
        if (!g_elevated)
            elevBtnStyle |= WS_VISIBLE;
        g_hwndAdminBtn = CreateWindowExA(0, "BUTTON", "Restart as Admin",
            elevBtnStyle, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ELEVATE)), hInst, nullptr);

        g_uiFont = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        g_logFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

        // Big headline + subtitle used by the loading overlay.
        g_titleFont = CreateFontA(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        g_subFont = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        g_bannerFont = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        g_bannerBrush = CreateSolidBrush(RGB(255, 224, 130)); // amber warning strip

        HWND uiControls[] = { g_hwndSearchLabel, g_hwndSearch, g_hwndModeUsb, g_hwndModeLan,
            g_hwndIpLabel, g_hwndIp, g_hwndDetect, g_hwndMaintLabel, g_hwndClean, g_hwndTests,
            g_hwndList, g_hwndKillFirst, g_hwndKill, g_hwndReset, g_hwndStatus, g_hwndAdminBtn };
        for (HWND ctrl : uiControls)
            SendMessage(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

        SendMessage(g_hwndLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_logFont), TRUE);
        SendMessage(g_hwndAdminWarn, WM_SETFONT, reinterpret_cast<WPARAM>(g_bannerFont), TRUE);

        // Created last so it sits on top of every sibling in the z-order when
        // shown. Hidden until a long-running operation calls ShowOverlay().
        g_hwndOverlay = CreateWindowExA(0, "EwrOverlay", "",
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);
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
        if (g_appIcon)
        {
            cfg.dwFlags |= TDF_USE_HICON_MAIN;
            cfg.hMainIcon = g_appIcon; // TPW logo
        }
        else
        {
            cfg.pszMainIcon = TD_INFORMATION_ICON;
        }
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

    // Relaunch elevated (triggers a UAC prompt) and close this instance on success.
    void OnRestartAsAdmin(HWND hwnd)
    {
        if (ewr::RelaunchElevated(0, nullptr))
            DestroyWindow(hwnd);
        else
            MessageBoxA(hwnd,
                "Could not restart as Administrator. The elevation prompt may have "
                "been cancelled.\n\nYou can also close TPW Epson Tool and right-click it, "
                "then choose \"Run as administrator\".",
                "TPW Epson Tool", MB_OK | MB_ICONWARNING);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
            g_hwndMain = hwnd;
            if (g_appIcon)
                SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
            if (g_appIconSm)
                SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIconSm));
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
            case IDC_CLEAN:
                if (HIWORD(wParam) == BN_CLICKED)
                    OnCleanHeads();
                return 0;
            case IDC_TESTS:
                if (HIWORD(wParam) == BN_CLICKED)
                    OnTestPatterns();
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
            case IDC_ELEVATE:
                if (HIWORD(wParam) == BN_CLICKED)
                    OnRestartAsAdmin(hwnd);
                return 0;
            }
            break;

        case WM_CTLCOLORSTATIC:
            // Paint the not-admin banner as a bold amber warning strip.
            if (reinterpret_cast<HWND>(lParam) == g_hwndAdminWarn)
            {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(120, 53, 0));
                SetBkColor(dc, RGB(255, 224, 130));
                return reinterpret_cast<LRESULT>(g_bannerBrush);
            }
            break;

        case WM_APP_LOG:
        {
            std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lParam));
            AppendLogLine(*line);
            return 0;
        }

        case WM_APP_STAGE:
        {
            std::unique_ptr<std::string> title(reinterpret_cast<std::string*>(lParam));
            g_overlayTitle = *title;
            InvalidateRect(g_hwndOverlay, nullptr, FALSE);
            return 0;
        }

        case WM_APP_INIT_DONE:
        {
            std::unique_ptr<std::vector<MenuOption>> options(reinterpret_cast<std::vector<MenuOption>*>(lParam));
            g_options = std::move(*options);
            RefreshList();
            SetBusy(false);
            HideOverlay();

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
            HideOverlay();

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
            HideOverlay();
            if (wParam == 1)
            {
                SetStatus("Reset complete. Power-cycle the printer.");
                MessageBoxA(hwnd,
                    "SUCCESS!\n\nTurn the printer OFF, then ON using its physical power button "
                    "to commit the changes.",
                    "TPW Epson Tool - Reset Complete", MB_OK | MB_ICONINFORMATION);
            }
            else if (!g_elevated && g_mode == Mode::Usb)
            {
                SetStatus("Reset failed - not running as Administrator.");
                int r = MessageBoxA(hwnd,
                    "Reset failed.\n\nTPW Epson Tool is NOT running as Administrator, and "
                    "USB resets almost always need it. This is the most likely cause.\n\n"
                    "Restart as Administrator and try again?",
                    "TPW Epson Tool - Reset Failed", MB_YESNO | MB_ICONWARNING);
                if (r == IDYES)
                    OnRestartAsAdmin(hwnd);
            }
            else
            {
                SetStatus("Reset failed. See the log for details.");
                MessageBoxA(hwnd,
                    "Reset failed. See the log on the right for details.",
                    "TPW Epson Tool - Reset Failed", MB_OK | MB_ICONWARNING);
            }
            return 0;

        case WM_APP_MAINT_DONE:
            SetBusy(false);
            HideOverlay();
            if (wParam == 1)
            {
                SetStatus("Maintenance command sent.");
                MessageBoxA(hwnd,
                    "Sent to the printer.\n\nIf nothing prints, load paper and check that the "
                    "printer is online. A nozzle check prints a grid of lines; gaps mean clogged "
                    "nozzles that a head cleaning can clear.",
                    "TPW Epson Tool", MB_OK | MB_ICONINFORMATION);
            }
            else if (!g_elevated && g_mode == Mode::Usb)
            {
                SetStatus("Maintenance failed - not running as Administrator.");
                int r = MessageBoxA(hwnd,
                    "Could not send the command.\n\nTPW Epson Tool is NOT running as Administrator, "
                    "and direct USB access almost always needs it.\n\nRestart as Administrator and "
                    "try again?",
                    "TPW Epson Tool", MB_YESNO | MB_ICONWARNING);
                if (r == IDYES)
                    OnRestartAsAdmin(hwnd);
            }
            else
            {
                SetStatus("Maintenance failed. See the log for details.");
                MessageBoxA(hwnd,
                    "Could not send the command. See the log on the right for details.",
                    "TPW Epson Tool", MB_OK | MB_ICONWARNING);
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

    g_elevated = ewr::IsRunningElevated();
    if (!g_elevated)
    {
        int r = MessageBoxA(nullptr,
            "TPW Epson Tool is not running as Administrator.\n\n"
            "Direct USB access to the printer's maintenance interface usually requires "
            "elevated privileges on Windows. Network (SNMP) resets do not need "
            "administrator rights.\n\n"
            "Relaunch TPW Epson Tool as Administrator now?",
            "TPW Epson Tool - Administrator Rights", MB_YESNO | MB_ICONWARNING);

        if (r == IDYES && ewr::RelaunchElevated(0, nullptr))
            return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // TPW logo icon: large for Alt-Tab / About, small for the title bar and taskbar.
    g_appIcon = static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APPICON),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    g_appIconSm = static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = g_appIconSm ? g_appIconSm : wc.hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "EwrMainWindow";
    RegisterClassExA(&wc);

    WNDCLASSEXA oc = { sizeof(oc) };
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = hInstance;
    oc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    oc.hbrBackground = nullptr; // painted entirely in WM_PAINT
    oc.lpszClassName = "EwrOverlay";
    RegisterClassExA(&oc);

    const char* windowTitle = g_elevated
        ? "TPW Epson Tool"
        : "TPW Epson Tool  -  Not running as Administrator";
    HWND hwnd = CreateWindowExA(0, "EwrMainWindow", windowTitle,
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
    ShowOverlay("Loading printer database...", "Checking for updates and preparing payloads");
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
