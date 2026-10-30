// Mini USB Bridge - server engine (USB/IP export side)
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/pairing.h"
#include "../core/protocol.h"
#include "../win/net.h"
#include "usb_detect.h"

namespace mub {

struct TlsIdentity;
class ControlChannel;
struct ApprovalRequest;

class ServerMode {
public:
    enum class State { Idle, Starting, WaitingClient, ApprovalPending, ClientConnected, Error };

    static ServerMode& Instance();

    void SetIdentity(std::shared_ptr<TlsIdentity> identity) { identity_ = std::move(identity); }
    bool Start(std::string* err);
    void Stop();
    bool Running() const { return running_.load(); }

    // Pairing code lifecycle
    void RegenerateCode();
    std::string CodeText() const;
    uint64_t CodeExpiresUnix() const;
    std::string EndpointText() const;
    // "LAN only - set Public address in Settings" / "public address OK" / ...
    std::string ReachabilityNote() const;
    std::string TransportText() const;

    State CurrentState() const;
    std::string StateText() const;

    // Device sharing (server user actions)
    bool Share(const std::string& busid, std::string& message);
    bool StopSharing(const std::string& busid, std::string& message);
    bool ShareAll(std::string& message);
    bool StopSharingAll(std::string& message);
    void RefreshDevices();

    void RespondToApproval(bool allow);

    void DisconnectClient(const std::string& reason);
    std::vector<UsbDevice> DevicesSnapshot() const;
    std::string ClientInfoText() const;

private:
    ServerMode() = default;
    ~ServerMode();
    ServerMode(const ServerMode&) = delete;
    ServerMode& operator=(const ServerMode&) = delete;

    void AcceptLoop();
    void RelayLoop();
    void WatchdogLoop();
    void HandleConnection(SOCKET sock, const std::string& path);
    void RunControlLoop(ControlChannel& ch, const std::string& clientName);
    void HandleDataChannel(ControlChannel& ch, const DataAuthBody& auth, const std::string& peer,
                           const std::string& path);
    bool TryResumeSession(ControlChannel& ch, const HelloBody& hello, const std::string& peer,
                          const std::string& path);
    bool RunApproval(const std::string& clientName, const std::string& peer, const std::string& path,
                     std::string* reason);
    void SetState(State s, const std::string& detail = {});
    void PublishDevices();
    void PublishCode();
    std::vector<DeviceEntry> BuildDeviceEntries() const;
    bool EnsureBound(const std::string& busid, std::string& message);
    void RevokeClientLocked(const std::string& reason);
    void MarkClientDisconnectedLocked();

    std::shared_ptr<TlsIdentity> identity_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    SOCKET listener_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::thread relayThread_;
    std::thread watchdogThread_;
    std::vector<std::thread> workers_;

    mutable std::mutex mu_;
    PairingCode code_;
    bool codeUsed_ = false;
    uint32_t codeExpiry_ = 0;
    int authFailures_ = 0;
    State state_ = State::Idle;
    std::string stateDetail_;
    std::vector<UsbDevice> devices_;
    std::set<std::string> sharedBusids_;
    std::set<std::string> allowedBusids_;
    std::string clientName_;
    std::string clientPeer_;
    std::string clientPath_;
    uint8_t token_[kTokenLen] = {};
    bool tokenValid_ = false;
    uint32_t tokenExpiry_ = 0;
    // Approved session (survives a network drop so the client can resume with its token)
    uint64_t sessionId_ = 0;
    uint8_t sessionSecret_[kSecretLen] = {};
    bool sessionValid_ = false;
    bool clientConnected_ = false;
    std::shared_ptr<ApprovalRequest> pendingApproval_;
    std::string lastError_;
    uint32_t connectionSerial_ = 0;
};

}  // namespace mub
