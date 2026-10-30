// Mini USB Bridge - elevated helper operations
// SPDX-License-Identifier: MIT
//
// The GUI runs unelevated. Operations that genuinely need administrator rights
// (usbipd bind/unbind, service start, firewall rule, usbip attach) are executed
// by re-launching this same executable with "--op <name>" via ShellExecuteEx
// "runas", which shows the normal UAC prompt. Results come back through a temp
// file, so no IPC surface is exposed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

struct ElevatedResult {
    bool ok = false;
    std::string message;
    int number = -1;
    std::string error;
};

bool ElevatedBind(const std::string& busid, ElevatedResult& out);
bool ElevatedUnbind(const std::string& busid, ElevatedResult& out);
bool ElevatedStartUsbipd(ElevatedResult& out);
bool ElevatedFirewallBlock(bool enable, ElevatedResult& out);
bool ElevatedAttach(uint16_t tcpPort, const std::string& busid, ElevatedResult& out);
bool ElevatedDetach(uint16_t tcpPort, int hubPort, ElevatedResult& out);

// Entry point used by main() when the process was started with --op.
int RunElevatedOperation(const std::vector<std::string>& args);

}  // namespace mub
