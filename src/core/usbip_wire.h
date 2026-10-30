// Mini USB Bridge - minimal USB/IP wire inspection
// SPDX-License-Identifier: MIT
//
// We do not implement USB/IP; we only *inspect* the first message of a data
// connection so that the tunnel can enforce device-level access control:
// a client may only import a busid the server user actually shared.
//
// Header layout (all big endian), see the Linux USB/IP protocol document:
//   u16 version ; u16 command ; u32 status
//   OP_REQ_IMPORT (0x8003) additionally carries: char busid[32]
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mub {

constexpr size_t kUsbipHeaderLen = 8;
constexpr size_t kUsbipImportLen = kUsbipHeaderLen + 32;
constexpr uint16_t kUsbipVersion111 = 0x0111;
constexpr uint16_t kUsbipOpReqImport = 0x8003;
constexpr uint16_t kUsbipOpRepImport = 0x0003;

struct UsbipImport {
    uint16_t version = 0;
    uint16_t command = 0;
    uint32_t status = 0;
    std::string busid;
};

// Parses the first 40 bytes of an OP_REQ_IMPORT. Returns false when the buffer
// is not a well-formed import request (wrong version/command/short/empty busid).
bool ParseUsbipImport(const uint8_t* data, size_t len, UsbipImport& out, std::string* error);

// Builds a synthetic OP_REQ_IMPORT (used by tests and diagnostics).
void BuildUsbipImport(const std::string& busid, uint8_t out[kUsbipImportLen]);

}  // namespace mub
