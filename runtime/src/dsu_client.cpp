// DSU ("cemuhook") controller client. Ported from the Luigi's Mansion recomp's src/recomp/dsu.cpp.
//
// Protocol (version 1001, little endian): every packet starts with a 16-byte header
// ("DSUC" from the client / "DSUS" from the server, u16 version, u16 length of what follows
// the header, u32 CRC32 of the whole packet with this field zero, u32 sender id), then a u32
// message type. The client asks for the slots' info (0x100001) and subscribes to pad data
// (0x100002, all slots); a server stops sending after about 5 s without a request, so both
// are repeated every second. Pad data: slot, state (2 = connected), model, connection,
// MAC, battery, connected flag, packet number, the DS4's buttons and analog values, touch
// and motion. Motion and touch are not used.
//
// Each connected (server, slot) is an SDL virtual gamepad named "DSU slot <n>" (or "DSU <server>
// slot <n>"), so VirtualRemote and Aurora's PAD code treat it like any other controller. The
// network thread only stores the latest packet per slot; the virtual pads are attached, updated
// and detached by Update() on the guest thread.
#include "dsu_client.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace DsuClient {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kProtocol = 1001;
constexpr uint32_t kMsgPorts = 0x100001, kMsgPadData = 0x100002;
constexpr int kSlots = 4;
constexpr int kMaxServers = 4;
constexpr double kSlotTimeout = 2.0;  // no pad data for this long: the pad is unplugged
constexpr double kServerTimeout = 3.0;

// What the server's controllers are: decides how the DS4-shaped DSU buttons become the
// virtual pad's buttons.
enum class Layout : int {
    GameCube,     // a GameCube controller behind the server (NSO GC Driver: A/B/X/Y = cross/
                  // circle/square/triangle, Z = R3, L/R = analog L2/R2 with L1/R1 at the click)
    PlayStation,  // a DS4-shaped pad (DS4Windows, BetterJoy, phone apps): buttons by position
};
struct Config {
    bool enabled = false;
    std::string servers;
    Layout layout = Layout::GameCube;
    bool operator==(const Config&) const = default;
};
std::mutex sCfgM;
Config sCfg;
std::atomic<unsigned> sCfgGen{1};

Config ReadConfig() {
    Config cfg;
    cfg.enabled = RuntimeConfigFile::DsuEnabled(false);
    cfg.servers = RuntimeConfigFile::DsuServers();
    cfg.layout = RuntimeConfigFile::DsuLayout() == "playstation" ? Layout::PlayStation : Layout::GameCube;
    return cfg;
}

std::vector<std::string> SplitServers(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= s.size(); i++) {
        const char c = i < s.size() ? s[i] : ';';
        if (c == ';' || c == ',') {
            const auto b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
            if (b != std::string::npos && out.size() < size_t(kMaxServers)) out.push_back(cur.substr(b, e - b + 1));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// ---- Packets ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* p, size_t n) {
    static const auto kTable = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void Put32(uint8_t* p, uint32_t v) { Put16(p, uint16_t(v)), Put16(p + 2, uint16_t(v >> 16)); }
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Get32(const uint8_t* p) { return uint32_t(Get16(p)) | uint32_t(Get16(p + 2)) << 16; }

// A client packet: header + message type + payload.
std::vector<uint8_t> MakePacket(uint32_t clientId, uint32_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p(20 + payload.size());
    std::memcpy(p.data(), "DSUC", 4);
    Put16(&p[4], kProtocol);
    Put16(&p[6], uint16_t(p.size() - 16));
    Put32(&p[12], clientId);
    Put32(&p[16], type);
    std::copy(payload.begin(), payload.end(), p.begin() + 20);
    Put32(&p[8], Crc32(p.data(), p.size()));
    return p;
}

// Latest pad data of one slot (offsets into the 100-byte pad data packet).
struct PadData {
    bool valid = false;
    Clock::time_point when;
    uint8_t model = 0, connection = 0, battery = 0;
    uint8_t buttons1 = 0, buttons2 = 0, home = 0, touch = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    unsigned packetsThisSecond = 0, packetsLastSecond = 0;
    Clock::time_point secondStart;
};

struct Server {
    std::string address, host, port;
    std::string error;
    SocketHandle sock = kNoSocket;
    sockaddr_storage addr{};
    socklen_t addrLen = 0;
    Clock::time_point lastValid{}, lastRequest{};
    uint32_t serverId = 0;
    uint16_t version = 0;
    std::array<PadData, kSlots> pads{};
};

std::mutex sM;  // sServers' status and pad data (network thread vs Update()/the settings menu)
std::vector<Server> sServers;
std::atomic<bool> sThreadStarted{false};

void CloseSocket(SocketHandle& s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    s = kNoSocket;
}

// Resolves "host[:port]" and opens a non-connected UDP socket for it.
bool OpenServer(Server& s) {
    const std::string& a = s.address;
    const auto colon = a.rfind(':');
    s.host = colon == std::string::npos ? a : a.substr(0, colon);
    s.port = colon == std::string::npos ? "26760" : a.substr(colon + 1);
    if (s.host.size() > 2 && s.host.front() == '[' && s.host.back() == ']') s.host = s.host.substr(1, s.host.size() - 2);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (const int rc = getaddrinfo(s.host.c_str(), s.port.c_str(), &hints, &res); rc != 0 || !res) {
        s.error = "cannot resolve " + s.address + ": " + gai_strerror(rc);
        return false;
    }
    std::memcpy(&s.addr, res->ai_addr, res->ai_addrlen);
    s.addrLen = socklen_t(res->ai_addrlen);
    s.sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    freeaddrinfo(res);
    if (s.sock == kNoSocket) {
        s.error = "cannot open a UDP socket";
        return false;
    }
    s.error.clear();
    return true;
}

void SendRequests(Server& s, uint32_t clientId) {
    const std::vector<uint8_t> ports = MakePacket(clientId, kMsgPorts, {kSlots, 0, 0, 0, 0, 1, 2, 3});
    // Registration flags 0: every slot (slot and MAC are ignored).
    const std::vector<uint8_t> data = MakePacket(clientId, kMsgPadData, {0, 0, 0, 0, 0, 0, 0, 0});
    for (const auto* p : {&ports, &data})
        sendto(s.sock, reinterpret_cast<const char*>(p->data()), int(p->size()), 0,
               reinterpret_cast<const sockaddr*>(&s.addr), s.addrLen);
}

// One received datagram from server s (sM held).
void HandlePacket(Server& s, const uint8_t* p, size_t n) {
    if (n < 20 || std::memcmp(p, "DSUS", 4) != 0) return;
    const uint16_t version = Get16(p + 4), len = Get16(p + 6);
    if (version > kProtocol || size_t(len) + 16 > n) return;
    n = size_t(len) + 16;
    std::vector<uint8_t> copy(p, p + n);
    Put32(&copy[8], 0);
    if (Crc32(copy.data(), n) != Get32(p + 8)) return;
    s.version = version;
    s.serverId = Get32(p + 12);
    s.lastValid = Clock::now();
    if (Get32(p + 16) != kMsgPadData || n < 56) return;
    const int slot = p[20];
    if (slot < 0 || slot >= kSlots) return;
    PadData& d = s.pads[size_t(slot)];
    const auto now = Clock::now();
    if (p[21] != 2 || !p[31]) {  // state "connected" and the connected flag
        d.valid = false;
        return;
    }
    d.valid = true;
    d.when = now;
    d.model = p[22];
    d.connection = p[23];
    d.battery = p[30];
    d.buttons1 = p[36];
    d.buttons2 = p[37];
    d.home = p[38];
    d.touch = p[39];
    d.lx = p[40], d.ly = p[41], d.rx = p[42], d.ry = p[43];
    d.l2 = p[55], d.r2 = p[54];
    if (std::chrono::duration<double>(now - d.secondStart).count() >= 1.0) {
        d.packetsLastSecond = d.packetsThisSecond;
        d.packetsThisSecond = 0;
        d.secondStart = now;
    }
    d.packetsThisSecond++;
}

// Network thread: (re)opens the configured servers, repeats the requests every second and
// stores what comes back. Runs for the whole session once DSU was first enabled.
void NetThread() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const uint32_t clientId = std::random_device{}();
    unsigned gen = 0;
    std::vector<uint8_t> buf(2048);
    for (;;) {
        if (const unsigned g = sCfgGen.load(); g != gen) {
            gen = g;
            Config cfg;
            {
                std::lock_guard lk(sCfgM);
                cfg = sCfg;
            }
            std::lock_guard lk(sM);
            for (auto& s : sServers) CloseSocket(s.sock);
            sServers.clear();
            if (cfg.enabled)
                for (const auto& a : SplitServers(cfg.servers)) {
                    Server s;
                    s.address = a;
                    if (OpenServer(s)) RT_LOGF(RT_TAG_CONFIG, "[dsu] client for %s (%s port %s)\n", a.c_str(), s.host.c_str(), s.port.c_str());
                    else RT_LOGF(RT_TAG_CONFIG, "[dsu] %s\n", s.error.c_str());
                    sServers.push_back(std::move(s));
                }
        }
        fd_set fds;
        FD_ZERO(&fds);
        SocketHandle maxFd = 0;
        {
            std::lock_guard lk(sM);
            const auto now = Clock::now();
            for (auto& s : sServers) {
                if (s.sock == kNoSocket) continue;
                if (std::chrono::duration<double>(now - s.lastRequest).count() >= 1.0) {
                    SendRequests(s, clientId);
                    s.lastRequest = now;
                }
                FD_SET(s.sock, &fds);
                maxFd = std::max(maxFd, s.sock);
            }
        }
        if (maxFd == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        timeval tv{0, 100000};
        if (select(int(maxFd + 1), &fds, nullptr, nullptr, &tv) <= 0) continue;
        std::lock_guard lk(sM);
        for (auto& s : sServers) {
            if (s.sock == kNoSocket || !FD_ISSET(s.sock, &fds)) continue;
            for (;;) {  // every datagram that is waiting
                sockaddr_storage from{};
                socklen_t fromLen = sizeof(from);
                const auto n = recvfrom(s.sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                        reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;
                HandlePacket(s, buf.data(), size_t(n));
                fd_set more;
                FD_ZERO(&more);
                FD_SET(s.sock, &more);
                timeval zero{0, 0};
                if (select(int(s.sock + 1), &more, nullptr, nullptr, &zero) <= 0) break;
            }
        }
    }
}

// ---- Virtual gamepads ------------------------------------------------------------------

struct Virtual {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    std::string key;  // pad name
    Layout layout = Layout::GameCube;
};
std::vector<Virtual> sVirtual;

std::string PadName(int server, int slot, size_t servers) {
    char n[48];
    if (servers > 1) std::snprintf(n, sizeof(n), "DSU %d slot %d", server + 1, slot + 1);
    else std::snprintf(n, sizeof(n), "DSU slot %d", slot + 1);
    return n;
}

Virtual Attach(const std::string& name, Layout layout) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // The ids decide SDL's gamepad type: Nintendo's GameCube adapter (labels, Z, L/R clicks on
    // MISC3/MISC4), or a DualShock 4 (PlayStation labels and prompts).
    const bool gc = layout == Layout::GameCube;
    desc.vendor_id = gc ? 0x057E : 0x054C;
    desc.product_id = gc ? 0x0337 : 0x09CC;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = name.c_str();
    Virtual v;
    v.key = name;
    v.layout = layout;
    v.id = SDL_AttachVirtualJoystick(&desc);
    v.joy = v.id ? SDL_OpenJoystick(v.id) : nullptr;
    if (v.joy)
        for (int t : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
            SDL_SetJoystickVirtualAxis(v.joy, t, SDL_JOYSTICK_AXIS_MIN);  // released
    RT_LOGF(RT_TAG_CONFIG, "[dsu] %s connected (virtual gamepad %u)%s\n", name.c_str(), unsigned(v.id),
            v.joy ? "" : SDL_GetError());
    return v;
}

void Detach(Virtual& v) {
    if (v.joy) SDL_CloseJoystick(v.joy);
    if (v.id) SDL_DetachVirtualJoystick(v.id);
    RT_LOGF(RT_TAG_CONFIG, "[dsu] %s disconnected\n", v.key.c_str());
    v = Virtual{};
}

// DSU stick byte (128 = centre, larger = right / up) to an SDL axis (negative = left / up).
Sint16 StickAxis(uint8_t v, bool flipY) {
    const int c = (int(v) - 128) * 256;
    return Sint16(std::clamp(flipY ? -c : c, -32768, 32767));
}
// DSU trigger byte (0..255) to a virtual trigger axis (-32768 released .. 32767 pulled).
Sint16 TriggerAxis(uint8_t v) { return Sint16(-32768 + int(v) * 257); }

void Apply(SDL_Joystick* j, const PadData& d, Layout layout) {
    const uint8_t b1 = d.buttons1, b2 = d.buttons2;
    if (layout == Layout::GameCube) {
        // SDL's GameCube buttons: A south, B west, X east, Y north, Z right shoulder, L/R clicks
        // MISC3/MISC4. The server sends B as circle, X as square, Z as R3 and the clicks as L1/R1.
        const std::pair<int, bool> buttons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, (b2 & 0x20) != 0},          {SDL_GAMEPAD_BUTTON_WEST, (b2 & 0x40) != 0},
            {SDL_GAMEPAD_BUTTON_EAST, (b2 & 0x10) != 0},           {SDL_GAMEPAD_BUTTON_NORTH, (b2 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, (b1 & 0x04) != 0}, {SDL_GAMEPAD_BUTTON_MISC3, (b2 & 0x05) != 0},
            {SDL_GAMEPAD_BUTTON_MISC4, (b2 & 0x0A) != 0},          {SDL_GAMEPAD_BUTTON_START, (b1 & 0x08) != 0},
            {SDL_GAMEPAD_BUTTON_BACK, (b1 & 0x01) != 0},           {SDL_GAMEPAD_BUTTON_LEFT_STICK, (b1 & 0x02) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, (b1 & 0x10) != 0},        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (b1 & 0x20) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, (b1 & 0x40) != 0},      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, (b1 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_GUIDE, d.home != 0},
        };
        for (const auto& [b, down] : buttons) SDL_SetJoystickVirtualButton(j, b, down);
    } else {
        const std::pair<int, bool> buttons[] = {
            {SDL_GAMEPAD_BUTTON_BACK, (b1 & 0x01) != 0},          {SDL_GAMEPAD_BUTTON_LEFT_STICK, (b1 & 0x02) != 0},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, (b1 & 0x04) != 0},   {SDL_GAMEPAD_BUTTON_START, (b1 & 0x08) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, (b1 & 0x10) != 0},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (b1 & 0x20) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, (b1 & 0x40) != 0},     {SDL_GAMEPAD_BUTTON_DPAD_LEFT, (b1 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, (b2 & 0x04) != 0}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, (b2 & 0x08) != 0},
            {SDL_GAMEPAD_BUTTON_WEST, (b2 & 0x10) != 0},          {SDL_GAMEPAD_BUTTON_SOUTH, (b2 & 0x20) != 0},
            {SDL_GAMEPAD_BUTTON_EAST, (b2 & 0x40) != 0},          {SDL_GAMEPAD_BUTTON_NORTH, (b2 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_GUIDE, d.home != 0},              {SDL_GAMEPAD_BUTTON_TOUCHPAD, d.touch != 0},
        };
        for (const auto& [b, down] : buttons) SDL_SetJoystickVirtualButton(j, b, down);
    }
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, StickAxis(d.lx, false));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, StickAxis(d.ly, true));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, StickAxis(d.rx, false));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTY, StickAxis(d.ry, true));
    // A server that only sets the L2/R2 bits still gives a full pull.
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, TriggerAxis((b2 & 0x01) ? std::max<uint8_t>(d.l2, 255) : d.l2));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, TriggerAxis((b2 & 0x02) ? std::max<uint8_t>(d.r2, 255) : d.r2));
}

const char* ConnectionName(uint8_t c) {
    switch (c) {
    case 1: return "USB";
    case 2: return "Bluetooth";
    default: return "network";
    }
}
const char* BatteryName(uint8_t b) {
    switch (b) {
    case 0x01: return "dying";
    case 0x02: return "low";
    case 0x03: return "medium";
    case 0x04: return "high";
    case 0x05: return "full";
    case 0xEE: return "charging";
    case 0xEF: return "charged";
    default: return "";
    }
}

// Pushes the saved settings to the network thread when they changed.
Config CurrentConfig() {
    const Config cfg = ReadConfig();
    std::lock_guard lk(sCfgM);
    if (!(cfg == sCfg)) {
        sCfg = cfg;
        sCfgGen++;
    }
    return cfg;
}

}  // namespace

void Update() {
    const Config cfg = CurrentConfig();
    if (cfg.enabled && !sThreadStarted.exchange(true)) std::thread(NetThread).detach();
    std::lock_guard lk(sM);
    const auto now = Clock::now();
    std::vector<std::string> live;
    for (size_t s = 0; s < sServers.size(); s++)
        for (int slot = 0; slot < kSlots; slot++) {
            const PadData& d = sServers[s].pads[size_t(slot)];
            if (!d.valid || std::chrono::duration<double>(now - d.when).count() > kSlotTimeout) continue;
            const std::string key = PadName(int(s), slot, sServers.size());
            live.push_back(key);
            auto it = std::find_if(sVirtual.begin(), sVirtual.end(), [&](const Virtual& v) { return v.key == key; });
            if (it != sVirtual.end() && it->layout != cfg.layout) {  // layout changed: plug in again as the other type
                Detach(*it);
                sVirtual.erase(it);
                it = sVirtual.end();
            }
            if (it == sVirtual.end()) {
                sVirtual.push_back(Attach(key, cfg.layout));
                it = sVirtual.end() - 1;
            }
            if (it->joy) Apply(it->joy, d, cfg.layout);
        }
    for (auto it = sVirtual.begin(); it != sVirtual.end();) {
        if (std::find(live.begin(), live.end(), it->key) == live.end()) {
            Detach(*it);
            it = sVirtual.erase(it);
        } else {
            ++it;
        }
    }
}

void DrawSettingsMenu() {
    if (!ImGui::BeginMenu("DSU controllers (NSO GC Driver)")) {
        return;
    }
    static bool sEnabled = RuntimeConfigFile::DsuEnabled(false);
    static char sServersBuf[256];
    static bool sLoaded = false;
    if (!sLoaded) {
        std::snprintf(sServersBuf, sizeof(sServersBuf), "%s", RuntimeConfigFile::DsuServers().c_str());
        sLoaded = true;
    }
    ImGui::TextDisabled("Pads served over UDP with the DSU (cemuhook) protocol, such as NSO GC Driver.");
    ImGui::TextDisabled("Each connected slot appears as a gamepad. Motion is not used.");
    if (ImGui::Checkbox("Enable DSU client", &sEnabled)) {
        RuntimeConfigFile::SetDsuEnabled(sEnabled);
    }
    ImGui::BeginDisabled(!sEnabled);
    if (ImGui::InputText("Servers", sServersBuf, sizeof(sServersBuf), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        RuntimeConfigFile::SetDsuServers(sServersBuf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("host:port of each DSU server, separated by ';' (up to %d).\n"
                          "NSO GC Driver on this Mac is 127.0.0.1:26760.", kMaxServers);
    }
    int layout = RuntimeConfigFile::DsuLayout() == "playstation" ? 1 : 0;
    if (ImGui::Combo("Controller layout", &layout, "GameCube controller\0PlayStation (DS4) or other pad\0")) {
        RuntimeConfigFile::SetDsuLayout(layout == 1 ? "playstation" : "gamecube");
    }
    ImGui::EndDisabled();
    if (sEnabled) {
        ImGui::Separator();
        std::lock_guard lk(sM);
        const auto now = Clock::now();
        if (sServers.empty()) ImGui::TextDisabled("No servers.");
        for (size_t s = 0; s < sServers.size(); s++) {
            const Server& sv = sServers[s];
            const bool answering = sv.lastValid != Clock::time_point{} &&
                                   std::chrono::duration<double>(now - sv.lastValid).count() < kServerTimeout;
            if (!sv.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s", sv.address.c_str(), sv.error.c_str());
            else if (!answering) ImGui::Text("%s: waiting for the server...", sv.address.c_str());
            else ImGui::Text("%s: connected (protocol %u)", sv.address.c_str(), sv.version);
            int shown = 0;
            for (int slot = 0; slot < kSlots; slot++) {
                const PadData& d = sv.pads[size_t(slot)];
                if (!d.valid || std::chrono::duration<double>(now - d.when).count() > kSlotTimeout) continue;
                const char* battery = BatteryName(d.battery);
                ImGui::BulletText("%s: %s%s%s, %u packets/s", PadName(int(s), slot, sServers.size()).c_str(),
                                  ConnectionName(d.connection), *battery ? ", battery " : "", battery,
                                  d.packetsLastSecond);
                shown++;
            }
            if (answering && !shown) ImGui::BulletText("No controller connected to this server.");
        }
    }
    ImGui::EndMenu();
}

}  // namespace DsuClient
