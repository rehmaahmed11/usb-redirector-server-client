// Mini USB Bridge - USB/IP header inspection
// SPDX-License-Identifier: MIT
#include "usbip_wire.h"

#include <cstring>

#include "protocol.h"  // Hex()
#include "strings.h"

namespace mub {

bool ParseUsbipImport(const uint8_t* data, size_t len, UsbipImport& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (len < kUsbipImportLen) return fail("USB/IP import request is incomplete");
    out.version = static_cast<uint16_t>((data[0] << 8) | data[1]);
    out.command = static_cast<uint16_t>((data[2] << 8) | data[3]);
    out.status = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);
    if (out.version != kUsbipVersion111) {
        if (error)
            *error = "Unsupported USB/IP version 0x" + Hex(reinterpret_cast<const void*>(data), 2);
        return false;
    }
    if (out.command != kUsbipOpReqImport) return fail("First USB/IP message is not OP_REQ_IMPORT");

    char raw[33];
    std::memcpy(raw, data + kUsbipHeaderLen, 32);
    raw[32] = '\0';
    out.busid = raw;  // stops at the first NUL, as the kernel does
    if (out.busid.empty()) return fail("USB/IP import request has an empty busid");
    if (!IsValidBusId(out.busid)) {
        if (error) *error = "Malformed busid '" + out.busid + "'";
        return false;
    }
    return true;
}

void BuildUsbipImport(const std::string& busid, uint8_t out[kUsbipImportLen]) {
    std::memset(out, 0, kUsbipImportLen);
    out[0] = static_cast<uint8_t>(kUsbipVersion111 >> 8);
    out[1] = static_cast<uint8_t>(kUsbipVersion111);
    out[2] = static_cast<uint8_t>(kUsbipOpReqImport >> 8);
    out[3] = static_cast<uint8_t>(kUsbipOpReqImport);
    size_t n = busid.size() > 31 ? 31 : busid.size();
    std::memcpy(out + kUsbipHeaderLen, busid.data(), n);
}

}  // namespace mub
