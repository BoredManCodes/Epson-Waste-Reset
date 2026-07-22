#include "ewr/snmp.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

namespace ewr {

    namespace {

        // ---- Winsock lifetime -------------------------------------------------

        struct WinsockGuard
        {
            bool ok = false;
            WinsockGuard()
            {
                WSADATA wsa;
                ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
            }
            ~WinsockGuard()
            {
                if (ok)
                    WSACleanup();
            }
        };

        // ---- BER / ASN.1 encoding (SNMP v1) -----------------------------------

        void BerAppendLength(std::vector<uint8_t>& out, size_t len)
        {
            if (len < 0x80)
            {
                out.push_back(static_cast<uint8_t>(len));
                return;
            }
            uint8_t tmp[8];
            int n = 0;
            while (len > 0)
            {
                tmp[n++] = static_cast<uint8_t>(len & 0xFF);
                len >>= 8;
            }
            out.push_back(static_cast<uint8_t>(0x80 | n));
            for (int i = n - 1; i >= 0; --i)
                out.push_back(tmp[i]);
        }

        // Wrap 'content' in a tag/length/value triple.
        std::vector<uint8_t> BerTLV(uint8_t tag, const std::vector<uint8_t>& content)
        {
            std::vector<uint8_t> out;
            out.push_back(tag);
            BerAppendLength(out, content.size());
            out.insert(out.end(), content.begin(), content.end());
            return out;
        }

        std::vector<uint8_t> BerInteger(uint32_t value)
        {
            // Minimal-length two's-complement, always non-negative here.
            uint8_t bytes[5];
            int n = 0;
            do
            {
                bytes[n++] = static_cast<uint8_t>(value & 0xFF);
                value >>= 8;
            } while (value > 0);

            std::vector<uint8_t> content;
            // Prepend 0x00 if the top bit is set, so it stays positive.
            if (bytes[n - 1] & 0x80)
                content.push_back(0x00);
            for (int i = n - 1; i >= 0; --i)
                content.push_back(bytes[i]);
            return BerTLV(0x02, content);
        }

        void BerAppendBase128(std::vector<uint8_t>& out, uint32_t v)
        {
            uint8_t tmp[5];
            int n = 0;
            tmp[n++] = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            while (v > 0)
            {
                tmp[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
                v >>= 7;
            }
            for (int i = n - 1; i >= 0; --i)
                out.push_back(tmp[i]);
        }

        std::vector<uint8_t> BerOID(const std::vector<uint32_t>& arcs)
        {
            std::vector<uint8_t> content;
            // First two arcs are packed into one octet: 40*a + b.
            content.push_back(static_cast<uint8_t>(40 * arcs[0] + arcs[1]));
            for (size_t i = 2; i < arcs.size(); ++i)
                BerAppendBase128(content, arcs[i]);
            return BerTLV(0x06, content);
        }

        // Assemble a full SNMP v1 GetRequest for a single OID.
        std::vector<uint8_t> BuildGetRequest(const std::vector<uint32_t>& oidArcs, uint32_t requestId)
        {
            std::vector<uint8_t> varbind;
            {
                std::vector<uint8_t> vbContent = BerOID(oidArcs);
                std::vector<uint8_t> nullVal = BerTLV(0x05, {});
                vbContent.insert(vbContent.end(), nullVal.begin(), nullVal.end());
                varbind = BerTLV(0x30, vbContent);
            }

            std::vector<uint8_t> varbindList = BerTLV(0x30, varbind);

            std::vector<uint8_t> pduContent;
            {
                auto id = BerInteger(requestId);
                auto errStatus = BerInteger(0);
                auto errIndex = BerInteger(0);
                pduContent.insert(pduContent.end(), id.begin(), id.end());
                pduContent.insert(pduContent.end(), errStatus.begin(), errStatus.end());
                pduContent.insert(pduContent.end(), errIndex.begin(), errIndex.end());
                pduContent.insert(pduContent.end(), varbindList.begin(), varbindList.end());
            }
            std::vector<uint8_t> pdu = BerTLV(0xA0, pduContent); // GetRequest-PDU

            std::vector<uint8_t> msgContent;
            {
                auto version = BerInteger(0); // SNMP v1
                std::vector<uint8_t> community = { 'p', 'u', 'b', 'l', 'i', 'c' };
                auto communityTLV = BerTLV(0x04, community);
                msgContent.insert(msgContent.end(), version.begin(), version.end());
                msgContent.insert(msgContent.end(), communityTLV.begin(), communityTLV.end());
                msgContent.insert(msgContent.end(), pdu.begin(), pdu.end());
            }
            return BerTLV(0x30, msgContent);
        }

        // ---- BER parsing ------------------------------------------------------

        // Read a TLV header at 'pos'. Fills tag, content offset and length, and
        // advances 'pos' past the whole element on return of the value region.
        bool BerReadHeader(const std::vector<uint8_t>& buf, size_t& pos,
                           uint8_t& tag, size_t& contentStart, size_t& contentLen)
        {
            if (pos + 2 > buf.size())
                return false;
            tag = buf[pos++];
            uint8_t lenByte = buf[pos++];
            size_t len = 0;
            if (lenByte < 0x80)
            {
                len = lenByte;
            }
            else
            {
                int n = lenByte & 0x7F;
                if (n == 0 || n > 4 || pos + n > buf.size())
                    return false;
                for (int i = 0; i < n; ++i)
                    len = (len << 8) | buf[pos++];
            }
            if (pos + len > buf.size())
                return false;
            contentStart = pos;
            contentLen = len;
            return true;
        }

        // Extract the first varbind's value (tag + bytes) from a GetResponse.
        bool ParseFirstVarbindValue(const std::vector<uint8_t>& buf, uint8_t& valueTag,
                                    std::vector<uint8_t>& valueBytes)
        {
            size_t pos = 0;
            uint8_t tag;
            size_t cs, cl;

            // Outer message SEQUENCE
            if (!BerReadHeader(buf, pos, tag, cs, cl) || tag != 0x30)
                return false;
            pos = cs;

            // version INTEGER
            if (!BerReadHeader(buf, pos, tag, cs, cl)) return false;
            pos = cs + cl;
            // community OCTET STRING
            if (!BerReadHeader(buf, pos, tag, cs, cl)) return false;
            pos = cs + cl;
            // PDU (GetResponse = 0xA2)
            if (!BerReadHeader(buf, pos, tag, cs, cl)) return false;
            pos = cs;

            // request-id, error-status, error-index INTEGERs
            for (int i = 0; i < 3; ++i)
            {
                if (!BerReadHeader(buf, pos, tag, cs, cl)) return false;
                pos = cs + cl;
            }
            // varbind list SEQUENCE
            if (!BerReadHeader(buf, pos, tag, cs, cl) || tag != 0x30) return false;
            pos = cs;
            // first varbind SEQUENCE
            if (!BerReadHeader(buf, pos, tag, cs, cl) || tag != 0x30) return false;
            pos = cs;
            // OID
            if (!BerReadHeader(buf, pos, tag, cs, cl) || tag != 0x06) return false;
            pos = cs + cl;
            // value
            if (!BerReadHeader(buf, pos, tag, cs, cl)) return false;

            valueTag = tag;
            valueBytes.assign(buf.begin() + cs, buf.begin() + cs + cl);
            return true;
        }

        // ---- UDP transport ----------------------------------------------------

        std::atomic<uint32_t> g_requestId{ 0x0A000000 };

        // Send one GET and return the raw response datagram. Empty on failure.
        std::vector<uint8_t> SnmpExchange(const std::string& host,
                                          const std::vector<uint32_t>& oidArcs,
                                          int timeoutMs)
        {
            std::vector<uint8_t> empty;

            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET)
                return empty;

            DWORD tv = static_cast<DWORD>(timeoutMs);
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

            sockaddr_in addr = { 0 };
            addr.sin_family = AF_INET;
            addr.sin_port = htons(161);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            {
                closesocket(sock);
                return empty;
            }

            uint32_t reqId = g_requestId.fetch_add(1);
            std::vector<uint8_t> packet = BuildGetRequest(oidArcs, reqId);

            if (sendto(sock, reinterpret_cast<const char*>(packet.data()),
                       static_cast<int>(packet.size()), 0,
                       reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
            {
                closesocket(sock);
                return empty;
            }

            char recvBuf[2048];
            sockaddr_in from = { 0 };
            int fromLen = sizeof(from);
            int n = recvfrom(sock, recvBuf, sizeof(recvBuf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            closesocket(sock);

            if (n <= 0)
                return empty;

            return std::vector<uint8_t>(recvBuf, recvBuf + n);
        }

        // EPSON-CTRL OID prefix: 1.3.6.1.4.1.1248.1.2.2.44.1.1.2.1
        const std::vector<uint32_t> EPSON_CTRL_PREFIX =
            { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 44, 1, 1, 2, 1 };

        std::vector<uint32_t> BuildWriteOid(const LanModel& model, uint16_t address, uint8_t value)
        {
            uint16_t oid = address % 256;
            uint16_t msb = address / 256;

            // Payload: read_key(2), 'B' + derived bytes, addr(2), value, caesar(write_key)
            std::vector<uint32_t> payload = {
                model.read_key[0], model.read_key[1],
                'B',                              // 66  = write
                static_cast<uint32_t>(~'B' & 0xFF),                         // 189
                static_cast<uint32_t>((('B' >> 1) & 0x7F) | (('B' << 7) & 0x80)), // 33
                oid, msb, value
            };
            for (uint8_t b : model.write_key)
                payload.push_back(b == 0 ? 0u : static_cast<uint32_t>(b + 1)); // caesar

            std::vector<uint32_t> arcs = EPSON_CTRL_PREFIX;
            arcs.push_back('|');
            arcs.push_back('|');
            arcs.push_back(static_cast<uint32_t>(payload.size() & 0xFF));        // length low
            arcs.push_back(static_cast<uint32_t>((payload.size() >> 8) & 0xFF)); // length high
            arcs.insert(arcs.end(), payload.begin(), payload.end());
            return arcs;
        }

        bool ContainsAscii(const std::vector<uint8_t>& buf, const char* needle)
        {
            std::string hay(buf.begin(), buf.end());
            return hay.find(needle) != std::string::npos;
        }

        // Blocking connect() can stall ~21s on an unreachable host, so use a
        // non-blocking connect gated by select() with an explicit timeout.
        bool ConnectWithTimeout(SOCKET sock, const sockaddr_in& addr, int timeoutMs)
        {
            u_long nonBlocking = 1;
            ioctlsocket(sock, FIONBIO, &nonBlocking);

            int rc = connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            bool connected = false;

            if (rc == 0)
            {
                connected = true;
            }
            else if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(sock, &writeSet);
                timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

                if (select(0, nullptr, &writeSet, nullptr, &tv) > 0 && FD_ISSET(sock, &writeSet))
                {
                    int err = 0;
                    int errLen = sizeof(err);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen);
                    connected = (err == 0);
                }
            }

            u_long blocking = 0;
            ioctlsocket(sock, FIONBIO, &blocking);
            return connected;
        }

        bool SendAll(SOCKET sock, const unsigned char* data, size_t len)
        {
            size_t sent = 0;
            while (sent < len)
            {
                int n = send(sock, reinterpret_cast<const char*>(data) + sent, static_cast<int>(len - sent), 0);
                if (n == SOCKET_ERROR || n == 0)
                    return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        bool SendStr(SOCKET sock, const std::string& s)
        {
            return SendAll(sock, reinterpret_cast<const unsigned char*>(s.data()), s.size());
        }

        // Read one LPR acknowledgment byte; RFC 1179 uses 0x00 for success.
        bool RecvAck(SOCKET sock)
        {
            char b = 1;
            int n = recv(sock, &b, 1, 0);
            return (n == 1 && b == 0);
        }

        std::string LocalHostName()
        {
            char h[256] = { 0 };
            if (gethostname(h, sizeof(h)) != 0)
                return "ewr";
            return std::string(h);
        }

        std::string LocalUserName()
        {
            const char* u = getenv("USERNAME");
            return (u && *u) ? std::string(u) : std::string("user");
        }

        std::string TrimAndUpper(std::string s)
        {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos)
                return "";
            s = s.substr(a, b - a + 1);
            for (char& c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

    } // anonymous namespace

    // ---- Public API -----------------------------------------------------------

    bool SyncLanDatabaseOTA()
    {
        const char* url = "https://raw.githubusercontent.com/BoredManCodes/Epson-Waste-Reset/main/lan_database.json";
        const char* dest = "lan_database.json";

        HRESULT res = URLDownloadToFileA(NULL, url, dest, 0, NULL);
        return (res == S_OK);
    }

    bool LoadLanDatabase(const std::string& filepath, std::vector<LanModel>& out)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
            return false;

        try
        {
            json j;
            file >> j;
            if (!j.contains("models") || !j["models"].is_array())
                return false;

            for (auto& m : j["models"])
            {
                LanModel model;
                model.name = m.value("name", "");
                if (model.name.empty())
                    continue;

                auto rk = m["read_key"];
                if (rk.is_array() && rk.size() >= 2)
                {
                    model.read_key[0] = rk[0].get<uint8_t>();
                    model.read_key[1] = rk[1].get<uint8_t>();
                }

                for (auto& b : m["write_key"])
                    model.write_key.push_back(b.get<uint8_t>());

                for (auto& pair : m["reset"])
                {
                    if (pair.is_array() && pair.size() >= 2)
                        model.reset.emplace_back(pair[0].get<uint16_t>(), pair[1].get<uint8_t>());
                }

                if (!model.write_key.empty() && !model.reset.empty())
                    out.push_back(std::move(model));
            }
            return !out.empty();
        }
        catch (const json::exception& e)
        {
            std::cerr << "[!] LAN database JSON parse error: " << e.what() << std::endl;
            return false;
        }
    }

    const LanModel* MatchLanModel(const std::vector<LanModel>& db, const std::string& reportedModel)
    {
        std::string report = TrimAndUpper(reportedModel);
        if (report.empty())
            return nullptr;

        // Strip a leading "EPSON " and a trailing " SERIES" for matching.
        if (report.rfind("EPSON ", 0) == 0)
            report = report.substr(6);

        const LanModel* best = nullptr;
        size_t bestLen = 0;

        for (const auto& model : db)
        {
            std::string name = TrimAndUpper(model.name);
            if (name.empty())
                continue;

            // The reported string usually contains the DB model name as a token,
            // e.g. "XP-205 207 SERIES" contains "XP-205". Prefer the longest
            // matching DB name to avoid a short name shadowing a specific one.
            if (report.find(name) != std::string::npos || name.find(report) != std::string::npos)
            {
                if (name.size() > bestLen)
                {
                    best = &model;
                    bestLen = name.size();
                }
            }
        }
        return best;
    }

    std::string SnmpGetModel(const std::string& host, int timeoutMs)
    {
        WinsockGuard ws;
        if (!ws.ok)
            return "";

        // Standard Printer MIB model OID: 1.3.6.1.2.1.25.3.2.1.3.1
        const std::vector<uint32_t> modelOid = { 1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 3, 1 };

        std::vector<uint8_t> resp = SnmpExchange(host, modelOid, timeoutMs);
        if (resp.empty())
            return "";

        uint8_t tag;
        std::vector<uint8_t> value;
        if (!ParseFirstVarbindValue(resp, tag, value) || tag != 0x04)
            return "";

        return std::string(value.begin(), value.end());
    }

    std::vector<LanPrinter> DiscoverLanPrinters()
    {
        std::vector<LanPrinter> found;
        std::mutex foundMutex;

        WinsockGuard ws;
        if (!ws.ok)
        {
            std::cerr << "[!] LAN discovery: Winsock init failed." << std::endl;
            return found;
        }

        // Collect this machine's local IPv4 addresses via the hostname, then
        // scan each /24 (mirrors the Python find_printers.py behaviour).
        char hostname[256] = { 0 };
        if (gethostname(hostname, sizeof(hostname)) != 0)
        {
            std::cerr << "[!] LAN discovery: could not resolve local hostname." << std::endl;
            return found;
        }

        std::vector<std::string> subnets; // "192.168.1." prefixes
        addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0)
        {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next)
            {
                sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                char ip[INET_ADDRSTRLEN] = { 0 };
                InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip));

                std::string ipStr(ip);
                if (ipStr.rfind("127.", 0) == 0)
                    continue;

                size_t dot = ipStr.find_last_of('.');
                if (dot == std::string::npos)
                    continue;
                std::string prefix = ipStr.substr(0, dot + 1);

                if (std::find(subnets.begin(), subnets.end(), prefix) == subnets.end())
                    subnets.push_back(prefix);
            }
            freeaddrinfo(res);
        }

        if (subnets.empty())
        {
            std::cerr << "[!] LAN discovery: no local IPv4 subnet found." << std::endl;
            return found;
        }

        for (const auto& prefix : subnets)
            std::cout << "[i] Scanning subnet " << prefix << "0/24 for Epson printers..." << std::endl;

        // Build the full host list, then hand it to a bounded worker pool.
        std::vector<std::string> hosts;
        for (const auto& prefix : subnets)
            for (int i = 1; i <= 254; ++i)
                hosts.push_back(prefix + std::to_string(i));

        std::atomic<size_t> next{ 0 };
        const int workerCount = 48;

        auto worker = [&]()
        {
            for (;;)
            {
                size_t idx = next.fetch_add(1);
                if (idx >= hosts.size())
                    return;

                std::string model = SnmpGetModel(hosts[idx], 500);
                if (model.empty())
                    continue;

                std::string upper = TrimAndUpper(model);
                if (upper.find("EPSON") == std::string::npos)
                    continue;

                std::lock_guard<std::mutex> lock(foundMutex);
                found.push_back({ hosts[idx], model });
                std::cout << "[+] Found: " << hosts[idx] << "  (" << model << ")" << std::endl;
            }
        };

        std::vector<std::thread> pool;
        for (int i = 0; i < workerCount; ++i)
            pool.emplace_back(worker);
        for (auto& t : pool)
            t.join();

        std::cout << "[i] Discovery complete. " << found.size() << " Epson printer(s) found." << std::endl;
        return found;
    }

    bool SnmpWasteReset(const std::string& host, const LanModel& model)
    {
        WinsockGuard ws;
        if (!ws.ok)
        {
            std::cerr << "[!] SNMP reset: Winsock init failed." << std::endl;
            return false;
        }

        std::cout << "\n[*] Starting SNMP waste-ink reset for " << model.name
                  << " at " << host << "..." << std::endl;
        std::cout << "[i] Writing " << model.reset.size() << " EEPROM value(s)." << std::endl;

        size_t okCount = 0;
        for (size_t i = 0; i < model.reset.size(); ++i)
        {
            uint16_t address = model.reset[i].first;
            uint8_t value = model.reset[i].second;

            std::vector<uint32_t> oid = BuildWriteOid(model, address, value);
            std::vector<uint8_t> resp = SnmpExchange(host, oid, 3000);

            if (resp.empty())
            {
                std::cerr << "[-] No response writing address " << address
                          << " (write " << (i + 1) << "/" << model.reset.size() << ")." << std::endl;
                std::cerr << "    Check the IP address, that the printer is on, and that SNMP is enabled." << std::endl;
                return false;
            }

            if (!ContainsAscii(resp, ":OK;"))
            {
                std::cerr << "[-] Printer rejected write to address " << address
                          << " (write " << (i + 1) << "/" << model.reset.size() << ")." << std::endl;
                std::cerr << "    The read/write keys may not match this firmware, or SNMP EEPROM"
                          << " access is disabled on this model." << std::endl;
                return false;
            }

            ++okCount;
            std::cout << "-> Write " << (i + 1) << " / " << model.reset.size()
                      << " | address " << address << " = " << static_cast<int>(value)
                      << " | OK" << std::endl;
        }

        std::cout << "[i] All " << okCount << " EEPROM write(s) acknowledged." << std::endl;
        return true;
    }

    bool LanSendPrintJob(const std::string& host, const std::vector<unsigned char>& job, const std::string& label)
    {
        WinsockGuard ws;
        if (!ws.ok)
        {
            std::cerr << "[!] LPR: Winsock init failed." << std::endl;
            return false;
        }
        if (job.empty())
            return false;

        std::cout << "\n[*] Sending " << label << " to " << host
                  << " over LPR (RFC 1179, port 515, " << job.size() << " bytes)..." << std::endl;

        sockaddr_in addr = { 0 };
        addr.sin_family = AF_INET;
        addr.sin_port = htons(515);
        if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            std::cerr << "[-] '" << host << "' is not a valid IPv4 address." << std::endl;
            return false;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            std::cerr << "[-] Could not create a network socket." << std::endl;
            return false;
        }

        // RFC 1179 asks the client source port to be in 721-731. Best-effort bind;
        // continue with an ephemeral port if none are free (matches pyprintlpr).
        for (u_short sp = 721; sp <= 731; ++sp)
        {
            sockaddr_in local = { 0 };
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port = htons(sp);
            if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0)
                break;
        }

        if (!ConnectWithTimeout(sock, addr, 5000))
        {
            std::cerr << "[-] Could not connect to " << host << ":515 (LPR)." << std::endl;
            std::cerr << "    Check the IP and that LPR/LPD printing is enabled on the printer." << std::endl;
            closesocket(sock);
            return false;
        }

        DWORD to = 8000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

        const std::string queue = "PASSTHRU"; // Epson raw pass-through queue
        const std::string hostname = LocalHostName();
        const std::string username = LocalUserName();
        int jobnum = static_cast<int>(GetTickCount() % 1000);

        char cf[80], df[80];
        snprintf(cf, sizeof(cf), "cfA%03d%s", jobnum, hostname.c_str());
        snprintf(df, sizeof(df), "dfA%03d%s", jobnum, hostname.c_str());

        // RFC 1179 control file: 'l' prints the data file leaving control bytes intact.
        std::string ctrl;
        ctrl += "H" + hostname + "\n";
        ctrl += "P" + username + "\n";
        ctrl += "J" + label + "\n";
        ctrl += "C" + hostname + "\n";
        ctrl += "N" + label + "\n";
        ctrl += std::string("l") + df + "\n";
        ctrl += std::string("U") + df + "\n";

        auto fail = [&](const char* msg) -> bool
        {
            std::cerr << "[-] " << msg << std::endl;
            std::string abort = std::string(1, '\x01') + queue + "\n";
            SendStr(sock, abort);
            closesocket(sock);
            return false;
        };

        // 0x02: receive a printer job for the queue.
        if (!SendStr(sock, std::string(1, '\x02') + queue + "\n") || !RecvAck(sock))
            return fail("Printer did not accept the LPR job (queue PASSTHRU).");

        // 0x02: receive control file (length + name), then content + NUL.
        if (!SendStr(sock, std::string(1, '\x02') + std::to_string(ctrl.size()) + " " + cf + "\n") || !RecvAck(sock))
            return fail("Printer rejected the LPR control-file header.");
        unsigned char nul = 0x00;
        if (!SendStr(sock, ctrl) || !SendAll(sock, &nul, 1) || !RecvAck(sock))
            return fail("Printer rejected the LPR control file.");

        // 0x03: receive data file (length + name), then content + NUL.
        if (!SendStr(sock, std::string(1, '\x03') + std::to_string(job.size()) + " " + df + "\n") || !RecvAck(sock))
            return fail("Printer rejected the LPR data-file header.");
        if (!SendAll(sock, job.data(), job.size()) || !SendAll(sock, &nul, 1) || !RecvAck(sock))
            return fail("Printer rejected the LPR data file.");

        closesocket(sock);
        std::cout << "[i] " << label << " sent over LPR. The printer should start shortly." << std::endl;
        return true;
    }

} // namespace ewr
