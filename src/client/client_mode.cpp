// Mini USB Bridge - client engine implementation
// SPDX-License-Identifier: MIT
#include "client_mode.h"

#include <chrono>
#include <cstring>

#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../core/usbip_wire.h"
#include "../ui/ui_event.h"
#include "../win/crypto_win.h"
#include "../win/elevated.h"
#include "../win/process.h"
#include "../win/tls.h"
#include "usbip_cli.h"

namespace mub {
namespace {
constexpr int kConnectTimeoutMs = 8000;
constexpr int kHandshakeTimeoutMs = 15000;
constexpr int kAttachTimeoutMs = 40000;
constexpr int kConfirmTimeoutMs = 12000;
}  // namespace

ClientMode& ClientMode::Instance() {
    static ClientMode inst;
    return inst;
}

ClientMode::~ClientMode() { Disconnect("program exit"); }

void ClientMode::SetState(State s, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = s;
        stateDetail_ = detail;
    }
    auto ev = MakeUiEvent(UiKind::ClientState, StateText());
    ev->n = static_cast<uint64_t>(s);
    PostUi(std::move(ev));
}

ClientMode::State ClientMode::CurrentState() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string ClientMode::StateText() const {
    std::lock_guard<std::mutex> lock(mu_);
    switch (state_) {
        case State::Idle: return "Not connected";
        case State::Invalid: return stateDetail_.empty() ? "Invalid pairing code" : stateDetail_;
        case State::Connecting: return "Connecting to server...";
        case State::Authenticating: return "Authenticating...";
        case State::WaitingApproval: return "Waiting for server approval...";
        case State::Connected: return "Connected";
        case State::Reconnecting: return "Connection Lost - Waiting for Reconnection";
        case State::Error: return stateDetail_.empty() ? "Error" : stateDetail_;
    }
    return "Unknown";
}

bool ClientMode::Connect(const std::string& text, std::string* err) {
    if (running_.load()) {
        if (err) *err = "Already connected";
        return false;
    }
    PairingCode code;
    std::string decodeError;
    if (!DecodePairingCode(text, code, &decodeError)) {
        SetState(State::Invalid, decodeError);
        if (err) *err = decodeError;
        return false;
    }
    if (code.Expired(NowUnix())) {
        SetState(State::Invalid, "This pairing code has expired - ask the server for a new one");
        if (err) *err = "Pairing code expired";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        code_ = code;
        haveCode_ = true;
        haveToken_ = false;
        devices_.clear();
        attach_.clear();
        serverEndpoint_ = code.Endpoint();
        lastError_.clear();
    }

    Settings& s = GetSettings();
    tunnelPort_ = s.tunnelPort;
    StartTunnel();

    stop_.store(false);
    running_.store(true);
    controlThread_ = std::thread([this]() { RunLoop(); });
    if (err) err->clear();
    return true;
}

void ClientMode::Disconnect(const std::string& reason) {
    if (!running_.exchange(false)) {
        StopTunnel();
        return;
    }
    stop_.store(true);
    CloseChannel();
    if (controlThread_.joinable()) controlThread_.join();

    // Detach everything we attached, otherwise the virtual devices linger.
    if (GetSettings().autoDetachOnExit) {
        std::vector<DeviceEntry> attached;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& kv : attach_) {
                if (kv.second.attached) {
                    DeviceEntry e;
                    e.busid = kv.first;
                    attached.push_back(e);
                }
            }
        }
        if (!attached.empty()) {
            auto res = UsbipCli::Instance().DetachAll(tunnelPort_);
            LogI("client", "Detach all on disconnect: " + res.message);
        }
    }
    StopTunnel();
    {
        std::lock_guard<std::mutex> lock(mu_);
        devices_.clear();
        attach_.clear();
        haveToken_ = false;
        serverName_.clear();
        serverEndpoint_.clear();
        tlsInfo_.clear();
        transportPath_.clear();
    }
    LogI("client", "Disconnected: " + reason);
    SetState(State::Idle);
    PublishDevices();
    auto ev = MakeUiEvent(UiKind::ClientServer);
    PostUi(std::move(ev));
}

void ClientMode::CloseChannel() {
    std::lock_guard<std::mutex> lock(channelMu_);
    if (channel_) {
        channel_->Close();
        channel_.reset();
    }
}

bool ClientMode::OpenTransport(const PairingCode& code, const std::string& /*expected*/, SOCKET* out,
                               std::string* path, std::string* err) {
    Settings& s = GetSettings();
    std::string directError;
    if (code.HasDirect()) {
        SOCKET sock = TcpConnect(code.host, code.directPort, kConnectTimeoutMs, &directError);
        if (sock != INVALID_SOCKET) {
            *out = sock;
            *path = "Direct";
            return true;
        }
        LogW("client", "Direct connection failed (" + directError + "); trying relay");
    } else {
        directError = "the pairing code has no direct address";
    }
    if (code.HasRelay() && s.relayEnabled && !s.relayHost.empty()) {
        std::string relayError;
        SOCKET sock = ConnectRelay(s.relayHost, s.relayPort, 1, code.sessionId, RelayAuthTag(code), 30000,
                                   &relayError);
        if (sock != INVALID_SOCKET) {
            *out = sock;
            *path = "Relay";
            return true;
        }
        if (err) *err = "Direct connection failed (" + directError + "). Relay failed: " + relayError;
        return false;
    }
    if (err) {
        *err = "Cannot reach the server: " + directError +
               ". Ask the server user to put a reachable address in Settings > Public address "
               "(a Tailscale/ZeroTier IP, or a tunnel host:port such as name.playit.gg:25565), "
               "or to forward TCP port " + std::to_string(code.directPort) +
               " on the router, or to turn the relay on at both ends.";
    }
    return false;
}

bool ClientMode::TunnelAuth(const PairingCode& code, ControlChannel& channel, std::string* err) {
    // Used for data channels: token auth, then the raw USB/IP stream takes over.
    DataAuthBody auth;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::memcpy(auth.token, token_, kTokenLen);
    }
    ByteWriter w;
    auth.Write(w);
    if (!channel.Send(MsgType::DataAuth, w)) {
        if (err) *err = "Cannot send the data channel token";
        return false;
    }
    Message reply;
    if (!channel.Receive(reply, kHandshakeTimeoutMs) || reply.type != MsgType::DataOk) {
        if (err) *err = "Server did not accept the data channel";
        return false;
    }
    StatusBody st;
    ByteReader r(reply.payload);
    if (!StatusBody::Read(r, st) || st.status != Status::Ok) {
        if (err) *err = std::string("Server refused the data channel: ") + StatusText(st.status);
        return false;
    }
    (void)code;
    return true;
}

bool ClientMode::HandshakeAndAuth(ControlChannel& channel, const PairingCode& code, std::string* err) {
    HelloBody hello;
    hello.version = kProtocolVersion;
    hello.sessionId = code.sessionId;
    if (!RandomBytes(hello.nonceC, kNonceLen)) {
        if (err) *err = "System RNG failed";
        return false;
    }
    ByteWriter hw;
    hello.Write(hw);
    if (!channel.Send(MsgType::Hello, hw)) {
        if (err) *err = "Cannot send the hello message";
        return false;
    }
    Message reply;
    if (!channel.Receive(reply, kHandshakeTimeoutMs) || reply.type != MsgType::Challenge) {
        if (err) *err = "Server did not answer the handshake";
        return false;
    }
    ChallengeBody challenge;
    {
        ByteReader r(reply.payload);
        if (!ChallengeBody::Read(r, challenge)) {
            if (err) *err = "Malformed challenge from the server";
            return false;
        }
    }

    bool resuming = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resuming = haveToken_;
    }
    if (resuming) {
        ResumeBody resume;
        RandomBytes(resume.nonceC, kNonceLen);
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::memcpy(resume.token, token_, kTokenLen);
        }
        ByteWriter rw;
        resume.Write(rw);
        if (!channel.Send(MsgType::ResumeAuth, rw)) {
            if (err) *err = "Cannot send the resume request";
            return false;
        }
        SetState(State::Authenticating, "Resuming session...");
    } else {
        AuthBody auth;
        auth.clientName = LocalPCName();
        Sha256Digest proof = ComputeClientProof(code.secret, code.sessionId, hello.nonceC, challenge.nonceS,
                                                code.certPin, sizeof(code.certPin));
        std::memcpy(auth.mac, proof.data(), kMacLen);
        ByteWriter aw;
        auth.Write(aw);
        SetState(State::WaitingApproval);
        if (!channel.Send(MsgType::Auth, aw)) {
            if (err) *err = "Cannot send the authentication proof";
            return false;
        }
    }

    Message result;
    // The human on the server side has up to 60 s to answer.
    if (!channel.Receive(result, 90000) || result.type != MsgType::AuthResult) {
        if (err) *err = "The server closed the connection before answering";
        return false;
    }
    AuthResultBody body;
    {
        ByteReader r(result.payload);
        if (!AuthResultBody::Read(r, body)) {
            if (err) *err = "Malformed authentication result";
            return false;
        }
    }
    if (body.status != Status::Ok) {
        std::string detail = body.detail.empty() ? StatusText(body.status) : body.detail;
        if (body.status == Status::Rejected) {
            SetState(State::Error, "Access rejected by the server user");
        } else if (body.status == Status::Expired) {
            SetState(State::Error, "Pairing code expired");
        } else if (body.status == Status::AlreadyUsed) {
            SetState(State::Error, "This pairing code was already used - ask for a new one");
        } else {
            SetState(State::Error, detail);
        }
        if (err) *err = detail;
        return false;
    }
    // Verify the server also knows the secret (mutual authentication).
    Sha256Digest expected = ComputeServerProof(code.secret, code.sessionId, hello.nonceC, challenge.nonceS);
    if (!ConstantTimeEqual(expected.data(), body.mac, kMacLen)) {
        SetState(State::Error, "Server authentication failed (wrong pairing code or intercepting proxy)");
        if (err) *err = "Server proof mismatch";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::memcpy(token_, body.token, kTokenLen);
        haveToken_ = true;
        serverName_ = body.detail;
        tlsInfo_ = channel.Tls().CipherInfo();
    }
    return true;
}

bool ClientMode::SendControl(MsgType type, const ByteWriter& body) {
    std::lock_guard<std::mutex> lock(sendMu_);
    ControlChannel* ch = nullptr;
    {
        std::lock_guard<std::mutex> chanLock(channelMu_);
        ch = channel_.get();
    }
    if (!ch) return false;
    return ch->Send(type, body);
}

ClientMode::ChannelReply ClientMode::AwaitReply(const std::string& key, int timeoutMs) {
    std::unique_lock<std::mutex> lock(replyMu_);
    replyCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                      [&]() { return replies_[key].done; });
    ChannelReply reply = replies_[key];
    replies_.erase(key);
    return reply;
}

void ClientMode::RunLoop() {
    int attempt = 0;
    while (running_.load() && !stop_.load()) {
        PairingCode code;
        {
            std::lock_guard<std::mutex> lock(mu_);
            code = code_;
            attempt = 0;
        }
        SetState(State::Connecting);
        SOCKET sock = INVALID_SOCKET;
        std::string path;
        std::string err;
        if (!OpenTransport(code, code.Endpoint(), &sock, &path, &err)) {
            SetState(State::Error, err);
            LogE("client", err);
            running_.store(false);
            break;
        }
        LogI("client", "Transport: " + path + " to " + code.Endpoint());

        auto channel = std::make_unique<ControlChannel>();
        if (!channel->StartClient(sock, code.certPin, &err)) {
            SetState(State::Error, "Secure channel failed: " + err);
            LogE("client", "TLS: " + err);
            running_.store(false);
            break;
        }
        SetState(State::Authenticating);
        if (!HandshakeAndAuth(*channel, code, &err)) {
            channel->Close();
            {
                std::lock_guard<std::mutex> lock(mu_);
                lastError_ = err;
            }
            LogE("client", "Pairing failed: " + err);
            running_.store(false);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(channelMu_);
            channel_ = std::move(channel);
            std::lock_guard<std::mutex> lock2(mu_);
            transportPath_ = path;
            serverEndpoint_ = code.Endpoint();
        }
        SetState(State::Connected);
        PublishServerInfo();
        LogI("client", "Connected to " + serverName_ + " via " + path);

        // Request the device list (single reader rule: writes are serialized).
        {
            std::lock_guard<std::mutex> lock(sendMu_);
            std::lock_guard<std::mutex> chanLock(channelMu_);
            if (channel_) channel_->SendEmpty(MsgType::DeviceListRequest);
        }

        bool lost = false;
        std::string lostReason;
        while (running_.load() && !stop_.load()) {
            Message m;
            ControlChannel* ch = nullptr;
            {
                std::lock_guard<std::mutex> lock(channelMu_);
                ch = channel_.get();
            }
            if (!ch) break;
            if (!ch->Receive(m, 15000)) {
                if (ch->TimedOut()) {
                    ByteWriter pw;
                    NonceBody{static_cast<uint32_t>(NowUnix())}.Write(pw);
                    if (!SendControl(MsgType::Ping, pw)) {
                        lost = true;
                        lostReason = "keepalive failed";
                        break;
                    }
                    continue;
                }
                lost = true;
                lostReason = ch->LastError();
                break;
            }
            switch (m.type) {
                case MsgType::DeviceList: {
                    DeviceListBody list;
                    ByteReader r(m.payload);
                    if (DeviceListBody::Read(r, list)) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            devices_ = list.devices;
                        }
                        PublishDevices();
                    }
                    break;
                }
                case MsgType::DeviceEvent: {
                    DeviceEventBody ev;
                    ByteReader r(m.payload);
                    if (DeviceEventBody::Read(r, ev)) {
                        std::lock_guard<std::mutex> lock(mu_);
                        bool found = false;
                        for (auto& d : devices_) {
                            if (!d.busid.empty() && d.busid == ev.device.busid) {
                                if (ev.kind == 1) {
                                    d.state = 1;
                                    d.shared = 0;
                                } else {
                                    d = ev.device;
                                }
                                found = true;
                                break;
                            }
                        }
                        if (!found && ev.kind == 0) devices_.push_back(ev.device);
                    }
                    PublishDevices();
                    break;
                }
                case MsgType::AttachResult:
                case MsgType::DetachResult: {
                    ResultBody res;
                    ByteReader r(m.payload);
                    if (ResultBody::Read(r, res)) {
                        {
                            std::lock_guard<std::mutex> lock(replyMu_);
                            ChannelReply& slot = replies_[res.busid];
                            slot.done = true;
                            slot.ok = res.status == Status::Ok;
                            slot.status = res.status;
                            slot.message = res.message;
                        }
                        replyCv_.notify_all();
                        if (res.status != Status::Ok) {
                            auto ev = MakeUiEvent(UiKind::Banner, res.message, "error");
                            PostUi(std::move(ev));
                        }
                    }
                    break;
                }
                case MsgType::Ping: {
                    NonceBody nb;
                    ByteReader r(m.payload);
                    NonceBody::Read(r, nb);
                    ByteWriter w;
                    nb.Write(w);
                    SendControl(MsgType::Pong, w);
                    break;
                }
                case MsgType::Pong: break;
                case MsgType::Bye: {
                    ByeBody bye;
                    ByteReader r(m.payload);
                    ByeBody::Read(r, bye);
                    lost = true;
                    lostReason = bye.reason.empty() ? "server closed the session" : bye.reason;
                    break;
                }
                default: break;
            }
            if (lost) break;
        }

        CloseChannel();
        if (!running_.load() || stop_.load()) break;

        // Network interruption: report it and try to resume with the session token.
        SetState(State::Reconnecting, lostReason);
        LogW("client", "Connection lost: " + lostReason);
        if (++attempt > 20) {
            SetState(State::Error, "Connection lost - giving up after repeated failures");
            running_.store(false);
            break;
        }
        for (int i = 0; i < 10 && running_.load() && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    running_.store(false);
}

void ClientMode::PublishDevices() {
    auto ev = MakeUiEvent(UiKind::ClientDevices);
    {
        std::lock_guard<std::mutex> lock(mu_);
        ev->devices = devices_;
    }
    PostUi(std::move(ev));
}

void ClientMode::PublishServerInfo() {
    std::lock_guard<std::mutex> lock(mu_);
    auto ev = MakeUiEvent(UiKind::ClientServer, serverName_, serverEndpoint_);
    ev->c = transportPath_ + (tlsInfo_.empty() ? "" : ("  |  " + tlsInfo_));
    PostUi(std::move(ev));
}

std::string ClientMode::ServerInfoText() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (serverName_.empty() && serverEndpoint_.empty()) return {};
    std::string txt = serverName_.empty() ? "Server" : serverName_;
    if (!serverEndpoint_.empty()) txt += "  |  " + serverEndpoint_;
    if (!transportPath_.empty()) txt += "  |  " + transportPath_;
    return txt;
}

std::string ClientMode::EndpointText() const {
    std::lock_guard<std::mutex> lock(mu_);
    return serverEndpoint_;
}

std::vector<DeviceEntry> ClientMode::DevicesSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_;
}

std::map<std::string, ClientMode::AttachState> ClientMode::AttachStates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attach_;
}

bool ClientMode::RefreshDevices(std::string* err) {
    ControlChannel* ch = nullptr;
    {
        std::lock_guard<std::mutex> lock(channelMu_);
        ch = channel_.get();
    }
    if (!ch) {
        if (err) *err = "Not connected";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sendMu_);
        if (!ch->SendEmpty(MsgType::DeviceListRequest)) {
            if (err) *err = "Cannot send the request";
            return false;
        }
    }
    if (err) err->clear();
    return true;
}

bool ClientMode::AttachDevice(const DeviceEntry& device, std::string& message) {
    if (device.busid.empty()) {
        message = "This device has no bus id yet";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (attach_[device.busid].attached) {
            message = "Device is already attached";
            return false;
        }
        attach_[device.busid].busy = true;
    }
    auto fail = [&](const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            attach_[device.busid].busy = false;
        }
        message = msg;
        auto ev = MakeUiEvent(UiKind::Banner, msg, "error");
        PostUi(std::move(ev));
        return false;
    };

    // 1. Ask the server to export the device (server-side authorisation).
    {
        std::lock_guard<std::mutex> lock(channelMu_);
        if (!channel_) return fail("Not connected to the server");
    }
    ByteWriter w;
    BusidBody{device.busid}.Write(w);
    if (!SendControl(MsgType::RequestAttach, w)) return fail("Cannot send the attach request");
    ChannelReply reply = AwaitReply(device.busid, 90000);
    if (!reply.done) return fail("The server did not answer the attach request");
    if (!reply.ok) return fail(reply.message.empty() ? StatusText(reply.status) : reply.message);

    // 2. Make sure our loopback tunnel is listening on the port usbip.exe will use.
    StartTunnel();
    if (tunnelListener_ == INVALID_SOCKET) return fail("The local tunnel could not be started");

    // 3. Attach through the real client driver.
    auto attach = UsbipCli::Instance().Attach("127.0.0.1", tunnelPort_, device.busid, kAttachTimeoutMs);
    if (!attach.ok && attach.needsElevation) {
        ElevatedResult er;
        if (ElevatedAttach(tunnelPort_, device.busid, er)) {
            attach.ok = true;
            attach.hubPort = er.number;
            attach.message = er.message;
        } else {
            attach.message = er.message.empty() ? er.error : er.message;
        }
    }
    if (!attach.ok) {
        // Be explicit: the server still exports the device, we just could not import it.
        if (attach.message.empty()) attach.message = "usbip.exe attach failed";
        return fail("USB/IP attach failed: " + attach.message);
    }

    // 4. Only claim success when the device is really there.
    bool confirmed = true;
    if (GetSettings().verifyAttachWithSetupApi) {
        confirmed = UsbipCli::Instance().ConfirmDevicePresent(device.vid, device.pid, kConfirmTimeoutMs);
    }
    if (!confirmed) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            attach_[device.busid].attached = false;
            attach_[device.busid].hubPort = attach.hubPort;
            attach_[device.busid].busy = false;
        }
        UsbipCli::Instance().Detach(tunnelPort_, attach.hubPort);  // never leave a phantom port behind
        return fail("usbip.exe reported success, but no USB device " + HexU16(device.vid) + ":" +
                    HexU16(device.pid) +
                    " appeared in Device Manager. The attach is NOT trusted (check the usbip-win2 driver).");
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        attach_[device.busid].attached = true;
        attach_[device.busid].hubPort = attach.hubPort;
        attach_[device.busid].busy = false;
        for (auto& d : devices_)
            if (d.busid == device.busid) d.attached = 1;
    }
    message = "Attached " + device.name + " (bus " + device.busid + ")" +
              (attach.hubPort > 0 ? ", hub port " + std::to_string(attach.hubPort) : "");
    LogI("client", message);
    PublishDevices();
    auto ev = MakeUiEvent(UiKind::Banner, message, "info");
    PostUi(std::move(ev));
    return true;
}

bool ClientMode::DetachDevice(const DeviceEntry& device, std::string& message) {
    int hubPort = -1;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = attach_.find(device.busid);
        if (it != attach_.end()) hubPort = it->second.hubPort;
    }
    auto detach = UsbipCli::Instance().Detach(tunnelPort_, hubPort);
    if (!detach.ok) {
        ElevatedResult er;
        if (ElevatedDetach(tunnelPort_, hubPort, er)) detach.ok = true;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        attach_[device.busid] = AttachState{};
        for (auto& d : devices_)
            if (d.busid == device.busid) d.attached = 0;
    }
    // Tell the server so it can release the export.
    ByteWriter w;
    BusidBody{device.busid}.Write(w);
    SendControl(MsgType::RequestDetach, w);
    PublishDevices();
    message = detach.ok ? ("Detached " + device.name) : ("Detach failed: " + detach.message);
    auto ev = MakeUiEvent(UiKind::Banner, message, detach.ok ? "info" : "error");
    PostUi(std::move(ev));
    return detach.ok;
}

// --- loopback tunnel -------------------------------------------------------

void ClientMode::StartTunnel() {
    if (tunnelListener_ != INVALID_SOCKET) return;
    std::string err;
    SOCKET s = TcpListen(tunnelPort_, true, &err);
    if (s == INVALID_SOCKET) {
        // Fall back to the USB/IP default port so old usbip.exe builds still work.
        if (tunnelPort_ != 3240) {
            SOCKET alt = TcpListen(3240, true, &err);
            if (alt != INVALID_SOCKET) {
                tunnelPort_ = 3240;
                s = alt;
                LogW("client", "Tunnel port " + std::to_string(GetSettings().tunnelPort) +
                                   " was busy; using 3240 instead");
            }
        }
        if (s == INVALID_SOCKET) {
            LogE("client", "Cannot open the loopback tunnel: " + err);
            auto ev = MakeUiEvent(UiKind::Banner,
                                  "Cannot open the local tunnel on 127.0.0.1:" +
                                      std::to_string(tunnelPort_) + " - " + err,
                                  "error");
            PostUi(std::move(ev));
            return;
        }
    }
    tunnelListener_ = s;
    tunnelStop_.store(false);
    tunnelThread_ = std::thread([this]() { TunnelAcceptLoop(); });
    LogI("client", "Loopback tunnel listening on 127.0.0.1:" + std::to_string(tunnelPort_));
}

void ClientMode::StopTunnel() {
    tunnelStop_.store(true);
    CloseSocket(tunnelListener_);
    if (tunnelThread_.joinable()) tunnelThread_.join();
}

void ClientMode::TunnelAcceptLoop() {
    while (!tunnelStop_.load()) {
        std::string peer;
        SOCKET sock = TcpAccept(tunnelListener_, &peer, 500);
        if (sock == INVALID_SOCKET) continue;
        if (!IsLoopbackPeer(sock)) {
            // Never proxy USB/IP for anything but this machine.
            LogW("client", "Refused a non-loopback connection to the tunnel from " + peer);
            CloseSocket(sock);
            continue;
        }
        std::thread([this, sock]() { HandleTunnelConnection(sock); }).detach();
    }
}

void ClientMode::HandleTunnelConnection(SOCKET client) {
    activeChannels_.fetch_add(1);
    PairingCode code;
    {
        std::lock_guard<std::mutex> lock(mu_);
        code = code_;
    }
    std::string err;
    std::string path;
    SOCKET server = INVALID_SOCKET;
    if (!OpenTransport(code, code.Endpoint(), &server, &path, &err)) {
        LogE("client", "Data channel could not reach the server: " + err);
        CloseSocket(client);
        activeChannels_.fetch_sub(1);
        return;
    }
    auto channel = std::make_unique<ControlChannel>();
    if (!channel->StartClient(server, code.certPin, &err)) {
        LogE("client", "Data channel TLS failed: " + err);
        CloseSocket(client);
        activeChannels_.fetch_sub(1);
        return;
    }
    if (!TunnelAuth(code, *channel, &err)) {
        LogE("client", "Data channel rejected: " + err);
        channel->Close();
        CloseSocket(client);
        activeChannels_.fetch_sub(1);
        return;
    }
    LogI("client", "USB/IP data channel established (" + path + ")");
    channel->ReleaseRawSocketForPump();
    std::atomic<bool> stopPump{false};
    PipeTlsToSocket(*channel, client, stopPump);
    CloseSocket(client);
    activeChannels_.fetch_sub(1);
    LogI("client", "USB/IP data channel closed");
}

}  // namespace mub
