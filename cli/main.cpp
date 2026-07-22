#include <iostream>
#include <algorithm>
#include <string>
#include <cctype>
#include <cstdlib>
#include "ewr/payload.h"
#include "ewr/parser.h"
#include "ewr/usb.h"
#include "ewr/generator.h"

struct MenuOption
{
    std::string displayName;
    bool isReplay;
    ewr::PrinterModel replayModel;
    ewr::DbPrinterModel smartModel;
};

std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

std::string PromptLine(const std::string& prompt)
{
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

bool PromptYesNo(const std::string& prompt, bool defaultYes)
{
    std::string suffix = defaultYes ? " [Y/n]: " : " [y/N]: ";
    std::string answer = toLower(PromptLine(prompt + suffix));

    if (answer.empty())
        return defaultYes;

    return answer[0] == 'y';
}

void OfferEpsonProcessCleanup(bool autoDetected)
{
    auto processes = ewr::ListEpsonProcesses();

    if (processes.empty())
    {
        if (!autoDetected)
            std::cout << "[i] No running Epson background processes were found.\n";
        return;
    }

    std::cout << "\n[!] Found " << processes.size() << " Epson-related process(es) that may hold the printer port open:\n";
    for (const auto& p : processes)
        std::cout << "    - " << p.name << " (PID " << p.pid << ")\n";

    if (PromptYesNo("Close these now (recommended before resetting)?", true))
    {
        int killed = ewr::KillEpsonProcesses();
        std::cout << "[i] Closed " << killed << " of " << processes.size() << " process(es).\n";
    }
}

void CheckElevation(int argc, char** argv)
{
    if (ewr::IsRunningElevated())
        return;

    std::cout << "[!] EWR is not running as Administrator." << std::endl;
    std::cout << "    Direct USB access to the printer's maintenance interface usually requires\n";
    std::cout << "    elevated privileges on Windows; without them, connecting to the printer\n";
    std::cout << "    may fail with an Access Denied error.\n";

    if (PromptYesNo("Relaunch EWR as Administrator now?", true))
    {
        if (ewr::RelaunchElevated(argc, argv))
        {
            std::exit(0);
        }
        else
        {
            std::cout << "[!] Could not relaunch elevated (UAC prompt declined or unsupported on this platform).\n";
            std::cout << "    Continuing without administrator rights.\n\n";
        }
    }
    else
    {
        std::cout << "[i] Continuing without administrator rights.\n\n";
    }
}

int main(int argc, char** argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "       EWR - Epson Waste Reset          " << std::endl;
    std::cout << "========================================\n" << std::endl;

    CheckElevation(argc, argv);
    OfferEpsonProcessCleanup(true);

    ewr::UniversalGenerator generator;

    std::cout << "\n[i] Checking for OTA database updates... ";

    if (generator.SyncDatabaseOTA())
        std::cout << "SUCCESS." << std::endl;
    else
        std::cout << "OFFLINE (Using local cache)." << std::endl;

    generator.LoadDatabase("database.json");
    auto replayModels = ewr::ScanModelsFolder("models");
    auto smartModels = generator.GetAvailableModels();

    if (replayModels.empty() && smartModels.empty())
    {
        std::cerr << "\n[!] No payloads found. You need internet access on first run, or a 'models' folder with payload dumps." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[i] Loaded " << smartModels.size() << " Smart Protocol payloads." << std::endl;
    std::cout << "[i] Loaded " << replayModels.size() << " Custom payloads.\n" << std::endl;

    std::vector<MenuOption> options;

    for (const auto& sm : smartModels)
        options.push_back({ sm.name + " (Smart Protocol)", false, {}, sm });

    for (const auto& lm : replayModels)
        options.push_back({ lm.name + " (Replay)", true, lm, {} });

    std::sort(options.begin(), options.end(), [](const MenuOption& a, const MenuOption& b)
        {
            return a.displayName < b.displayName;
        });

    // Outer loop: on any failure (bad selection, connection failure, execution
    // failure) we land back here instead of exiting the program.
    while (true)
    {
        MenuOption selected;
        bool hasSelected = false;

        while (!hasSelected)
        {
            std::cout << "\nEnter printer model to search (e.g., 'L3150' or 'XP'), 'kill' to close Epson background processes, or 'exit' to quit: ";
            std::string searchQuery;
            std::getline(std::cin, searchQuery);

            if (searchQuery.empty())
                continue;

            std::string searchLower = toLower(searchQuery);
            if (searchLower == "exit" || searchLower == "quit")
                return 0;

            if (searchLower == "kill")
            {
                OfferEpsonProcessCleanup(false);
                continue;
            }

            std::vector<MenuOption> filteredOptions;
            for (const auto& opt : options)
            {
                if (toLower(opt.displayName).find(searchLower) != std::string::npos)
                    filteredOptions.push_back(opt);
            }

            if (filteredOptions.empty())
            {
                std::cout << "[-] No printers found matching '" << searchQuery << "'. Please try again.\n";
                continue;
            }

            std::cout << "\nFound " << filteredOptions.size() << " matching printers:\n";

            for (size_t i = 0; i < filteredOptions.size(); ++i)
                std::cout << "[" << i + 1 << "] " << filteredOptions[i].displayName << "\n";

            std::cout << "[0] Search again...\n";

            std::cout << "\nSelect your printer [0-" << filteredOptions.size() << "]: ";
            std::string choiceStr;
            std::getline(std::cin, choiceStr);

            try
            {
                int choice = std::stoi(choiceStr);
                if (choice == 0)
                {
                    continue;
                }
                else if (choice >= 1 && choice <= static_cast<int>(filteredOptions.size()))
                {
                    selected = filteredOptions[choice - 1];
                    hasSelected = true;
                }
                else
                {
                    std::cout << "[-] Invalid selection. Please try again.\n";
                }
            }
            catch (...)
            {
                std::cout << "[-] Invalid input. Please enter a number.\n";
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
            executionSequence = generator.GenerateSequence(selected.smartModel);
        }

        if (executionSequence.empty())
        {
            std::cerr << "[-] Failed to construct payload. Returning to main menu.\n";
            continue;
        }

        std::cout << "Scanning USB ports for Epson device..." << std::endl;
        ewr::EwrDeviceHandle hPrinter = ewr::AutoConnectEpsonPrinter();

        while (!hPrinter)
        {
            std::cerr << "[ERROR] Could not find an Epson printer, or the connection was refused." << std::endl;
            std::cout << "\n[1] Close Epson background processes and retry\n";
            std::cout << "[2] Retry connection\n";
            std::cout << "[3] Return to main menu\n";
            std::cout << "Select an option [1-3]: ";

            std::string choiceStr;
            std::getline(std::cin, choiceStr);

            if (choiceStr == "1")
            {
                OfferEpsonProcessCleanup(false);
                hPrinter = ewr::AutoConnectEpsonPrinter();
            }
            else if (choiceStr == "2")
            {
                hPrinter = ewr::AutoConnectEpsonPrinter();
            }
            else
            {
                break;
            }
        }

        if (!hPrinter)
            continue;

        if (ewr::ExecutePayloadSequence(hPrinter, executionSequence))
        {
            std::cout << "\n========================================" << std::endl;
            std::cout << " SUCCESS! Turn the printer OFF, then ON." << std::endl;
            std::cout << "========================================" << std::endl;
        }
        else
        {
            std::cerr << "\n[-] The reset sequence did not complete successfully. Returning to main menu.\n";
        }

        ewr::DisconnectPrinter(hPrinter);
    }
}
