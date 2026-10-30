// Mini USB Bridge - server engine implementation
// SPDX-License-Identifier: MIT
#include "server_mode.h"

#include <chrono>
#include <cstring>

#include "../core/logger.h"
#include "../core/session_auth.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../core/usbip_wire.h"
#include "../net/channel.h"
#include "../ui/ui_event.h"
#include "../win/crypto_win.h"
#include "../win/elevated.h"
#include "../win/process.h"
#include "../win/tls.h"
#include "usbipd_cli.h"

namespace mub {
namespace {

constexpr int kPairingTimeoutMs = 12000;
constexpr int kChallengeTimeoutMs = 12000;
constexpr int kApprovalTimeoutMs = 60000;
constexpr int kMaxAuthFailures = kDefaultMaxAuthFailures;
constexpr uint16_t kUsbipdPort = 3240;
constexpr uint32_t kTokenTtlSeconds = 8 * 3600;

}  // namespace

ServerMode& ServerMode::Instance() {
    static ServerMode inst;
    return inst;
}

ServerMode::~ServerMode() { Stop(); }

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool ServerMode::Start(std::string* err) {
    if (running_.load()) return true;
    if (!identity_) {
        if (err) *err = "TLS identity is not initialized";
        return false;
    }
    Settings& s = GetSettings();

    UsbipdInfo info = UsbipdCli::Instance().Probe(true);
    if (!info.installed) {
        if (err) *err = info.detail;
        return false;
    }
    if (!info.serviceRunning || !info.loopbackReachable) {
        std::string msg;
        if (!UsbipdCli::Instance().EnsureService(msg)) {
            ElevatedResult er;
            if (!ElevatedStartUsbipd(er)) {
                if (err) {
                    *err = "The usbipd service is not running. Start it as administrator or reinstall "
                           "usbipd-win. Details: " + (er.message.empty() ? msg : er.message);
                }
                return false;
            }
        }
    }

    stop_.store(false);
    listener_ = TcpListen(s.serverPort, false, err);
    if (listener_ == INVALID_SOCKET) {
        if (err) *err = "Cannot listen on TCP port " + std::to_string(s.serverPort) + ". " + *err;
        return false;
    }

    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = State::Starting;
        lastError_.clear();
    }
    RegenerateCode();
    SetState(State::WaitingClient, "Waiting for Client");

    acceptThread_ = std::thread([this]() { AcceptLoop(); });
    if (s.relayEnabled && !s.relayHost.empty()) {
        relayThread_ = std::thread([this]() { RelayLoop(); });
    }
    watchdogThread_ = std::thread([this]() { WatchdogLoop(); });

    LogI("server", "Server started on port " + std::to_string(s.serverPort) +
                       (s.relayEnabled ? (" with relay " + s.relayHost) : ""));
    if (err) err->clear();
    return true;
}

void ServerMode::Stop() {
    if (!running_.exchange(false)) return;
    stop_.store(true);
    CloseSocket(listener_);
    if (acceptThread_.joinable()) acceptThread_.join();
    if (relayThread_.joinable()) relayThread_.join();
    if (watchdogThread_.joinable()) watchdogThread_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        RevokeClientLocked("server stopped");
        state_ = State::Idle;
    }
    LogI("server", "Server stopped");
}

void ServerMode::SetState(State s, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = s;
        stateDetail_ = detail;
    }
    auto ev = MakeUiEvent(UiKind::ServerState, StateText());
    ev->n = static_cast<uint64_t>(s);
    PostUi(std::move(ev));
}

ServerMode::State ServerMode::CurrentState() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string ServerMode::StateText() const {
    std::lock_guard<std::mutex> lock(mu_);
    switch (state_) {
        case State::Idle: return "Stopped";
        case State::Starting: return "Starting Server...";
        case State::WaitingClient:
            return codeUsed_ ? "Client approved - regenerate the code for a new client" : "Waiting for Client";
        case State::ApprovalPending: return "A client is requesting access";
        case State::ClientConnected: return "Client connected";
        case State::Error: return stateDetail_.empty() ? "Error" : stateDetail_;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// pairing code
// ---------------------------------------------------------------------------

void ServerMode::RegenerateCode() {
    Settings& s = GetSettings();
    PairingCode code;
    code.version = kPairVersion;
    code.sessionId = RandomSessionId();
    if (!RandomBytes(code.secret, kSecretLen)) {
        LogE("server", "System RNG failed - refusing to create a pairing code");
        SetState(State::Error, "Secure random number generator failed");
        return;
    }
    code.expiresUnix = static_cast<uint32_t>(NowUnix() + s.codeTtlSeconds);
    if (identity_) std::memcpy(code.certPin, identity_->pin, sizeof(code.certPin));

    // The advertised address may be a plain host ("100.64.0.5" from Tailscale, or a
    // DDNS name) or "host:port" - a free tunnel such as playit.gg publishes a random
    // port, so the port inside the code has to be able to differ from the local
    // listener. Our own listener always stays on serverPort.
    std::string host = s.publicHost;
    uint16_t advertisedPort = s.serverPort;
    if (host.empty()) {
        host = LocalIPv4();
    } else {
        std::string parsedHost;
        uint16_t parsedPort = 0;
        SplitHostPort(host, &parsedHost, &parsedPort);
        if (!parsedHost.empty()) host = parsedHost;
        if (parsedPort) advertisedPort = parsedPort;
        if (host.size() > kMaxHostLen) {
            LogW("server", "Public address is longer than " + std::to_string(kMaxHostLen) +
                               " characters and was truncated");
            host = host.substr(0, kMaxHostLen);
        }
    }
    if (!host.empty()) {
        code.flags |= kPairFlagDirect;
        code.directPort = advertisedPort;
        code.host = host;
        if (IsPrivateIPv4(host)) code.flags |= kPairFlagLanOnly;
    }
    if (s.relayEnabled && !s.relayHost.empty()) code.flags |= kPairFlagRelay;
    LogI("server", "Advertised address: " + (host.empty() ? std::string("-") : code.Endpoint()) +
                       (code.flags & kPairFlagRelay ? " (relay also available)" : ""));

    {
        std::lock_guard<std::mutex> lock(mu_);
        // Security: regenerating invalidates the previous code immediately. An
        // already approved session keeps its token so a running client is not cut off.
        code_ = code;
        codeExpiry_ = code.expiresUnix;
        codeUsed_ = false;
        authFailures_ = 0;
    }
    LogI("server", "Pairing code generated, expires " + FormatUtc(code.expiresUnix));
    PublishCode();
    if (CurrentState() == State::Error) SetState(State::WaitingClient, "Waiting for Client");
}

std::string ServerMode::CodeText() const {
    std::lock_guard<std::mutex> lock(mu_);
    return EncodePairingCode(code_);
}

uint64_t ServerMode::CodeExpiresUnix() const {
    std::lock_guard<std::mutex> lock(mu_);
    return codeExpiry_;
}

std::string ServerMode::EndpointText() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string txt = code_.HasDirect() ? code_.Endpoint() : std::string("-");
    if (code_.HasRelay()) txt += "  + relay";
    return txt;
}

// Short, honest answer to "can my friend on another network use this code?".
std::string ServerMode::ReachabilityNote() const {
    std::lock_guard<std::mutex> lock(mu_);
    const bool relay = (code_.flags & kPairFlagRelay) != 0;
    if (!code_.HasDirect()) return relay ? "relay only" : "no route";
    if (IsPrivateIPv4(code_.host)) {
        return relay ? "LAN address - relay handles the internet"
                     : "LAN only - set Public address in Settings";
    }
    return "public address OK";
}

std::string ServerMode::TransportText() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (clientPath_ == "Relay") return "Relay (encrypted)";
    if (clientPath_ == "Direct") return "Direct (encrypted)";
    return "-";
}

void ServerMode::PublishCode() {
    auto ev = MakeUiEvent(UiKind::ServerCode, CodeText());
    ev->n = CodeExpiresUnix();
    PostUi(std::move(ev));
}

// ---------------------------------------------------------------------------
// devices
// ---------------------------------------------------------------------------

void ServerMode::PublishDevices() {
    auto ev = MakeUiEvent(UiKind::ServerDevices);
    ev->devices = BuildDeviceEntries();
    PostUi(std::move(ev));
}

std::vector<DeviceEntry> ServerMode::BuildDeviceEntries() const {
    std::vector<DeviceEntry> out;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& d : devices_) {
        DeviceEntry e;
        e.busid = d.busid;
        e.instanceId = d.instanceId;
        e.name = d.DisplayName();
        e.manufacturer = d.manufacturer;
        e.vid = d.vid;
        e.pid = d.pid;
        e.state = d.present ? 0 : 1;
        e.shared = d.shared ? 1 : 0;
        e.attached = d.attached ? 1 : 0;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<UsbDevice> ServerMode::DevicesSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return devices_;
}

void ServerMode::RefreshDevices() {
    std::string err;
    std::vector<UsbDevice> devs = EnumerateUsbDevices(&err);
    std::map<std::string, std::string> instanceToBusid;
    UsbipdCli::Instance().ReadState(instanceToBusid, nullptr);
    std::vector<UsbipdCli::ListRow> rows;
    UsbipdCli::Instance().ReadList(rows, nullptr);

    for (auto& d : devs) {
        auto it = instanceToBusid.find(d.instanceId);
        if (it != instanceToBusid.end()) d.busid = it->second;
    }
    // Fallback: match by VID:PID when exactly one candidate exists.
    {
        std::map<std::string, std::string> byVidPid;
        std::map<std::string, int> count;
        for (const auto& d : devs) {
            if (!d.busid.empty()) {
                byVidPid[d.VidPid()] = d.busid;
                count[d.VidPid()] += 1;
            }
        }
        for (auto& d : devs) {
            if (d.busid.empty() && count[d.VidPid()] == 1) d.busid = byVidPid[d.VidPid()];
        }
    }
    for (auto& d : devs) {
        for (const auto& row : rows) {
            if (!d.busid.empty() && row.busid == d.busid) {
                d.backendState = row.state;
                std::string s = Lower(row.state);
                d.shared = s.find("shared") != std::string::npos;
                d.attached = s.find("attached") != std::string::npos;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    // Exported devices that no longer enumerate as a USB interface keep an entry,
    // otherwise a shared device would silently vanish from the list.
    for (const auto& kv : instanceToBusid) {
        bool known = false;
        for (const auto& d : devs)
            if (d.instanceId == kv.first) known = true;
        if (known) continue;
        UsbDevice synth;
        synth.instanceId = kv.first;
        synth.busid = kv.second;
        synth.name = "Exported device " + kv.second;
        synth.shared = true;
        synth.present = true;
        devs.push_back(synth);
    }
    for (auto& d : devs) {
        if (!d.busid.empty() && sharedBusids_.count(d.busid)) d.shared = true;
        if (!d.busid.empty() && allowedBusids_.count(d.busid)) d.attached = true;
    }
    devices_ = std::move(devs);
}

bool ServerMode::EnsureBound(const std::string& busid, std::string& message) {
    UsbipdInfo info = UsbipdCli::Instance().Probe();
    if (!info.installed) {
        message = info.detail;
        return false;
    }
    bool needsElevation = false;
    if (UsbipdCli::Instance().Bind(busid, message, &needsElevation)) {
        std::lock_guard<std::mutex> lock(mu_);
        sharedBusids_.insert(busid);
        // An explicit "Share" is also a grant for the client that is connected
        // right now, otherwise it could never attach a device shared after approval.
        if (tokenValid_ && clientConnected_) allowedBusids_.insert(busid);
        return true;
    }
    if (needsElevation) {
        ElevatedResult er;
        if (ElevatedBind(busid, er)) {
            std::lock_guard<std::mutex> lock(mu_);
            sharedBusids_.insert(busid);
            if (tokenValid_ && clientConnected_) allowedBusids_.insert(busid);
            message = "Shared " + busid;
            return true;
        }
        message = er.message.empty() ? er.error : er.message;
        return false;
    }
    return false;
}

bool ServerMode::Share(const std::string& busid, std::string& message) {
    if (!IsValidBusId(busid)) {
        message = "This device has no USB/IP bus id yet. Refresh the list and try again.";
        return false;
    }
    UsbDevice target{};
    {
        std::lock_guard<std::mutex> lock(mu_);
        bool found = false;
        for (const auto& d : devices_) {
            if (d.busid == busid) {
                target = d;
                found = true;
                break;
            }
        }
        if (!found) {
            message = "Device " + busid + " is no longer connected";
            return false;
        }
    }
    if (target.isHub) {
        message = "USB hubs cannot be shared - select an actual device.";
        return false;
    }
    if (target.isInput) {
        message = "This is a keyboard/mouse class device: sharing it removes local input control. "
                  "The USB/IP backend normally refuses this for safety.";
        LogW("server", "Attempt to share HID input device " + busid);
    }
    if (!EnsureBound(busid, message)) return false;
    RefreshDevices();
    PublishDevices();
    return true;
}

bool ServerMode::StopSharing(const std::string& busid, std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (allowedBusids_.count(busid)) {
            message = "Device " + busid + " is attached to a client. Detach it first.";
            return false;
        }
    }
    bool ok = UsbipdCli::Instance().Unbind(busid, message);
    if (!ok) {
        ElevatedResult er;
        ok = ElevatedUnbind(busid, er);
        message = ok ? ("Stopped sharing " + busid) : (er.message.empty() ? er.error : er.message);
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(mu_);
        sharedBusids_.erase(busid);
        allowedBusids_.erase(busid);
    }
    RefreshDevices();
    PublishDevices();
    return ok;
}

bool ServerMode::ShareAll(std::string& message) {
    std::vector<std::string> targets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& d : devices_) {
            if (!d.isHub && !d.busid.empty() && !d.shared) targets.push_back(d.busid);
        }
    }
    if (targets.empty()) {
        message = "No shareable devices found.";
        return false;
    }
    int okCount = 0;
    std::string firstError;
    for (const auto& busid : targets) {
        std::string msg;
        if (EnsureBound(busid, msg))
            ++okCount;
        else if (firstError.empty())
            firstError = busid + ": " + msg;
    }
    RefreshDevices();
    PublishDevices();
    message = std::to_string(okCount) + " of " + std::to_string(targets.size()) + " device(s) shared";
    if (!firstError.empty()) message += "  |  " + firstError;
    return okCount > 0;
}

bool ServerMode::StopSharingAll(std::string& message) {
    bool ok = UsbipdCli::Instance().UnbindAll(message);
    if (!ok) {
        ElevatedResult er;
        ok = ElevatedUnbind("", er);
        if (!er.message.empty()) message = er.message;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        sharedBusids_.clear();
        allowedBusids_.clear();
    }
    RefreshDevices();
    PublishDevices();
    return ok;
}

std::string ServerMode::ClientInfoText() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!tokenValid_) return {};
    if (!clientConnected_) return "Session approved - client offline (can resume)";
    return clientName_ + "  |  " + clientPeer_ + "  |  " + clientPath_;
}

// ---------------------------------------------------------------------------
// approval
// ---------------------------------------------------------------------------

void ServerMode::RespondToApproval(bool allow) {
    std::shared_ptr<ApprovalRequest> req;
    {
        std::lock_guard<std::mutex> lock(mu_);
        req = pendingApproval_;
    }
    if (req) req->Decide(allow);
}

bool ServerMode::RunApproval(const std::string& clientName, const std::string& peer, const std::string& path,
                             std::string* reason) {
    auto req = std::make_shared<ApprovalRequest>();
    req->clientName = clientName;
    req->peer = peer;
    req->path = path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        req->requested = std::to_string(sharedBusids_.size()) + " shared device(s)";
        pendingApproval_ = req;
    }
    SetState(State::ApprovalPending, "A client is requesting access");
    auto ev = MakeUiEvent(UiKind::Approval);
    ev->approval = req;
    PostUi(std::move(ev));

    bool answered = req->Wait(kApprovalTimeoutMs);
    {
        std::lock_guard<std::mutex> lock(mu_);
        pendingApproval_.reset();
    }
    if (!answered) {
        if (reason) *reason = "The approval request timed out";
        return false;
    }
    if (req->decision != 1) {
        if (reason) *reason = "The server user rejected the request";
        return false;
    }
    return true;
}

void ServerMode::RevokeClientLocked(const std::string& reason) {
    tokenValid_ = false;
    std::memset(token_, 0, sizeof(token_));
    allowedBusids_.clear();
    clientName_.clear();
    clientPeer_.clear();
    clientPath_.clear();
    sessionValid_ = false;
    sessionId_ = 0;
    std::memset(sessionSecret_, 0, sizeof(sessionSecret_));
    clientConnected_ = false;
    (void)reason;
}

// The control channel dropped: keep the approval alive so the client can resume,
// but stop calling it "connected".
void ServerMode::MarkClientDisconnectedLocked() {
    clientConnected_ = false;
    clientPath_.clear();
    clientPeer_.clear();
    clientName_.clear();
}

void ServerMode::DisconnectClient(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        RevokeClientLocked(reason);
    }
    LogI("server", "Client disconnected: " + reason);
    auto ev = MakeUiEvent(UiKind::ServerClient);
    ev->flag = false;
    PostUi(std::move(ev));
    SetState(State::WaitingClient, "Waiting for Client");
    RegenerateCode();
}

// ---------------------------------------------------------------------------
// loops
// ---------------------------------------------------------------------------

void ServerMode::AcceptLoop() {
    while (!stop_.load()) {
        std::string peer;
        SOCKET sock = TcpAccept(listener_, &peer, 500);
        if (sock == INVALID_SOCKET) continue;
        std::thread([this, sock]() { HandleConnection(sock, "Direct"); }).detach();
    }
}

void ServerMode::RelayLoop() {
    Settings& s = GetSettings();
    while (!stop_.load()) {
        if (!s.relayEnabled || s.relayHost.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        PairingCode snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot = code_;
        }
        std::string err;
        SOCKET sock = ConnectRelay(s.relayHost, s.relayPort, 2, snapshot.sessionId, RelayAuthTag(snapshot), 90000,
                                   &err);
        if (sock == INVALID_SOCKET) {
            if (!stop_.load()) {
                LogW("server", "Relay registration failed: " + err);
                auto ev = MakeUiEvent(UiKind::Banner, "Relay unavailable: " + err, "warn");
                PostUi(std::move(ev));
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            continue;
        }
        HandleConnection(sock, "Relay");
    }
}

void ServerMode::WatchdogLoop() {
    int tick = 0;
    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stop_.load()) break;
        if (++tick % 4 != 0) continue;  // ~2 s device refresh

        RefreshDevices();
        PublishDevices();

        bool expired = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            expired = !codeUsed_ && codeExpiry_ != 0 && NowUnix() > codeExpiry_;
        }
        if (expired && CurrentState() != State::ClientConnected) {
            LogI("server", "Pairing code expired - generating a new one");
            RegenerateCode();
        }

        bool sessionExpired = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (tokenValid_ && !clientConnected_ && tokenExpiry_ != 0 &&
                NowUnix() > static_cast<uint64_t>(tokenExpiry_) + 3600) {
                RevokeClientLocked("approved session expired");
                sessionExpired = true;
            }
        }
        if (sessionExpired) {
            LogI("server", "Approved session expired");
            auto ev = MakeUiEvent(UiKind::ServerClient);
            ev->flag = false;
            PostUi(std::move(ev));
        }
    }
}

// ---------------------------------------------------------------------------
// data channel: authenticated tunnel to the local usbipd endpoint
// ---------------------------------------------------------------------------

void ServerMode::HandleDataChannel(ControlChannel& channel, const DataAuthBody& auth, const std::string& peer,
                                  const std::string& path) {
    std::string err;
    bool tokenOk = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        tokenOk = tokenValid_ && NowUnix() < tokenExpiry_ && ConstantTimeEqual(auth.token, token_, kTokenLen);
    }
    if (!tokenOk) {
        ByteWriter w;
        StatusBody{Status::BadToken}.Write(w);
        channel.Send(MsgType::DataOk, w);
        LogW("server", "Data channel rejected (bad token) from " + peer);
        channel.Close();
        return;
    }

    uint8_t header[kUsbipImportLen];
    if (!channel.ReadExact(header, sizeof(header), 15000)) {
        LogW("server", "Data channel closed before the USB/IP import request");
        channel.Close();
        return;
    }
    UsbipImport imp;
    std::string perr;
    if (!ParseUsbipImport(header, sizeof(header), imp, &perr)) {
        ByteWriter w;
        StatusBody{Status::NotAllowed}.Write(w);
        channel.Send(MsgType::DataOk, w);
        LogW("server", "Rejected malformed USB/IP import: " + perr);
        channel.Close();
        return;
    }
    {
        bool allowed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            allowed = allowedBusids_.count(imp.busid) > 0;
        }
        if (!allowed) {
            ByteWriter w;
            StatusBody{Status::NotAllowed}.Write(w);
            channel.Send(MsgType::DataOk, w);
            LogW("server", "Client asked for unshared bus id '" + imp.busid + "' - refused");
            channel.Close();
            return;
        }
    }

    SOCKET local = TcpConnect("127.0.0.1", kUsbipdPort, 5000, &err);
    if (local == INVALID_SOCKET) {
        ByteWriter w;
        StatusBody{Status::BackendError}.Write(w);
        channel.Send(MsgType::DataOk, w);
        LogE("server", "Local usbipd endpoint unreachable: " + err);
        channel.Close();
        return;
    }
    ByteWriter ok;
    StatusBody{Status::Ok}.Write(ok);
    if (!channel.Send(MsgType::DataOk, ok) || !SendAll(local, header, sizeof(header))) {
        CloseSocket(local);
        channel.Close();
        return;
    }
    LogI("server", "USB/IP tunnel established for bus id " + imp.busid + " (" + path + ")");
    std::atomic<bool> stopPump{false};
    channel.ReleaseRawSocketForPump();
    PipeTlsToSocket(channel, local, stopPump);
    CloseSocket(local);
    LogI("server", "USB/IP tunnel closed for bus id " + imp.busid);
}

// ---------------------------------------------------------------------------
// control channel
// ---------------------------------------------------------------------------

void ServerMode::RunControlLoop(ControlChannel& ch, const std::string& clientName) {
    int idle = 0;
    while (!stop_.load()) {
        {
            // The server user may revoke access at any time; notice it quickly.
            std::lock_guard<std::mutex> lock(mu_);
            if (!tokenValid_) {
                ByteWriter bw;
                ByeBody{"access revoked by the server user"}.Write(bw);
                ch.Send(MsgType::Bye, bw);
                break;
            }
        }
        Message m;
        if (!ch.Receive(m, 3000)) {
            if (ch.TimedOut()) {
                if (++idle % 4 == 0) {  // keepalive roughly every 12 s
                    ByteWriter pw;
                    NonceBody{static_cast<uint32_t>(NowUnix())}.Write(pw);
                    if (!ch.Send(MsgType::Ping, pw)) break;
                }
                continue;
            }
            break;
        }
        idle = 0;

        if (m.type == MsgType::Bye) break;
        if (m.type == MsgType::Ping) {
            NonceBody nb;
            ByteReader r(m.payload);
            NonceBody::Read(r, nb);
            ByteWriter w;
            nb.Write(w);
            ch.Send(MsgType::Pong, w);
            continue;
        }
        if (m.type == MsgType::DeviceListRequest) {
            ByteWriter w;
            DeviceListBody body;
            body.devices = BuildDeviceEntries();
            body.Write(w);
            ch.Send(MsgType::DeviceList, w);
            continue;
        }
        if (m.type == MsgType::RequestAttach) {
            BusidBody req;
            ByteReader r(m.payload);
            BusidBody::Read(r, req);
            ResultBody reply;
            reply.busid = req.busid;
            bool allowedNow = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                allowedNow = allowedBusids_.count(req.busid) > 0;
            }
            if (!IsValidBusId(req.busid)) {
                reply.status = Status::NotShared;
                reply.message = "Invalid bus id";
            } else if (!allowedNow) {
                reply.status = Status::NotAllowed;
                reply.message = "The server has not shared this device with you";
            } else {
                std::string msg;
                if (EnsureBound(req.busid, msg)) {
                    reply.status = Status::Ok;
                    reply.message = "Device " + req.busid + " is exported";
                    LogI("server", "Attach granted for " + req.busid + " to " + clientName);
                } else {
                    reply.status = Status::BackendError;
                    reply.message = msg;
                    LogE("server", "Attach failed for " + req.busid + ": " + msg);
                }
            }
            ByteWriter w;
            reply.Write(w);
            ch.Send(MsgType::AttachResult, w);
            RefreshDevices();
            PublishDevices();
            continue;
        }
        if (m.type == MsgType::RequestDetach) {
            BusidBody req;
            ByteReader r(m.payload);
            BusidBody::Read(r, req);
            {
                std::lock_guard<std::mutex> lock(mu_);
                allowedBusids_.erase(req.busid);
            }
            ResultBody reply;
            reply.status = Status::Ok;
            reply.busid = req.busid;
            reply.message = "Detached " + req.busid;
            ByteWriter w;
            reply.Write(w);
            ch.Send(MsgType::DetachResult, w);
            RefreshDevices();
            PublishDevices();
            continue;
        }
    }
}

// Resume an already approved session after a network drop (token based, no new code).
bool ServerMode::TryResumeSession(ControlChannel& channel, const HelloBody& hello, const std::string& peer,
                                 const std::string& path) {
    uint64_t sessionId = 0;
    uint8_t secret[kSecretLen] = {};
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!tokenValid_ || !sessionValid_) return false;
        if (tokenExpiry_ != 0 && NowUnix() > static_cast<uint64_t>(tokenExpiry_) + 3600) return false;
        if (hello.sessionId == 0 || hello.sessionId != sessionId_) return false;
        sessionId = sessionId_;
        std::memcpy(secret, sessionSecret_, kSecretLen);
    }

    ChallengeBody challenge;
    if (!RandomBytes(challenge.nonceS, kNonceLen)) return false;
    challenge.serverName = LocalPCName();
    ByteWriter cw;
    challenge.Write(cw);
    if (!channel.Send(MsgType::Challenge, cw)) return true;  // handled: channel is broken

    Message msg;
    bool resumed = false;
    uint8_t fresh[kTokenLen] = {};
    uint32_t newExpiry = 0;
    if (channel.Receive(msg, kChallengeTimeoutMs) && msg.type == MsgType::ResumeAuth) {
        ResumeBody resume;
        ByteReader r(msg.payload);
        if (ResumeBody::Read(r, resume)) {
            std::lock_guard<std::mutex> lock(mu_);
            if (tokenValid_ && ConstantTimeEqual(resume.token, token_, kTokenLen) &&
                RandomBytes(token_, sizeof(token_))) {
                std::memcpy(fresh, token_, kTokenLen);
                tokenExpiry_ = static_cast<uint32_t>(NowUnix() + kTokenTtlSeconds);
                newExpiry = tokenExpiry_;
                clientConnected_ = true;
                clientPeer_ = peer;
                clientPath_ = path;
                resumed = true;
            }
        }
    }
    if (!resumed) {
        LogW("server", "Session resume rejected for " + peer);
        AuthResultBody body;
        body.status = Status::AlreadyUsed;
        body.detail = "The session could not be resumed - ask for a new pairing code";
        ByteWriter w;
        body.Write(w);
        channel.Send(MsgType::AuthResult, w);
        channel.Close();
        return true;
    }

    AuthResultBody result;
    result.status = Status::Ok;
    result.expiresUnix = newExpiry;
    result.detail = LocalPCName();
    Sha256Digest proof = ComputeServerProof(secret, sessionId, hello.nonceC, challenge.nonceS);
    std::memcpy(result.mac, proof.data(), kMacLen);
    std::memcpy(result.token, fresh, kTokenLen);
    ByteWriter rw;
    result.Write(rw);
    if (!channel.Send(MsgType::AuthResult, rw)) {
        channel.Close();
        return true;
    }

    LogI("server", "Approved client resumed from " + peer + " (" + path + ")");
    {
        auto ev = MakeUiEvent(UiKind::ServerClient, "Client (resumed)", peer);
        ev->c = path;
        ev->flag = true;
        PostUi(std::move(ev));
    }
    SetState(State::ClientConnected, "Client reconnected");
    RefreshDevices();
    PublishDevices();
    RunControlLoop(channel, "Client (resumed)");
    {
        std::lock_guard<std::mutex> lock(mu_);
        MarkClientDisconnectedLocked();
    }
    channel.Close();
    {
        auto ev = MakeUiEvent(UiKind::ServerClient);
        ev->flag = false;
        PostUi(std::move(ev));
    }
    SetState(State::WaitingClient, "Waiting for Client");
    return true;
}

void ServerMode::HandleConnection(SOCKET sock, const std::string& path) {
    std::string peer = PeerAddress(sock);
    std::string err;
    if (!identity_) {
        CloseSocket(sock);
        return;
    }
    ControlChannel channel;
    if (!channel.StartServer(sock, *identity_, &err)) {
        LogW("server", "TLS handshake failed (" + path + "): " + err);
        return;
    }

    Message first;
    if (!channel.Receive(first, kPairingTimeoutMs)) {
        LogW("server", "No handshake message from " + peer + " (" + path + ")");
        channel.Close();
        return;
    }

    if (first.type == MsgType::DataAuth) {
        DataAuthBody auth;
        ByteReader r(first.payload);
        if (!DataAuthBody::Read(r, auth)) {
            channel.Close();
            return;
        }
        HandleDataChannel(channel, auth, peer, path);
        return;
    }
    if (first.type != MsgType::Hello) {
        LogW("server", "Unexpected first message from " + peer);
        channel.Close();
        return;
    }

    HelloBody hello;
    {
        ByteReader r(first.payload);
        if (!HelloBody::Read(r, hello)) {
            channel.Close();
            return;
        }
    }

    // Already approved client coming back after a network interruption?
    if (TryResumeSession(channel, hello, peer, path)) return;

    PairingCode active;
    bool used = false;
    uint32_t expiry = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        active = code_;
        used = codeUsed_;
        expiry = codeExpiry_;
    }

    auto reject = [&](Status st, const std::string& detail, bool countFailure) {
        AuthResultBody body;
        body.status = st;
        body.detail = detail;
        ByteWriter w;
        body.Write(w);
        channel.Send(MsgType::AuthResult, w);
        if (countFailure) {
            std::lock_guard<std::mutex> lock(mu_);
            if (++authFailures_ >= kMaxAuthFailures) {
                LogW("server", "Too many failed pairing attempts - the code is locked");
            }
        }
        LogW("server", "Pairing rejected (" + std::string(StatusText(st)) + ") from " + peer + ": " + detail);
    };

    // Cheap rejections first: an unknown client never gets a challenge.
    {
        int failures = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            failures = authFailures_;
        }
        if (hello.version != kProtocolVersion) {
            reject(Status::BadVersion, "Client protocol version " + std::to_string(hello.version), false);
            channel.Close();
            return;
        }
        if (failures >= kMaxAuthFailures) {
            reject(Status::RateLimited, "Too many failed attempts - regenerate the pairing code", false);
            channel.Close();
            return;
        }
        if (hello.sessionId != active.sessionId) {
            reject(Status::UnknownSession, "Unknown pairing code", true);
            channel.Close();
            return;
        }
        if (used) {
            reject(Status::AlreadyUsed, "This pairing code was already used", false);
            channel.Close();
            return;
        }
        if (expiry != 0 && NowUnix() > expiry) {
            reject(Status::Expired, "The pairing code expired", false);
            channel.Close();
            return;
        }
    }

    ChallengeBody challenge;
    if (!RandomBytes(challenge.nonceS, kNonceLen)) {
        channel.Close();
        return;
    }
    challenge.serverName = LocalPCName();
    ByteWriter cw;
    challenge.Write(cw);
    if (!channel.Send(MsgType::Challenge, cw)) {
        channel.Close();
        return;
    }

    Message authMsg;
    if (!channel.Receive(authMsg, kPairingTimeoutMs) || authMsg.type != MsgType::Auth) {
        channel.Close();
        return;
    }
    AuthBody auth;
    {
        ByteReader r(authMsg.payload);
        if (!AuthBody::Read(r, auth)) {
            channel.Close();
            return;
        }
    }

    // The full decision (including the constant-time HMAC check) lives in core so
    // every rejection path is unit tested.
    AuthAttempt attempt;
    attempt.version = hello.version;
    attempt.sessionId = hello.sessionId;
    std::memcpy(attempt.nonceC, hello.nonceC, kNonceLen);
    std::memcpy(attempt.nonceS, challenge.nonceS, kNonceLen);
    std::memcpy(attempt.mac, auth.mac, kMacLen);
    {
        int failures = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            failures = authFailures_;
        }
        AuthDecision decision =
            CheckPairingAttempt(active, used, expiry, failures, kMaxAuthFailures, attempt, NowUnix());
        if (!decision.ok()) {
            reject(decision.status, decision.detail, decision.countFailure);
            channel.Close();
            return;
        }
    }

    // The human decides whether this client may use the shared devices.
    std::string clientName = auth.clientName.empty() ? "Unknown client" : auth.clientName;
    std::string reason;
    if (!RunApproval(clientName, peer, path, &reason)) {
        AuthResultBody body;
        body.status = Status::Rejected;
        body.detail = reason;
        ByteWriter w;
        body.Write(w);
        channel.Send(MsgType::AuthResult, w);
        LogW("server", "Access rejected for " + clientName + " (" + peer + "): " + reason);
        SetState(State::WaitingClient, "Waiting for Client");
        channel.Close();
        return;
    }

    uint8_t token[kTokenLen];
    if (!RandomBytes(token, sizeof(token))) {
        channel.Close();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::memcpy(token_, token, sizeof(token_));
        tokenValid_ = true;
        tokenExpiry_ = static_cast<uint32_t>(NowUnix() + kTokenTtlSeconds);
        allowedBusids_ = sharedBusids_;
        sessionValid_ = true;
        sessionId_ = active.sessionId;
        std::memcpy(sessionSecret_, active.secret, kSecretLen);
        clientConnected_ = true;
        clientName_ = clientName;
        clientPeer_ = peer;
        clientPath_ = path;
        if (GetSettings().singleUseCode) codeUsed_ = true;
    }

    AuthResultBody result;
    result.status = Status::Ok;
    result.expiresUnix = static_cast<uint32_t>(NowUnix() + kTokenTtlSeconds);
    result.detail = LocalPCName();
    Sha256Digest proof = ComputeServerProof(active.secret, active.sessionId, hello.nonceC, challenge.nonceS);
    std::memcpy(result.mac, proof.data(), kMacLen);
    std::memcpy(result.token, token, kTokenLen);
    ByteWriter rw;
    result.Write(rw);
    if (!channel.Send(MsgType::AuthResult, rw)) {
        channel.Close();
        return;
    }

    LogI("server", "Client approved: " + clientName + " from " + peer + " via " + path);
    {
        auto ev = MakeUiEvent(UiKind::ServerClient, clientName, peer);
        ev->c = path;
        ev->flag = true;
        PostUi(std::move(ev));
    }
    SetState(State::ClientConnected, "Client connected");
    RefreshDevices();
    PublishDevices();

    RunControlLoop(channel, clientName);

    {
        std::lock_guard<std::mutex> lock(mu_);
        MarkClientDisconnectedLocked();
    }
    channel.Close();
    {
        auto ev = MakeUiEvent(UiKind::ServerClient);
        ev->flag = false;
        PostUi(std::move(ev));
    }
    SetState(State::WaitingClient, "Waiting for Client");
    LogI("server", "Client " + clientName + " disconnected");
    RegenerateCode();
}

}  // namespace mub
