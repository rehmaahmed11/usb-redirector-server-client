// Mini USB Bridge - unit tests for the portable core (crypto, pairing, protocol)
// Build & run on any host:  g++ -std=c++17 -O2 -Isrc tests/test_core.cpp src/core/*.cpp -o test_core
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/base32.h"
#include "../src/core/bytes.h"
#include "../src/core/pairing.h"
#include "../src/core/protocol.h"
#include "../src/core/sha256.h"
#include "../src/core/strings.h"
#include "../src/core/session_auth.h"
#include "../src/core/usbip_wire.h"

using namespace mub;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                            \
    do {                                                                                              \
        ++g_checks;                                                                                   \
        std::string _a = (a), _b = (b);                                                               \
        if (_a != _b) {                                                                               \
            std::printf("  FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, _a.c_str(), _b.c_str());   \
            ++g_failures;                                                                             \
        }                                                                                             \
    } while (0)

static void TestSha256() {
    std::printf("SHA-256 vectors\n");
    CHECK_EQ_STR(Hex(Sha256Once("").data(), 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ_STR(Hex(Sha256Once("abc").data(), 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ_STR(Hex(Sha256Once("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").data(), 32),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a' (streaming path + block boundaries).
    Sha256 s;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.Update(chunk);
    CHECK_EQ_STR(Hex(s.Finish().data(), 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void TestHmac() {
    std::printf("HMAC-SHA256 (RFC 4231)\n");
    {  // case 1
        Sha256Digest d = HmacSha256(std::string(20, '\x0b'), "Hi There");
        CHECK_EQ_STR(Hex(d.data(), 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    {  // case 2: "Jefe" / "what do ya want for nothing?"
        Sha256Digest d = HmacSha256("Jefe", "what do ya want for nothing?");
        CHECK_EQ_STR(Hex(d.data(), 32), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    {  // case 3
        std::string key(20, '\xaa');
        std::string data(50, '\xdd');
        Sha256Digest d = HmacSha256(key, data);
        CHECK_EQ_STR(Hex(d.data(), 32), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    }
    {  // case 6: key longer than block size
        Sha256Digest d = HmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First");
        CHECK_EQ_STR(Hex(d.data(), 32), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
    // Constant-time compare behaves like an equality test.
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};
    CHECK(ConstantTimeEqual(a, b, 4));
    CHECK(!ConstantTimeEqual(a, c, 4));
}

static void TestBase32() {
    std::printf("Crockford base32\n");
    const uint8_t data[5] = {'H', 'e', 'l', 'l', 'o'};
    std::string enc = Base32Encode(data, 5);
    CHECK_EQ_STR(enc, "91JPRV3F");  // Crockford reference for "Hello"
    std::vector<uint8_t> dec;
    CHECK(Base32Decode(enc, dec));
    CHECK(dec.size() == 5 && std::memcmp(dec.data(), data, 5) == 0);
    // Ambiguous characters map the same way.
    std::vector<uint8_t> dec2;
    CHECK(Base32Decode("91jprv3f", dec2));
    CHECK(dec2.size() == 5 && std::memcmp(dec2.data(), data, 5) == 0);
    std::vector<uint8_t> dec3;
    CHECK(Base32Decode("OIL", dec3));  // valid symbols after mapping (0,1,1)
    CHECK(!Base32Decode("AB$", dec3));
    // Random round trips.
    for (int len = 1; len <= 64; ++len) {
        std::vector<uint8_t> v(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) v[static_cast<size_t>(i)] = static_cast<uint8_t>((i * 37 + len * 11) & 0xFF);
        std::vector<uint8_t> back;
        CHECK(Base32Decode(Base32Encode(v), back));
        CHECK(back == v);
    }
}

static void TestPairing() {
    std::printf("Pairing code round trip\n");
    PairingCode code;
    code.flags = kPairFlagDirect | kPairFlagRelay;
    code.directPort = 4437;
    code.expiresUnix = 1767225600u;
    code.sessionId = 0x0123456789ABCDEFull;
    for (size_t i = 0; i < kSecretLen; ++i) code.secret[i] = static_cast<uint8_t>(i * 7 + 3);
    for (size_t i = 0; i < kCertPinLen; ++i) code.certPin[i] = static_cast<uint8_t>(0xA0 + i);
    code.host = "203.0.113.9";

    std::string text = EncodePairingCode(code);
    CHECK(LooksLikePairingCode(text));
    CHECK(text.rfind("MUB1-", 0) == 0);

    PairingCode back;
    std::string err;
    CHECK(DecodePairingCode(text, back, &err));
    CHECK(back.version == code.version);
    CHECK(back.flags == code.flags);
    CHECK(back.directPort == code.directPort);
    CHECK(back.expiresUnix == code.expiresUnix);
    CHECK(back.sessionId == code.sessionId);
    CHECK(back.host == code.host);
    CHECK(std::memcmp(back.secret, code.secret, kSecretLen) == 0);
    CHECK(std::memcmp(back.certPin, code.certPin, kCertPinLen) == 0);
    CHECK(back.HasDirect());
    CHECK(back.HasRelay());
    CHECK_EQ_STR(back.Endpoint(), "203.0.113.9:4437");

    // Tolerant parsing: lowercase, spaces instead of dashes, extra outer whitespace.
    std::string messy = "  " + Lower(text) + "\r\n";
    for (char& c : messy)
        if (c == '-') c = ' ';
    PairingCode back2;
    CHECK(DecodePairingCode(messy, back2, &err));
    CHECK(back2.sessionId == code.sessionId);

    // Relay-only code has no host.
    PairingCode relayOnly = code;
    relayOnly.flags = kPairFlagRelay;
    relayOnly.directPort = 0;
    relayOnly.host.clear();
    PairingCode ro;
    CHECK(DecodePairingCode(EncodePairingCode(relayOnly), ro, &err));
    CHECK(!ro.HasDirect());
    CHECK(ro.HasRelay());
    CHECK_EQ_STR(ro.Endpoint(), "-");

    // Corrupt / invalid input is rejected.
    PairingCode bad;
    CHECK(!DecodePairingCode("", bad, &err));
    CHECK(!DecodePairingCode("MUB1-", bad, &err));
    CHECK(!DecodePairingCode("XYZ1-ABCDE-ABCDE", bad, &err));
    CHECK(!DecodePairingCode("MUB1-$$$$$$-$$$$$", bad, &err));
    std::string truncated = text.substr(0, text.size() - 12);
    CHECK(!DecodePairingCode(truncated, bad, &err));
    // Unknown version is rejected, not silently accepted.
    std::vector<uint8_t> payload;
    PairingCode v2 = code;
    std::string enc = EncodePairingCode(v2);
    CHECK(DecodePairingCode(enc, bad, &err));
    // Different secret => different relay tag, deterministic for the same code.
    PairingCode other = code;
    other.secret[0] ^= 0xFF;
    CHECK(RelayAuthTag(code) != RelayAuthTag(other));
    CHECK_EQ_STR(RelayAuthTag(code), RelayAuthTag(back));
}

static void TestProofs() {
    std::printf("Auth proofs\n");
    PairingCode code;
    for (size_t i = 0; i < kSecretLen; ++i) code.secret[i] = static_cast<uint8_t>(i + 1);
    uint8_t nonceC[kNonceLen], nonceS[kNonceLen], pin[kCertPinLen];
    for (size_t i = 0; i < kNonceLen; ++i) {
        nonceC[i] = static_cast<uint8_t>(i);
        nonceS[i] = static_cast<uint8_t>(0x80 + i);
    }
    for (size_t i = 0; i < kCertPinLen; ++i) pin[i] = static_cast<uint8_t>(0x11 * i);

    Sha256Digest p1 = ComputeClientProof(code.secret, code.sessionId, nonceC, nonceS, pin, kCertPinLen);
    Sha256Digest p2 = ComputeClientProof(code.secret, code.sessionId, nonceC, nonceS, pin, kCertPinLen);
    CHECK(ConstantTimeEqual(p1.data(), p2.data(), kMacLen));
    // Any input change breaks the proof.
    nonceS[0] ^= 1;
    Sha256Digest p3 = ComputeClientProof(code.secret, code.sessionId, nonceC, nonceS, pin, kCertPinLen);
    CHECK(!ConstantTimeEqual(p1.data(), p3.data(), kMacLen));
    Sha256Digest s1 = ComputeServerProof(code.secret, code.sessionId, nonceC, nonceS);
    Sha256Digest c1 = ComputeClientProof(code.secret, code.sessionId, nonceC, nonceS, pin, kCertPinLen);
    CHECK(!ConstantTimeEqual(s1.data(), c1.data(), kMacLen));  // distinct domains
}

static void TestFrames() {
    std::printf("Frame codec / parser\n");
    Message m;
    m.type = MsgType::Hello;
    HelloBody hello;
    hello.sessionId = 0xDEADBEEFCAFEBABEull;
    for (size_t i = 0; i < kNonceLen; ++i) hello.nonceC[i] = static_cast<uint8_t>(i);
    ByteWriter w;
    hello.Write(w);
    m.payload = w.buf;
    std::vector<uint8_t> wire;
    m.Encode(wire);
    CHECK(wire.size() == 3 + m.payload.size());
    CHECK(wire[0] == static_cast<uint8_t>(MsgType::Hello));
    CHECK(((wire[1] << 8) | wire[2]) == static_cast<int>(m.payload.size()));

    // Split the wire into odd chunks to prove streaming reassembly.
    FrameParser parser;
    std::vector<Message> got;
    size_t off = 0;
    const size_t chunks[] = {1, 2, 5, 3, 40};
    size_t ci = 0;
    while (off < wire.size()) {
        size_t n = chunks[ci++ % 5];
        if (off + n > wire.size()) n = wire.size() - off;
        CHECK(parser.Feed(wire.data() + off, n, got));
        off += n;
    }
    CHECK(got.size() == 1);
    CHECK(got.size() == 1 && got[0].type == MsgType::Hello);
    HelloBody back;
    ByteReader r(got[0].payload);
    CHECK(HelloBody::Read(r, back));
    CHECK(back.sessionId == hello.sessionId);
    CHECK(std::memcmp(back.nonceC, hello.nonceC, kNonceLen) == 0);

    // Two frames in one feed.
    std::vector<Message> two;
    std::vector<uint8_t> wire2 = wire;
    wire2.insert(wire2.end(), wire.begin(), wire.end());
    FrameParser p2;
    CHECK(p2.Feed(wire2.data(), wire2.size(), two));
    CHECK(two.size() == 2);

    // Device list round trip including UTF-8 text and bounds.
    DeviceListBody list;
    for (int i = 0; i < 3; ++i) {
        DeviceEntry d;
        d.busid = "1-" + std::to_string(i + 1);
        d.instanceId = "USB\\VID_0E8D&PID_2000\\5&2b1c";
        d.name = i == 0 ? "MediaTek Preloader USB VCOM" : "USB Mass Storage Device";
        d.manufacturer = "MediaTek Inc. \xE2\x84\xA2";  // multi-byte UTF-8 on purpose
        d.vid = 0x0E8D;
        d.pid = static_cast<uint16_t>(0x2000 + i);
        d.shared = static_cast<uint8_t>(i == 1);
        d.attached = static_cast<uint8_t>(i == 2);
        list.devices.push_back(d);
    }
    ByteWriter lw;
    list.Write(lw);
    DeviceListBody lback;
    ByteReader lr(lw.buf);
    CHECK(DeviceListBody::Read(lr, lback));
    CHECK(lback.devices.size() == 3);
    CHECK(lback.devices.size() == 3 && lback.devices[0].busid == "1-1");
    CHECK(lback.devices.size() == 3 && lback.devices[1].vid == 0x0E8D && lback.devices[1].pid == 0x2001);
    CHECK(lback.devices.size() == 3 && lback.devices[1].shared == 1);
    CHECK(lback.devices.size() == 3 && lback.devices[2].manufacturer == "MediaTek Inc. \xE2\x84\xA2");

    // Truncated payload must not be accepted.
    ByteReader trunc(lw.buf.data(), lw.buf.size() - 5);
    DeviceListBody bad;
    CHECK(!DeviceListBody::Read(trunc, bad));

    // Result bodies carry backend error text.
    ResultBody res;
    res.status = Status::BackendError;
    res.busid = "1-4";
    res.message = "usbipd: error: device is not shared";
    ByteWriter rw;
    res.Write(rw);
    ResultBody rback;
    ByteReader rr(rw.buf);
    CHECK(ResultBody::Read(rr, rback));
    CHECK(rback.status == Status::BackendError);
    CHECK_EQ_STR(rback.busid, "1-4");
    CHECK_EQ_STR(rback.message, res.message);

    // Resume body (reconnect after a network drop)
    ResumeBody rb;
    for (size_t i = 0; i < kTokenLen; ++i) rb.token[i] = static_cast<uint8_t>(i * 3);
    for (size_t i = 0; i < kNonceLen; ++i) rb.nonceC[i] = static_cast<uint8_t>(0x40 + i);
    ByteWriter rwb;
    rb.Write(rwb);
    ResumeBody rb2;
    ByteReader rbr(rwb.buf);
    CHECK(ResumeBody::Read(rbr, rb2));
    CHECK(std::memcmp(rb2.token, rb.token, kTokenLen) == 0);
    CHECK_EQ_STR(StatusText(Status::Expired), "Pairing code expired");
}

static void TestStrings() {
    std::printf("String helpers\n");
    CHECK_EQ_STR(WideToUtf8(Utf8ToWide("MediaTek \xE2\x84\xA2 \xC3\xA9")), "MediaTek \xE2\x84\xA2 \xC3\xA9");
    CHECK_EQ_STR(Trim("  hi \r\n"), "hi");
    CHECK_EQ_STR(ToUpperAscii("aBc-d"), "ABC-D");
    CHECK(StartsWithNoCase("MUB1-abc", "mub1"));
    CHECK(IsValidBusId("1-4"));
    CHECK(IsValidBusId("3-2.1"));
    CHECK(!IsValidBusId("1"));
    CHECK(!IsValidBusId("-4"));
    CHECK(!IsValidBusId("1-"));
    CHECK(!IsValidBusId("1-2-3"));
    CHECK(!IsValidBusId("usb1-2"));
    CHECK_EQ_STR(FormatCountdown(1000, 1300), "5:00");
    CHECK_EQ_STR(FormatCountdown(1000, 0), "--:--");
    CHECK_EQ_STR(FormatCountdown(2000, 1500), "expired");
    CHECK_EQ_STR(FormatUtc(0), "1970-01-01 00:00:00Z");
    CHECK_EQ_STR(HexU16(0x0E8D), "0e8d");
}

static void TestPairingDecisions() {
    std::printf("Pairing acceptance matrix\n");
    PairingCode code;
    code.sessionId = 0xAABBCCDD11223344ull;
    for (size_t i = 0; i < kSecretLen; ++i) code.secret[i] = static_cast<uint8_t>(0x30 + i);
    for (size_t i = 0; i < kCertPinLen; ++i) code.certPin[i] = static_cast<uint8_t>(0x70 + i);
    const uint64_t now = 1000000;
    const uint32_t expiry = static_cast<uint32_t>(now + 60);
    const int maxFail = 5;

    AuthAttempt good;
    good.version = kProtocolVersion;
    good.sessionId = code.sessionId;
    for (size_t i = 0; i < kNonceLen; ++i) {
        good.nonceC[i] = static_cast<uint8_t>(i + 1);
        good.nonceS[i] = static_cast<uint8_t>(0x90 + i);
    }
    Sha256Digest proof = ComputeClientProof(code.secret, code.sessionId, good.nonceC, good.nonceS, code.certPin,
                                            kCertPinLen);
    std::memcpy(good.mac, proof.data(), kMacLen);

    CHECK(CheckPairingAttempt(code, false, expiry, 0, maxFail, good, now).ok());

    // Wrong pairing secret (a guessed code) is a bad MAC and counts as a failure.
    AuthAttempt forged = good;
    forged.mac[0] ^= 0x01;
    AuthDecision d = CheckPairingAttempt(code, false, expiry, 0, maxFail, forged, now);
    CHECK(d.status == Status::BadMac && d.countFailure);

    // Wrong session id: unknown code, counted.
    AuthAttempt wrongSession = good;
    wrongSession.sessionId ^= 1;
    d = CheckPairingAttempt(code, false, expiry, 0, maxFail, wrongSession, now);
    CHECK(d.status == Status::UnknownSession && d.countFailure);

    // Replay of a used code is refused without counting.
    d = CheckPairingAttempt(code, true, expiry, 0, maxFail, good, now);
    CHECK(d.status == Status::AlreadyUsed && !d.countFailure);

    // Expired code.
    d = CheckPairingAttempt(code, false, expiry, 0, maxFail, good, now + 61);
    CHECK(d.status == Status::Expired);

    // Rate limiting kicks in and stays in effect.
    d = CheckPairingAttempt(code, false, expiry, maxFail, maxFail, good, now);
    CHECK(d.status == Status::RateLimited);
    d = CheckPairingAttempt(code, false, expiry, maxFail - 1, maxFail, forged, now);
    CHECK(d.status == Status::BadMac);

    // Version mismatch is refused but does not burn the code.
    AuthAttempt badVersion = good;
    badVersion.version = 2;
    d = CheckPairingAttempt(code, false, expiry, 0, maxFail, badVersion, now);
    CHECK(d.status == Status::BadVersion && !d.countFailure);

    CHECK(PairingCodeExhausted(false, expiry, 0, maxFail, now) == false);
    CHECK(PairingCodeExhausted(true, expiry, 0, maxFail, now) == true);
    CHECK(PairingCodeExhausted(false, expiry, 0, maxFail, now + 61) == true);
    CHECK(PairingCodeExhausted(false, expiry, maxFail, maxFail, now) == true);

    // Nonce reuse with a different challenge must fail (fresh nonce per attempt).
    AuthAttempt replayed = good;
    replayed.nonceS[0] ^= 0xFF;
    d = CheckPairingAttempt(code, false, expiry, 0, maxFail, replayed, now);
    CHECK(d.status == Status::BadMac);

    // A different pairing code with the same nonces must not validate.
    PairingCode other = code;
    other.secret[5] ^= 0x22;
    d = CheckPairingAttempt(other, false, expiry, 0, maxFail, good, now);
    CHECK(d.status == Status::BadMac);
}

static void TestUsbipWire() {
    std::printf("USB/IP import header inspection\n");
    uint8_t buf[kUsbipImportLen];
    BuildUsbipImport("1-4", buf);
    UsbipImport imp;
    std::string err;
    CHECK(ParseUsbipImport(buf, sizeof(buf), imp, &err));
    CHECK(imp.version == kUsbipVersion111);
    CHECK(imp.command == kUsbipOpReqImport);
    CHECK_EQ_STR(imp.busid, "1-4");

    // Busid with a sub-port ("3-2.1"), as exported by usbipd-win.
    BuildUsbipImport("3-2.1", buf);
    CHECK(ParseUsbipImport(buf, sizeof(buf), imp, &err));
    CHECK_EQ_STR(imp.busid, "3-2.1");

    // Wrong protocol version is refused (usbip-win2 requires 1.1.1).
    BuildUsbipImport("1-4", buf);
    buf[1] = 0x05;  // 0x0105
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));

    // Wrong command (e.g. OP_REQ_DEVLIST) is refused: only IMPORTS are tunnelled.
    BuildUsbipImport("1-4", buf);
    buf[2] = 0x80;
    buf[3] = 0x05;
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));

    // Short buffer, empty busid, garbage busid, missing NUL terminator.
    BuildUsbipImport("1-4", buf);
    CHECK(!ParseUsbipImport(buf, 12, imp, &err));
    BuildUsbipImport("", buf);
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));
    BuildUsbipImport("evil;;bus", buf);
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));
    BuildUsbipImport("1-4", buf);
    std::memset(buf + 8, 'A', 32);  // no NUL inside the fixed field
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));  // not a valid bus id -> refused

    // A bus id longer than the 32 byte field is truncated by the builder and
    // then refused by the validator, never silently accepted.
    BuildUsbipImport(std::string(40, '1'), buf);
    CHECK(!ParseUsbipImport(buf, sizeof(buf), imp, &err));
}

// The Public address field accepts a bare host, "host:port", a scheme prefix or a
// pasted URL. This is what makes free TCP tunnels (playit.gg, cloudflared, ngrok)
// usable, because they publish a random port that is not our local 4437.
static void TestSplitHostPort() {
    std::string host;
    uint16_t port = 0;

    SplitHostPort("192.168.1.20", &host, &port);
    CHECK_EQ_STR(host, "192.168.1.20");
    CHECK(port == 0);

    SplitHostPort("100.101.102.103:4437", &host, &port);
    CHECK_EQ_STR(host, "100.101.102.103");
    CHECK(port == 4437);

    SplitHostPort("  quiet-marble-otter.trycloudflare.com:25565 ", &host, &port);
    CHECK_EQ_STR(host, "quiet-marble-otter.trycloudflare.com");
    CHECK(port == 25565);

    SplitHostPort("tcp://something.at.playit.gg:12345", &host, &port);
    CHECK_EQ_STR(host, "something.at.playit.gg");
    CHECK(port == 12345);

    SplitHostPort("myhost.ddns.net/", &host, &port);
    CHECK_EQ_STR(host, "myhost.ddns.net");
    CHECK(port == 0);

    SplitHostPort("[::1]:6000", &host, &port);
    CHECK_EQ_STR(host, "::1");
    CHECK(port == 6000);

    // Out of range / junk ports fall back to the caller's default instead of
    // producing a nonsense endpoint in the pairing code.
    SplitHostPort("example.com:70000", &host, &port);
    CHECK_EQ_STR(host, "example.com:70000");
    CHECK(port == 0);

    SplitHostPort("example.com:0", &host, &port);
    CHECK(port == 0);

    SplitHostPort("", &host, &port);
    CHECK(host.empty());
    CHECK(port == 0);

    // A hostname that fits the 64-byte pairing-code field survives unchanged.
    const std::string tunnel = "something-random.at.playit.gg";
    SplitHostPort(tunnel + ":25565", &host, &port);
    CHECK(host.size() <= kMaxHostLen);
    CHECK(port == 25565);
}

int main() {
    std::printf("Mini USB Bridge core tests\n==========================\n");
    TestSha256();
    TestHmac();
    TestBase32();
    TestPairing();
    TestProofs();
    TestFrames();
    TestStrings();
    TestPairingDecisions();
    TestUsbipWire();
    TestSplitHostPort();
    std::printf("==========================\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
