// Mini USB Bridge - client engine (USB/IP import side)
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/pairing.h"
#include "../core/protocol.h"
#include "../net/channel.h"
#include "../win/net.h"

namespace mub {

class ClientMode {
public:
    enum class State {
        Idle,
        Invalid,
        Connecting,
        Authenticating,
        WaitingApproval,
        Connected,
        Reconnecting,
        Error
    };

    static ClientMode& Instance();

    bool Connect(const std::string& pairingCodeText, std::string* err);
    void Disconnect(const std::string& reason);
    bool Running() const { return running_.load(); }
    State CurrentState() const;
    std::string StateText() const;

    struct AttachState {
        bool attached = false;
        int hubPort = -1;
        bool busy = false;
    };
    // Requests the device from the server and attaches it through usbip.exe.
    // Blocks until the backend confirms or fails; never reports a fake success.
    bool AttachDevice(const DeviceEntry& device, std::string& message);
    bool DetachDevice(const DeviceEntry& device, std::string& message);
    bool RefreshDevices(std::string* err);

    std::vector<DeviceEntry> DevicesSnapshot() const;
    std::map<std::string, AttachState> AttachStates() const;
    std::string ServerInfoText() const;
    std::string EndpointText() const;
    uint16_t TunnelPort() const { return tunnelPort_; }

private:
    ClientMode() = default;
    ~ClientMode();
    ClientMode(const ClientMode&) = delete;
    ClientMode& operator=(const ClientMode&) = delete;

    void RunLoop();
    bool OpenTransport(const PairingCode& code, const std::string& expected, SOCKET* out, std::string* path,
                       std::string* err);
    bool HandshakeAndAuth(ControlChannel& channel, const PairingCode& code, std::string* err);
    void SetState(State s, const std::string& detail = {});
    void PublishDevices();
    void PublishServerInfo();
    void StartTunnel();
    void StopTunnel();
    void TunnelAcceptLoop();
    void HandleTunnelConnection(SOCKET client);
    void CloseChannel();
    bool TunnelAuth(const PairingCode& code, ControlChannel& channel, std::string* err);

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread controlThread_;

    mutable std::mutex mu_;
    PairingCode code_;
    bool haveCode_ = false;
    State state_ = State::Idle;
    std::string stateDetail_;
    std::vector<DeviceEntry> devices_;
    std::map<std::string, AttachState> attach_;
    std::string serverName_;
    std::string serverEndpoint_;
    std::string transportPath_;
    std::string tlsInfo_;
    uint8_t token_[kTokenLen] = {};
    bool haveToken_ = false;
    std::string lastError_;

    std::mutex channelMu_;
    std::unique_ptr<ControlChannel> channel_;

    // The control channel has exactly one reader (RunLoop). Requests that need an
    // answer wait here instead of reading the socket themselves.
    struct ChannelReply {
        bool done = false;
        bool ok = false;
        Status status = Status::Internal;
        std::string message;
    };
    std::mutex sendMu_;              // serializes writes to the control channel
    std::mutex replyMu_;
    std::condition_variable replyCv_;
    std::map<std::string, ChannelReply> replies_;
    bool SendControl(MsgType type, const ByteWriter& body);
    ChannelReply AwaitReply(const std::string& key, int timeoutMs);

    // Loopback tunnel (usbip.exe -> here -> TLS -> server)
    SOCKET tunnelListener_ = INVALID_SOCKET;
    std::thread tunnelThread_;
    uint16_t tunnelPort_ = 4240;
    std::atomic<bool> tunnelStop_{false};
    std::atomic<int> activeChannels_{0};
};

}  // namespace mub
