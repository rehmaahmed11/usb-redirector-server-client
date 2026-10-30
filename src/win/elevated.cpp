// Mini USB Bridge - elevated helper implementation
// SPDX-License-Identifier: MIT
#include "elevated.h"

#include <cstdlib>
#include <map>
#include <sstream>

#include "../client/usbip_cli.h"
#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../server/usbipd_cli.h"
#include "process.h"

namespace mub {
namespace {

std::string GetArg(const std::vector<std::string>& args, const std::string& key, const std::string& def = {}) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) return args[i + 1];
    }
    return def;
}

bool HasFlag(const std::vector<std::string>& args, const std::string& key) {
    for (const auto& a : args)
        if (a == key) return true;
    return false;
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return out;
}

ElevatedResult RunOp(const std::string& op, const std::string& extra) {
    ElevatedResult result;
    std::string resultFile = TempFilePath("mubresult.txt");
    std::string args = "--op " + op;
    if (!extra.empty()) args += " " + extra;
    args += " --result \"" + resultFile + "\"";

    std::string err;
    LogI("elev", "Requesting elevation: " + args);
    if (!RunElevated(ExePathW(), Widen(args), Widen(resultFile), 120000, &err)) {
        result.error = err;
        return result;
    }
    std::string text = ReadTextFile(resultFile);
    DeleteFileA(resultFile.c_str());
    auto kv = ParseKeyValues(text);
    result.ok = kv["ok"] == "1";
    result.message = kv["message"];
    if (kv.count("number")) result.number = std::atoi(kv["number"].c_str());
    if (!result.ok && result.message.empty()) {
        result.error = text.empty() ? "Elevated helper returned no result" : text;
    }
    return result;
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

bool ElevatedBind(const std::string& busid, ElevatedResult& out) {
    out = RunOp("bind", "--busid " + busid);
    return out.ok;
}

bool ElevatedUnbind(const std::string& busid, ElevatedResult& out) {
    out = RunOp("unbind", "--busid " + busid);
    return out.ok;
}

bool ElevatedStartUsbipd(ElevatedResult& out) {
    out = RunOp("start-usbipd", {});
    return out.ok;
}

bool ElevatedFirewallBlock(bool enable, ElevatedResult& out) {
    out = RunOp("firewall", std::string("--state ") + (enable ? "block" : "allow"));
    return out.ok;
}

bool ElevatedAttach(uint16_t tcpPort, const std::string& busid, ElevatedResult& out) {
    out = RunOp("attach", "--port " + std::to_string(tcpPort) + " --busid " + busid);
    return out.ok;
}

bool ElevatedDetach(uint16_t tcpPort, int hubPort, ElevatedResult& out) {
    out = RunOp("detach", "--port " + std::to_string(tcpPort) + " --hubport " + std::to_string(hubPort));
    return out.ok;
}

// --- the elevated side -----------------------------------------------------

int RunElevatedOperation(const std::vector<std::string>& args) {
    std::string op = GetArg(args, "--op");
    std::string resultFile = GetArg(args, "--result");
    std::string message;
    bool ok = false;
    int number = -1;

    Logger::Instance().SetLogFile(LogFilePath());
    LogI("elev", "Elevated operation: " + op);

    if (op == "bind") {
        std::string busid = GetArg(args, "--busid");
        bool needsElevation = false;
        ok = UsbipdCli::Instance().Bind(busid, message, &needsElevation);
    } else if (op == "unbind") {
        std::string busid = GetArg(args, "--busid");
        ok = UsbipdCli::Instance().Unbind(busid, message);
    } else if (op == "start-usbipd") {
        ok = UsbipdCli::Instance().EnsureService(message);
    } else if (op == "firewall") {
        bool block = GetArg(args, "--state") == "block";
        std::string cmd =
            std::string("netsh advfirewall firewall delete rule name=\"Mini USB Bridge raw USB/IP\" >nul 2>&1 & ") +
            (block ? std::string("netsh advfirewall firewall add rule name=\"Mini USB Bridge raw USB/IP\" "
                                 "dir=in action=block protocol=TCP localport=3240 profile=any")
                   : std::string("netsh advfirewall firewall add rule name=\"Mini USB Bridge raw USB/IP\" "
                                 "dir=in action=allow protocol=TCP localport=3240 profile=any"));
        ProcessResult res = RunProcess(Widen(cmd), 20000);
        ok = res.exitCode == 0;
        message = ok ? (block ? "Inbound TCP 3240 is now blocked by Windows Firewall"
                              : "Firewall block rule removed (usbipd default access restored)")
                     : Trim(res.Combined());
        if (ok) {
            Settings& s = GetSettings();
            s.firewallHardened = block;
            s.Save();
        }
    } else if (op == "attach") {
        uint16_t port = static_cast<uint16_t>(std::atoi(GetArg(args, "--port", "3240").c_str()));
        std::string busid = GetArg(args, "--busid");
        auto res = UsbipCli::Instance().Attach("127.0.0.1", port, busid, 40000);
        ok = res.ok;
        message = res.message;
        number = res.hubPort;
    } else if (op == "detach") {
        uint16_t port = static_cast<uint16_t>(std::atoi(GetArg(args, "--port", "3240").c_str()));
        int hubPort = std::atoi(GetArg(args, "--hubport", "0").c_str());
        auto res = UsbipCli::Instance().Detach(port, hubPort);
        ok = res.ok;
        message = res.message;
    } else {
        message = "Unknown elevated operation '" + op + "'";
    }

    std::string clean = message;
    for (char& c : clean)
        if (c == '\n' || c == '\r') c = ' ';
    std::string out = std::string("ok=") + (ok ? "1" : "0") + "\nnumber=" + std::to_string(number) +
                      "\nmessage=" + clean + "\n";
    if (!resultFile.empty()) WriteTextFile(resultFile, out);
    return ok ? 0 : 1;
}

}  // namespace mub
