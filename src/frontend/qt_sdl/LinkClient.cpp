/*
    melonDS Link - PC side client, see LinkClient.h
*/

#include "LinkClient.h"
#include "LinkLang.h"
#include "LinkPacing.h"
#include "LinkSession.h"
#include "lz4/lz4.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <qos2.h>
typedef SOCKET sock_t;
#define LINK_INVALID_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define LINK_INVALID_SOCK (-1)
#define closesock close
#endif

namespace Link
{

static std::atomic<uint32_t> g_mutedNotes { 0 };

bool NoteShown(Note n)
{
    return n == Note::Always || !(g_mutedNotes.load() & (1u << (int)n));
}

void SetNoteShown(Note n, bool shown)
{
    if (shown) g_mutedNotes &= ~(1u << (int)n);
    else g_mutedNotes |= (1u << (int)n);
}

namespace
{

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void netInit()
{
#ifdef _WIN32
    static bool done = false;
    if (!done)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

void setNonBlocking(sock_t s, bool nb)
{
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// wait until socket is readable (dir=0) or writable (dir=1)
bool waitSock(sock_t s, int dir, int timeoutMs)
{
#ifdef _WIN32
    WSAPOLLFD p = {};
    p.fd = s;
    p.events = dir ? POLLWRNORM : POLLRDNORM;
    return WSAPoll(&p, 1, timeoutMs) > 0;
#else
    pollfd p = {};
    p.fd = s;
    p.events = dir ? POLLOUT : POLLIN;
    return poll(&p, 1, timeoutMs) > 0;
#endif
}

bool resolve(const std::string& host, uint16_t port, sockaddr_in& out)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

bool sendAll(sock_t s, const void* buf, size_t len, int timeoutMs = 5000)
{
    const char* p = static_cast<const char*>(buf);
    while (len > 0)
    {
        if (!waitSock(s, 1, timeoutMs)) return false;
        int n = send(s, p, (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

std::string strf(const char* f, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

void log(const std::string& line)
{
    Session::Log(line);
}

bool recvAll(sock_t s, void* buf, size_t len, int timeoutMs = 10000)
{
    char* p = static_cast<char*>(buf);
    while (len > 0)
    {
        if (!waitSock(s, 0, timeoutMs)) return false;
        int n = recv(s, p, (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

}

Client::Client()
{
    netInit();
}

Client::~Client()
{
    Disconnect();
}

std::string Client::GetHost()
{
    std::lock_guard<std::mutex> lock(hostMutex);
    return currentHost;
}

void Client::Connect(const std::string& host)
{
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(hostMutex);
        currentHost = host;
    }
    stopRequested = false;
    state = State::Connecting;
    thread = std::thread(&Client::worker, this, host);
    videoThread = std::thread(&Client::videoWorker, this);
}

void Client::Disconnect()
{
    stopRequested = true;
    videoCv.notify_all();
    if (thread.joinable()) thread.join();
    if (videoThread.joinable()) videoThread.join();
    haveVideoAddr = false;
    closeSockets();
    state = State::Disconnected;
    std::lock_guard<std::mutex> lock(inputMutex);
    lastInput = InputSnapshot();
}

void Client::closeSockets()
{
    std::lock_guard<std::mutex> lock(reqMutex);
    std::lock_guard<std::mutex> ulock(udpMutex);
    if (tcpSock != -1) { closesock((sock_t)tcpSock); tcpSock = -1; }
    if (udpSock != -1) { closesock((sock_t)udpSock); udpSock = -1; }
    if (echoSock != -1) { closesock((sock_t)echoSock); echoSock = -1; }
}

// Marks a UDP socket's packets to `to` with a WiFi priority class: "voice" (top), "video"
// (above the other devices' normal traffic, below the buttons) or "background".
// Returns "" or what went wrong.
std::string Client::markQos(intptr_t sock, const void* toSockaddrIn, bool voice, bool background)
{
    sockaddr_in to;
    memcpy(&to, toSockaddrIn, sizeof(to));
#ifdef _WIN32
    static HANDLE qos = nullptr;
    if (!qos)
    {
        QOS_VERSION version = {1, 0};
        if (!QOSCreateHandle(&version, &qos))
        {
            qos = nullptr;
            return strf("QOSCreateHandle %lu", GetLastError());
        }
    }
    QOS_FLOWID flow = 0;
    if (!QOSAddSocketToFlow(qos, (SOCKET)sock, (PSOCKADDR)&to,
                            voice ? QOSTrafficTypeVoice : background ? QOSTrafficTypeBackground : QOSTrafficTypeAudioVideo,
                            QOS_NON_ADAPTIVE_FLOW, &flow))
        return strf("%lu", GetLastError());
    return "";
#else
    (void)to;
    int tos = voice ? 0xC0 : background ? 0x20 : 0xA0;
    if (setsockopt((sock_t)sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) return strf("%d", errno);
    return "";
#endif
}

void Client::pushMessage(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(msgMutex);
    messages.push_back(msg);
    while (messages.size() > 8) messages.pop_front();
}

bool Client::PopMessage(std::string& msg)
{
    std::lock_guard<std::mutex> lock(msgMutex);
    if (messages.empty()) return false;
    msg = messages.front();
    messages.pop_front();
    return true;
}

InputSnapshot Client::GetInput()
{
    std::lock_guard<std::mutex> lock(inputMutex);
    InputSnapshot s = lastInput;
    if (nowMs() - lastPacketTime > 500) s.valid = false;
    return s;
}

bool Client::Request(uint16_t cmd, const void* payload, uint32_t len,
                     uint16_t& status, std::vector<uint8_t>* out)
{
    std::lock_guard<std::mutex> lock(reqMutex);
    return requestLocked(cmd, payload, len, status, out);
}

int Client::TryRequest(uint16_t cmd, const void* payload, uint32_t len,
                       uint16_t& status, std::vector<uint8_t>* out)
{
    // the connection is busy (a save going to the 3DS, a ROM copy...):
    // don't wait for it, the caller tries again later
    std::unique_lock<std::mutex> lock(reqMutex, std::try_to_lock);
    if (!lock.owns_lock()) return -1;
    return requestLocked(cmd, payload, len, status, out) ? 1 : 0;
}

bool Client::requestLocked(uint16_t cmd, const void* payload, uint32_t len,
                           uint16_t& status, std::vector<uint8_t>* out)
{
    if (tcpSock == -1) return false;
    sock_t s = (sock_t)tcpSock;

    LinkHeader h = {};
    h.magic = LINK_MAGIC;
    h.cmd = cmd;
    h.seq = seq++;
    h.len = len;
    if (!sendAll(s, &h, sizeof(h))) goto fail;
    if (len && !sendAll(s, payload, len)) goto fail;

    {
        LinkHeader r = {};
        // the 3DS may need a while for SD card work: wait up to 30 s. Asking for
        // events is answered at once: there a silent 3DS (closed, asleep, out of
        // reach) is noticed after 4 s, so reconnecting can start right away
        int wait = (cmd == LINK_CMD_POLLEVENT || cmd == LINK_CMD_PING) ? 4000 : 30000;
        if (!recvAll(s, &r, sizeof(r), wait)) goto fail;
        if (r.magic != LINK_MAGIC || r.seq != h.seq || r.cmd != (cmd | LINK_RESP) ||
            r.len > LINK_MAX_PAYLOAD)
            goto fail;

        std::vector<uint8_t> tmp(r.len);
        if (r.len && !recvAll(s, tmp.data(), r.len)) goto fail;
        status = r.status;
        if (out) out->swap(tmp);
        return true;
    }

fail:
    // transport is broken; the worker notices and reports the disconnect
    closesock(s);
    tcpSock = -1;
    return false;
}

bool Client::Stat(const std::string& path, uint64_t& size, bool& isDir)
{
    uint16_t status;
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_STAT, path.data(), (uint32_t)path.size(), status, &out)) return false;
    if (status != LINK_OK || out.size() < sizeof(LinkStatResp)) return false;
    LinkStatResp r;
    memcpy(&r, out.data(), sizeof(r));
    size = r.size;
    isDir = r.isDir != 0;
    return true;
}

bool Client::Read(const std::string& path, uint64_t offset, uint32_t length, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> req(sizeof(LinkReadReq) + path.size());
    LinkReadReq r = {};
    r.offset = offset;
    r.length = length;
    memcpy(req.data(), &r, sizeof(r));
    memcpy(req.data() + sizeof(r), path.data(), path.size());
    uint16_t status;
    if (!Request(LINK_CMD_READ, req.data(), (uint32_t)req.size(), status, &out)) return false;
    return status == LINK_OK;
}

bool Client::Write(const std::string& path, uint64_t offset, const void* data, uint32_t length, bool create)
{
    std::vector<uint8_t> req(sizeof(LinkWriteReq) + path.size() + length);
    LinkWriteReq w = {};
    w.offset = offset;
    w.pathLen = (uint16_t)path.size();
    w.flags = create ? LINK_WRITE_CREATE : 0;
    memcpy(req.data(), &w, sizeof(w));
    memcpy(req.data() + sizeof(w), path.data(), path.size());
    if (length) memcpy(req.data() + sizeof(w) + path.size(), data, length);
    uint16_t status;
    if (!Request(LINK_CMD_WRITE, req.data(), (uint32_t)req.size(), status)) return false;
    return status == LINK_OK;
}

uint64_t Client::ReadStream(const std::string& path, uint64_t offset, uint64_t length,
                            const std::function<bool(const uint8_t*, size_t)>& sink, bool& ok,
                            bool compressed, bool skipPadding)
{
    ok = false;
    lastWireBytes = 0;
    std::lock_guard<std::mutex> lock(reqMutex);   // the connection is ours until the end
    if (tcpSock == -1) return 0;
    sock_t s = (sock_t)tcpSock;

    std::vector<uint8_t> req(sizeof(LinkStreamReq) + path.size());
    LinkStreamReq r = {};
    r.offset = offset;
    r.length = length;
    memcpy(req.data(), &r, sizeof(r));
    memcpy(req.data() + sizeof(r), path.data(), path.size());

    // skipPadding: the 3DS only spot-checks the padding behind the game data
    const uint16_t cmd = !compressed ? LINK_CMD_READSTREAM : skipPadding ? LINK_CMD_READSTREAMZS : LINK_CMD_READSTREAMZ;
    LinkHeader h = {};
    h.magic = LINK_MAGIC;
    h.cmd = cmd;
    h.seq = seq++;
    h.len = (uint32_t)req.size();

    uint64_t got = 0, wire = 0;
    LinkHeader resp = {};
    uint64_t total = 0;
    std::vector<uint8_t> buf(1024 * 1024), raw;
    if (!sendAll(s, &h, sizeof(h)) || !sendAll(s, req.data(), req.size())) goto broken;
    if (!recvAll(s, &resp, sizeof(resp)) || resp.magic != LINK_MAGIC || resp.seq != h.seq ||
        resp.cmd != (cmd | LINK_RESP))
        goto broken;
    if (resp.status != LINK_OK)
    {
        if (resp.len) { std::vector<uint8_t> junk(resp.len); recvAll(s, junk.data(), resp.len); }
        return 0;
    }
    if (resp.len != sizeof(total) || !recvAll(s, &total, sizeof(total))) goto broken;

    if (!compressed)
    {
        while (got < total)
        {
            if (!waitSock(s, 0, 10000)) goto broken;
            size_t want = (size_t)std::min<uint64_t>(buf.size(), total - got);
            int n = recv(s, (char*)buf.data(), (int)want, 0);
            if (n <= 0) goto broken;
            got += (uint64_t)n;
            wire += (uint64_t)n;
            lastWireBytes = wire;
            if (!sink(buf.data(), (size_t)n)) goto broken;   // cancelled: the stream must be dropped
        }
    }
    else
    {
        raw.resize(LINK_STREAM_BLOCK_SIZE);
        while (got < total)
        {
            LinkStreamBlock b;
            if (!recvAll(s, &b, sizeof(b))) goto broken;
            if (b.rawLen == 0 || b.rawLen > LINK_STREAM_BLOCK_SIZE || b.rawLen > total - got) goto broken;
            wire += sizeof(b);
            if (b.mode == LINK_BLOCK_FILL)
            {
                memset(raw.data(), b.fill, b.rawLen);
            }
            else if (b.mode == LINK_BLOCK_RAW)
            {
                if (b.encLen != b.rawLen || !recvAll(s, raw.data(), b.rawLen)) goto broken;
            }
            else if (b.mode == LINK_BLOCK_LZ4)
            {
                if (b.encLen == 0 || b.encLen > buf.size() || !recvAll(s, buf.data(), b.encLen)) goto broken;
                int n = LZ4_decompress_safe((const char*)buf.data(), (char*)raw.data(), (int)b.encLen, (int)b.rawLen);
                if (n != (int)b.rawLen) goto broken;
            }
            else goto broken;
            wire += b.encLen;
            lastWireBytes = wire;
            got += b.rawLen;
            if (!sink(raw.data(), b.rawLen)) goto broken;
        }
    }
    ok = true;
    return got;

broken:
    // the stream can't be resumed on this connection; the worker reconnects
    closesock(s);
    tcpSock = -1;
    return got;
}

bool Client::SpeedTest(uint32_t megabytes, double& bytesPerSec)
{
    LinkTuning t = {};
    t.megabytes = megabytes;
    return SpeedTest(t, bytesPerSec);
}

bool Client::SetTuning(const LinkTuning& t)
{
    uint16_t status;
    return Request(LINK_CMD_SETTUNING, &t, sizeof(t), status) && status == LINK_OK;
}

std::string Client::GetStats()
{
    uint16_t status;
    std::vector<uint8_t> out;
    if (!Request(LINK_CMD_GETSTATS, nullptr, 0, status, &out) || status != LINK_OK) return "";
    return std::string(out.begin(), out.end());
}

bool Client::SpeedTest(const LinkTuning& settings, double& bytesPerSec)
{
    uint32_t megabytes = settings.megabytes;
    bytesPerSec = 0;
    std::lock_guard<std::mutex> lock(reqMutex);
    if (tcpSock == -1) return false;
    sock_t s = (sock_t)tcpSock;

    LinkHeader h = {};
    h.magic = LINK_MAGIC;
    h.cmd = LINK_CMD_SPEEDTEST;
    h.seq = seq++;
    h.len = sizeof(settings);
    LinkHeader resp = {};
    uint64_t total = 0, got = 0;
    std::vector<uint8_t> buf(1024 * 1024);
    auto t0 = std::chrono::steady_clock::now();
    (void)megabytes;
    if (!sendAll(s, &h, sizeof(h)) || !sendAll(s, &settings, sizeof(settings))) goto broken;
    if (!recvAll(s, &resp, sizeof(resp)) || resp.magic != LINK_MAGIC || resp.seq != h.seq ||
        resp.cmd != (LINK_CMD_SPEEDTEST | LINK_RESP))
        goto broken;
    if (resp.status != LINK_OK)
    {
        if (resp.len) { std::vector<uint8_t> junk(resp.len); recvAll(s, junk.data(), resp.len); }
        return false;   // e.g. an older 3DS app that does not know the test
    }
    if (resp.len != sizeof(total) || !recvAll(s, &total, sizeof(total))) goto broken;
    t0 = std::chrono::steady_clock::now();
    while (got < total)
    {
        if (!waitSock(s, 0, 10000)) goto broken;
        int n = recv(s, (char*)buf.data(), (int)std::min<uint64_t>(buf.size(), total - got), 0);
        if (n <= 0) goto broken;
        got += (uint64_t)n;
    }
    {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        bytesPerSec = secs > 0 ? total / secs : 0;
    }
    return true;

broken:
    closesock(s);
    tcpSock = -1;
    return false;
}

bool Client::Replace(const std::string& from, const std::string& to)
{
    std::vector<uint8_t> req(2 + from.size() + to.size());
    uint16_t fl = (uint16_t)from.size();
    memcpy(req.data(), &fl, 2);
    memcpy(req.data() + 2, from.data(), from.size());
    memcpy(req.data() + 2 + from.size(), to.data(), to.size());
    uint16_t status;
    return Request(LINK_CMD_REPLACE, req.data(), (uint32_t)req.size(), status) && status == LINK_OK;
}

bool Client::TakeLaunchRequest(LaunchRequest& out)
{
    std::lock_guard<std::mutex> lock(launchMutex);
    if (!haveLaunch) return false;
    out = pendingLaunch;
    haveLaunch = false;
    return true;
}

int Client::pollEvents()
{
    uint16_t status;
    std::vector<uint8_t> out;
    int res = TryRequest(LINK_CMD_POLLEVENT, nullptr, 0, status, &out);
    if (res != 1 || status != LINK_OK) return res;
    if (out.size() < sizeof(LinkEvent)) return 1;
    LinkEvent ev;
    memcpy(&ev, out.data(), sizeof(ev));
    if (out.size() < sizeof(ev) + ev.romPathLen + ev.savePathLen) return 1;
    if (ev.type == LINK_EVENT_ANSWER)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        answerId = (uint32_t)ev.romSize;
        answerChoice = (int)ev.reserved;
        haveAnswer = true;
        return 1;
    }
    if (ev.type == LINK_EVENT_LANGUAGE)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        remoteLang.assign((const char*)out.data() + sizeof(ev), std::min<size_t>(ev.romPathLen, 3));
        return 1;
    }
    if (ev.type == LINK_EVENT_STOPGAME) { stopWanted = true; return 1; }
    if (ev.type == LINK_EVENT_BACKUPS || ev.type == LINK_EVENT_RESTORE)
    {
        std::string rom((const char*)out.data() + sizeof(ev), ev.romPathLen);
        std::lock_guard<std::mutex> lock(eventMutex);
        if (ev.type == LINK_EVENT_BACKUPS) { bakListRom = rom; haveBakList = true; }
        else
        {
            restoreRom = rom;
            restoreIndex = (int)ev.reserved;
            restoreToken = (uint32_t)ev.romSize;
            haveRestore = true;
        }
        return 1;
    }
    if (ev.type == LINK_EVENT_RESUME) { resumeWanted = true; return 1; }
    if (ev.type == LINK_EVENT_VIDEO)
    {
        bool on = ev.reserved != 0;
        if (on && !videoWanted) videoResync = true;   // the 3DS has nothing: send everything
        videoWanted = on;
        return 1;
    }
    if (ev.type == LINK_EVENT_SCREENSHOT)
    {
        screenshotWanted = (int)(ev.reserved < LINK_SHOT_MODES ? ev.reserved : LINK_SHOT_BOTH);
        return 1;
    }
    if (ev.type != LINK_EVENT_LAUNCH) return 1;

    LaunchRequest r;
    r.romSize = ev.romSize;
    r.romPath.assign((const char*)out.data() + sizeof(ev), ev.romPathLen);
    r.savePath.assign((const char*)out.data() + sizeof(ev) + ev.romPathLen, ev.savePathLen);
    std::lock_guard<std::mutex> lock(launchMutex);
    pendingLaunch = r;
    haveLaunch = true;
    return 1;
}

bool Client::Ask(uint32_t id, const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (haveAnswer && answerId == id) haveAnswer = false;
    }
    std::vector<uint8_t> payload(4 + text.size());
    memcpy(payload.data(), &id, 4);
    memcpy(payload.data() + 4, text.data(), text.size());
    uint16_t status;
    return Request(LINK_CMD_ASK, payload.data(), (uint32_t)payload.size(), status) && status == LINK_OK;
}

void Client::CancelAsk(uint32_t id)
{
    uint16_t status;
    Request(LINK_CMD_ASK, &id, 4, status);
}

bool Client::TakeBackupListRequest(std::string& romPath)
{
    std::lock_guard<std::mutex> lock(eventMutex);
    if (!haveBakList) return false;
    romPath = bakListRom;
    haveBakList = false;
    return true;
}

bool Client::TakeRestoreRequest(std::string& romPath, int& index, uint32_t& token)
{
    std::lock_guard<std::mutex> lock(eventMutex);
    if (!haveRestore) return false;
    romPath = restoreRom;
    index = restoreIndex;
    token = restoreToken;
    haveRestore = false;
    return true;
}

bool Client::TakeAnswer(uint32_t id, int& choice)
{
    std::lock_guard<std::mutex> lock(eventMutex);
    if (!haveAnswer || answerId != id) return false;
    choice = answerChoice;
    haveAnswer = false;
    return true;
}

std::string Client::Tr3DS(const char* key)
{
    return TrIn(RemoteLanguage(), key).toStdString();
}

bool Client::SetOptions(uint32_t opts)
{
    options = opts;
    uint16_t status;
    return Request(LINK_CMD_OPTIONS, &opts, 4, status) && status == LINK_OK;
}

bool Client::SetStatusText(const std::string& text)
{
    uint16_t status;
    return Request(LINK_CMD_STATUSTEXT, text.data(), (uint32_t)text.size(), status) && status == LINK_OK;
}

void Client::worker(std::string host)
{
    sockaddr_in tcpAddr = {}, udpAddr = {};
    if (!resolve(host, LINK_TCP_PORT, tcpAddr))
    {
        pushMessage(Tr("client.badaddr").arg(QString::fromStdString(host)).toStdString());
        state = State::Disconnected;
        return;
    }

    bool everConnected = false;

    // keep trying until the user disconnects
    while (!stopRequested)
    {
        state = State::Connecting;

        // --- TCP connect (non-blocking with timeout) ---
        sock_t ts = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        setNonBlocking(ts, true);
        connect(ts, (sockaddr*)&tcpAddr, sizeof(tcpAddr));
        bool ok = waitSock(ts, 1, 1500);   // a 3DS in the same WiFi answers far quicker
        if (ok)
        {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(ts, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
            ok = (err == 0);
        }
        if (!ok)
        {
            closesock(ts);
            // try again soon: after the 3DS woke up, every second counts
            for (int i = 0; i < 5 && !stopRequested; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        int one = 1;
        setsockopt(ts, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        int rcvbuf = 1024 * 1024;
        setsockopt(ts, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

        {
            std::lock_guard<std::mutex> lock(reqMutex);
            tcpSock = (intptr_t)ts;
        }

        // --- HELLO ---
        uint32_t ver = LINK_VERSION;
        uint16_t status = 0;
        std::vector<uint8_t> resp;
        bool helloOk = Request(LINK_CMD_HELLO, &ver, sizeof(ver), status, &resp);
        if (helloOk && status == LINK_ERR_VERSION)
        {
            // checked first: an older app answers with a shorter HELLO
            pushMessage(TrS("client.version"));
            closeSockets();
            state = State::Disconnected;
            return;
        }
        if (!helloOk || resp.size() < sizeof(LinkHelloResp))
        {
            closeSockets();
            continue;
        }
        LinkHelloResp hello;
        memcpy(&hello, resp.data(), sizeof(hello));
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            remoteLang.assign(hello.lang, strnlen(hello.lang, sizeof(hello.lang)));
            if (remoteLang.empty()) remoteLang = "en";
        }
        videoWanted = (hello.flags & LINK_HELLO_VIDEO_OFF) == 0;
        idle = false;
        {
            // a fresh connection: the 3DS is open (it reports otherwise right away)
            std::lock_guard<std::mutex> lock(inputMutex);
            console.lidClosed = false;
            lidClosed = false;
        }

        // --- UDP input stream ---
        udpAddr = tcpAddr;
        udpAddr.sin_port = htons(hello.udpPort);
        sock_t us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        {
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;
            bind(us, (sockaddr*)&local, sizeof(local));
            setNonBlocking(us, true);
        }
        {
            std::lock_guard<std::mutex> lock(reqMutex);
            std::lock_guard<std::mutex> ulock(udpMutex);
            udpSock = (intptr_t)us;
        }

        {
            sockaddr_in va = tcpAddr;
            va.sin_port = htons(LINK_VIDEO_PORT);
            static_assert(sizeof(va) == sizeof(videoAddr), "sockaddr_in size");
            memcpy(videoAddr, &va, sizeof(va));
            // WiFi priorities: the picture as "video" (above other devices' normal traffic),
            // the 3DS's buttons as "voice" above it; the echo for measuring as "voice" too
            int prio = wifiPrio;
            std::string pic = prio == 2 ? std::string() : markQos((intptr_t)us, &va, false, prio == 1);
            sock_t es = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            std::string echo = "no socket";
            if (es != LINK_INVALID_SOCK)
            {
                echo = markQos((intptr_t)es, &va, true, false);
                std::lock_guard<std::mutex> ulock(udpMutex);
                echoSock = (intptr_t)es;
            }
            log(strf("WiFi priority: picture as %s %s, echo as voice %s",
                     prio == 1 ? "background" : prio == 2 ? "normal" : "video",
                     pic.empty() ? "ok" : ("failed (" + pic + ")").c_str(),
                     echo.empty() ? "ok" : ("failed (" + echo + ")").c_str()));

            // what the 3DS app understands for the picture
            uint16_t st = 0;
            std::vector<uint8_t> caps;
            bool haveCaps = Request(LINK_CMD_VIDEOCAPS, nullptr, 0, st, &caps) && st == LINK_OK && caps.size() >= 4;
            uint32_t c = haveCaps ? caps[0] : 0;
            tileRs = (c & LINK_VIDCAP_TILE_RS) != 0;
            tileTs = (c & LINK_VIDCAP_TILE_TS) != 0;
            tileLz4 = (c & LINK_VIDCAP_TILE_LZ4) != 0;
            tileLz4x = (c & LINK_VIDCAP_TILE_LZ4X) != 0;
            echoOk = (c & LINK_VIDCAP_ECHO) != 0;
            h264Supported = H264Encoder::Available() && (c & LINK_VIDCAP_H264) != 0;
            h264Rs = h264Supported && (c & LINK_VIDCAP_H264_RS) != 0;
            h264Failed = false;
            h264Capable = h264Supported && optionH264;
            h264Silent = 0;
            linkGfInit();
            if (haveCaps && caps.size() >= 8)
            {
                int32_t tos;
                memcpy(&tos, caps.data() + 4, 4);
                log(tos == 0 ? std::string("WiFi priority: 3DS buttons as voice ok")
                             : strf("WiFi priority: 3DS buttons as voice failed (%d)", tos));
            }
            log(strf("video: tiles%s%s%s%s", tileRs ? ", parity" : "", tileTs ? ", time stamps" : "",
                     tileLz4 ? (tileLz4x ? ", LZ4 (MDFX)" : ", LZ4") : "", echoOk ? ", echo" : ""));
            log(h264Capable ? "video: H.264 (New 3DS hardware decoder)"
                : h264Supported ? "video: H.264 possible, switched off"
                : !H264Encoder::Available() ? "video: no H.264 in this build"
                : "video: the 3DS has no H.264 decoder (not a New 3DS)");
            videoResync = true;
            haveVideoAddr = true;
        }

        state = State::Connected;
        connectCount++;
        log("connected to " + host);
        if (NoteShown(Note::Connection))
            pushMessage(Tr("client.connected").arg(QString::fromUtf8(hello.name), QString::fromStdString(host)).toStdString());
        everConnected = true;
        SetStatusText(Tr3DS("status.connected"));
        {
            LinkTuning t;
            {
                std::lock_guard<std::mutex> l(tuneMutex);
                t = defaultTuning;
            }
            if (t.sndbuf || t.piece || t.sleepUs || t.flags) SetTuning(t);
        }

        int64_t lastHello = 0, lastPing = nowMs();
        // The buttons get their own thread at a high priority: nothing else (requests to the
        // 3DS that wait for its answer) may hold them up even a moment
        std::atomic<bool> inputRun { true };
        std::thread inputThread(&Client::inputLoop, this, (intptr_t)us, &inputRun);
        while (!stopRequested)
        {
            int64_t t = nowMs();
            if (t - lastHello >= 1000)
            {
                // + who we are: with Azahar connected too, the 3DS tells the two apart
                uint32_t helo[2] = {LINK_UDP_HELLO, LINK_CLIENT_MELONDS};
                sendto(us, (const char*)helo, sizeof(helo), 0, (sockaddr*)&udpAddr, sizeof(udpAddr));
                lastHello = t;
            }
            // asking for events is the keep-alive at the same time (no extra ping); never
            // waits for a busy connection (e.g. a save going to the 3DS)
            if (t - lastPing >= 250)
            {
                int r = pollEvents();
                if (r == 0) break;
                lastPing = t;
            }
            // closed (and it goes to sleep): drop the connection now and keep
            // knocking, so it is back as soon as the 3DS wakes up
            if (lidClosed && !(options.load() & LINK_OPT_KEEP_AWAKE))
            {
                lostByLid = true;
                break;
            }
            {
                std::unique_lock<std::mutex> lock(reqMutex, std::try_to_lock);
                if (lock.owns_lock() && tcpSock == -1) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        inputRun = false;
        inputThread.join();

        haveVideoAddr = false;
        closeSockets();
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            lastInput = InputSnapshot();
        }
        if (!stopRequested && everConnected && !lostByLid)
            if (NoteShown(Note::Connection)) pushMessage(TrS("client.lost"));
        if (lostByLid)
        {
            // it is falling asleep right now: knocking in the next moment would
            // only get a connection that dies with the sleep
            for (int i = 0; i < 15 && !stopRequested; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        lostByLid = false;
    }

    state = State::Disconnected;
}

void Client::inputLoop(intptr_t sock, std::atomic<bool>* run)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    sock_t us = (sock_t)sock;
    while (*run && !stopRequested)
    {
        if (!waitSock(us, 0, 20)) continue;

        LinkInputPacket pkt;
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        while (recvfrom(us, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&from, &flen) == (int)sizeof(pkt))
        {
            if (pkt.magic != LINK_MAGIC) continue;
            // Azahar is the active emulator on the 3DS: no buttons, no picture, no echo -
            // only the state of the console
            const bool nowIdle = (pkt.flags & LINK_INPUT_IDLE) != 0;
            if (nowIdle != idle.load())
            {
                idle = nowIdle;
                if (!nowIdle)
                {
                    videoResync = true;   // the 3DS shows our picture again: all of it
                    h264ForceKey = true;
                }
                log(nowIdle ? "3DS: Azahar is the active emulator - melonDS waits"
                            : "3DS: melonDS is the active emulator again");
            }
            if (nowIdle)
            {
                pkt.keys = 0;
                pkt.cpadX = pkt.cpadY = pkt.cstickX = pkt.cstickY = 0;
                pkt.flags = LINK_INPUT_IDLE;
            }
            {
                std::lock_guard<std::mutex> lock(inputMutex);
                // drop reordered packets (with wrap-around)
                if (lastInput.valid && (uint32_t)(pkt.id - lastPacketId) > 0x7FFFFFFFu) continue;
                lastPacketId = pkt.id;
                lastPacketTime = nowMs();
                lastInput.valid = true;
                lastInput.keys = pkt.keys;
                lastInput.cpadX = pkt.cpadX;
                lastInput.cpadY = pkt.cpadY;
                lastInput.touchX = pkt.touchX;
                lastInput.touchY = pkt.touchY;
                lastInput.cstickX = pkt.cstickX;
                lastInput.cstickY = pkt.cstickY;
                lastInput.dsTouchX = pkt.dsTouchX;
                lastInput.dsTouchY = pkt.dsTouchY;
                lastInput.flags = pkt.flags;
                console.known = true;
                console.battery = (pkt.status & 0xFF) <= 100 ? (int)(pkt.status & 0xFF) : -1;
                console.wifi = ((pkt.status >> 8) & 0xFF) <= 3 ? (int)((pkt.status >> 8) & 0xFF) : -1;
                console.charging = (pkt.status & LINK_STATUS_CHARGING) != 0;
                console.lidClosed = (pkt.status & LINK_STATUS_LIDCLOSED) != 0;
                lidClosed = console.lidClosed;
            }
            {
                // the 3DS sends at least every 50 ms: longer gaps mean its packets get stuck
                // in the WiFi - the picture then has to make room (see videoWorker)
                int64_t inMs = nowMs();
                int64_t gap = lastInputMs ? inMs - lastInputMs : 0;
                lastInputMs = inMs;
                if (gap > inputGapMax) inputGapMax = gap;
                if (gap > 100) inputGaps++;
                inputCount++;
            }
            // every 8th packet straight back: the 3DS measures the round trip; with our
            // clock and how far it is ahead of the pictures' time stamps, so the 3DS works
            // out how long after us it shows each picture
            if (echoOk && haveVideoAddr && !nowIdle && (pkt.id & 7) == 0)
            {
                uint32_t echo[4] = {LINK_ECHO_MAGIC, pkt.id, (uint32_t)nowMs(), emuClockOff.load()};
                sockaddr_in to;
                memcpy(&to, videoAddr, sizeof(to));
                std::lock_guard<std::mutex> lock(udpMutex);
                intptr_t es = echoSock != -1 ? echoSock : udpSock;
                if (es != -1) sendto((sock_t)es, (const char*)echo, sizeof(echo), 0, (sockaddr*)&to, sizeof(to));
            }
            if ((pkt.flags & LINK_INPUT_NEEDKEY) && !nowIdle) h264KeyWanted = true;
            if (pkt.flags & LINK_INPUT_VIDEOACK)
            {
                std::lock_guard<std::mutex> vlock(videoMutex);
                ackSeq = pkt.videoAckSeq;
                ackMask = pkt.videoAckMask;
                ackNew = true;
                // every confirmed seq is kept: the picture's thread may look only every
                // 100 ms (a big frame going out), and a receipt covers just 64 packets
                for (uint32_t d = 0; d < 64; d++)
                    if ((pkt.videoAckMask >> d) & 1)
                    {
                        uint32_t sq = pkt.videoAckSeq - d;
                        ackedRing[sq & (kAckRing - 1)] = sq;
                    }
            }
        }
    }
}

void Client::SubmitBottomScreen(const uint32_t* pixels, bool bgra, uint64_t emuFrame)
{
    // converted outside the lock: the sender never waits for the emulator, and back
    static thread_local std::vector<uint16_t> conv;
    conv.resize(256 * 192);
    for (int i = 0; i < 256 * 192; i++)
    {
        uint32_t p = pixels[i];
        uint32_t r, g, b;
        if (bgra) { b = p & 0xFF; g = (p >> 8) & 0xFF; r = (p >> 16) & 0xFF; }
        else      { r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF; }
        conv[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    std::lock_guard<std::mutex> lock(frameMutex);
    if (framePending) framesOverwritten++;
    pendingFrame.swap(conv);
    pendingEmuFrame = emuFrame;
    framePending = true;
    videoCv.notify_one();
}

namespace
{

constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr int kTilesX = LINK_TILES_X;
constexpr int kTiles = LINK_TILE_COUNT;
static_assert(kTilesX * 8 == kWidth && kTiles * 64 == kWidth * kHeight, "DS screen in tiles");

// when an emulated frame was made, in ms of emulated time (the pictures' time stamps)
uint32_t madeMs(uint64_t emuFrame)
{
    return (uint32_t)((double)emuFrame * (1000.0 / 59.8261)) & 0x7FFFFFFFu;
}

}

void Client::videoWorker()
{
    std::vector<uint16_t> frame;
    while (!stopRequested)
    {
        bool gotFrame = false;
        {
            std::unique_lock<std::mutex> lock(frameMutex);
            videoCv.wait_for(lock, std::chrono::milliseconds(30),
                             [this] { return framePending || stopRequested; });
            if (stopRequested) break;
            if (framePending)
            {
                frame.swap(pendingFrame);
                framePending = false;
                gotFrame = true;
                curEmuFrame = pendingEmuFrame;
            }
        }
        // our clock - the picture's time stamp (see LINK_ECHO_MAGIC)
        if (gotFrame) emuClockOff = (uint32_t)nowMs() - madeMs(curEmuFrame);
        if (!haveVideoAddr) continue;
        videoStats(nowMs());
        if (!videoWanted || lidClosed || idle) continue;

        // statistics: how many frames apart the picture changes
        if (gotFrame && frame != lastFrame)
        {
            uint64_t gap = curEmuFrame - prevChangedEmu;
            if (prevChangedEmu != 0 && gap >= 1 && gap <= 6) statsSrcGaps[std::min<uint64_t>(gap, 4) - 1]++;
            prevChangedEmu = curEmuFrame;
        }

        processAcks();

        // the buttons: a gap of more than 100 ms makes the picture smaller at once
        // (tiles: less data per second; H.264: lower bit rate)
        if (inputGaps != inputGapsSeen)
        {
            inputGapsSeen = inputGaps;
            inputSqueeze = true;
            if (h264Capable && h264.IsOpen())
            {
                h264Bitrate = std::max(2'000'000, (int)(h264Bitrate * 0.8));
                h264.SetBitrate(h264Bitrate);
            }
        }

        // the option switched while connected: from the next picture on
        const bool wantH264 = h264Supported && optionH264 && !h264Failed;
        if (wantH264 != h264Capable)
        {
            h264Capable = wantH264;
            videoResync = true;
            if (wantH264) h264ForceKey = true;   // the picture on the screen at once
            else tileLevel.assign(kTiles, 255);
            log(wantH264 ? "video: switched to H.264" : "video: switched to tiles");
        }
        if (h264Capable)
        {
            // the 3DS lost a frame it could not rebuild: a key frame at once. It keeps asking
            // until one arrived, so not again before the last one could have arrived
            if (h264KeyWanted.exchange(false) && nowMs() - h264KeyMs >= 70 && !h264ForceKey)
            {
                h264ForceKey = true;
                statsKeyAsks++;
            }
            if (gotFrame)
            {
                lastFrame = frame;
                sendH264(frame);
            }
            else if (!lastFrame.empty() && h264ForceKey)
                sendH264(lastFrame);   // a packet got lost while nothing moves: repair it
            continue;
        }

        // melonDS hands over every frame (60 a second) even if the game drew nothing new: an
        // unchanged one only gets the repairs - the time goes to the next real picture.
        // Steady frame rate: a new picture goes out only if it was made at least tileDiv
        // frames after the last one sent (60/tileDiv pictures a second at most, chosen to fit
        // the WiFi, see tilePace). Measured by when the pictures were made, so a 30 fps game
        // at "20" gets an even 15, not 2-4-2-4. A picture that comes too soon waits: the next
        // one replaces it, or it goes out when no new one comes.
        bool same = gotFrame && frame == lastFrame;
        bool sendNow = false;
        if (gotFrame && !same)
        {
            tileSameCount = 0;
            lastFrame = frame;
            // how many frames apart the game draws (2 = a 30 fps game)
            int64_t g = (int64_t)(curEmuFrame - tileLastEmu);
            if (g >= 1 && g <= 6) tileContentGap = 0.8 * tileContentGap + 0.2 * (double)g;
            tileLastEmu = curEmuFrame;
            tileHeld = true;
            int64_t since = (int64_t)(tileLastEmu - tileSentEmu);
            sendNow = since >= tileDiv || since < 0;   // (< 0: started over)
        }
        else if (tileHeld && gotFrame)
        {
            sendNow = (int64_t)(curEmuFrame - tileSentEmu) >= 2 * tileDiv;
        }
        if (sendNow)
        {
            tileHeld = false;
            tileSentEmu = tileLastEmu;
            tileSendEmu = tileLastEmu;
            sendTiles(lastFrame);
            tilePace();
        }
        else if (same && !tileHeld && tilesPending() && nowMs() - tilesSentMs >= 30)
        {
            // previews (or lost tiles) on the 3DS: sharpen them at once
            sendTiles(frame);
        }
        else if (same && !tileHeld && ++tileSameCount % 4 == 0)
        {
            sendTiles(frame);   // the slow refresh
        }
        else if (!gotFrame && !lastFrame.empty() && !tileHeld)
        {
            // nothing new (e.g. emulation paused): still repair lost tiles and replace
            // previews with the exact picture - once all is exact, stop. At most ~30 a second
            if ((tilesPending() || !inFlight.empty()) && nowMs() - tilesSentMs >= 30) sendTiles(lastFrame);
        }
    }
}

// How long the 3DS takes to confirm a packet. The shortest of the last seconds is the
// WiFi's own delay; how far the recent ones lie above it is the queue in front of the 3DS.
void Client::noteAckDelay(int64_t delay, int64_t now)
{
    if (delay < 0 || delay > 5000) return;
    if (now - ackMinSince > 5000)
    {
        ackMinPrev = ackMinCur;
        ackMinCur = delay;
        ackMinSince = now;
    }
    else ackMinCur = std::min(ackMinCur, delay);
    ackDelayRecent = 0.7 * ackDelayRecent + 0.3 * (double)delay;
    statsAckDelaySum += delay;
    statsAckDelayN++;
    statsAckDelayMax = std::max(statsAckDelayMax, delay);
}

// The steady picture rate: 60, 30, 20, 15 or 10 a second (every tileDiv-th DS frame). Like a
// console holding a frame rate: rather a steady 20 than 30 that keeps dropping. Down at once
// when the WiFi does not keep up; up only after some seconds with room to spare.
namespace
{
constexpr int kTileDivs[] = {1, 2, 3, 4, 6};
}

// a rate that fits how the game draws: a 30 fps game gets 30, 15 or 10 (20 is uneven there -
// every 3rd picture left out - and only with its option). Changes 4 or more frames apart
// (a menu with a blinking cursor, a still screen) have no rhythm to keep: every rate fits,
// each change goes out right away - the test: Pokemon's menus fell to 10-15 a second like
// that, and an animation starting there (the bag opening) came slowly
bool Client::tileDivFits(int d) const
{
    int cg = std::clamp((int)(tileContentGap + 0.5), 1, 6);
    if (cg >= 4) cg = 1;
    return d % cg == 0 || d == 6 || (d == 3 && optionTile20);
}

void Client::tileSlower(bool net)
{
    for (int d : kTileDivs)
        if (d > tileDiv && tileDivFits(d)) { tileDiv = d; break; }
    tileRoomSince = nowMs();
    if (net) tileNetSlowMs = tileRoomSince;
}

// tiles the 3DS does not have exactly yet (previews, lost ones)
bool Client::tilesPending() const
{
    for (uint8_t l : tileLevel) if (l != 0) return true;
    return false;
}

void Client::tilePace()
{
    int64_t now = nowMs();
    if (!tileDivFits(tileDiv)) { tileSlower(); return; }   // (the 20 fps option was switched off)
    // what the pictures need at this rate vs what the WiFi currently allows
    double need = tileFrameBytes * (60.0 / tileDiv);
    if (need > 0.9 * budgetBytes) { tileSlower(true); return; }
    // slower only because of how the game drew (not the WiFi): back up at once, to the fastest
    // rate that fits and has room - the animation that just started must not wait seconds
    if (now - tileNetSlowMs > 3000 && now - queueCutMs > 3000)
    {
        for (int d : kTileDivs)
        {
            if (d >= tileDiv) break;
            if (tileDivFits(d) && tileFrameBytes * (60.0 / d) < 0.6 * budgetBytes)
            {
                tileDiv = d;
                tileRoomSince = now;
                return;
            }
        }
    }
    int faster = 0;
    for (int d : kTileDivs) if (d < tileDiv && tileDivFits(d)) faster = d;
    if (faster == 0) { tileRoomSince = now; return; }
    bool room = tileFrameBytes * (60.0 / faster) < 0.6 * budgetBytes && now - queueCutMs > 3000;
    if (!room) tileRoomSince = now;
    else if (now - tileRoomSince > 3000)
    {
        tileDiv = faster;
        tileRoomSince = now;
    }
}

// Decide for packets in flight whether the 3DS got them; lost packets get
// their tiles queued for resending right away.
void Client::processAcks()
{
    std::lock_guard<std::mutex> lock(videoMutex);   // (ackedRing is filled by the input thread)
    bool valid = ackValid || ackNew;
    ackValid = valid;
    ackNew = false;
    uint32_t aSeq = ackSeq;
    uint64_t aMask = ackMask;

    int64_t now = nowMs();
    // how many packets went out that the 3DS has not confirmed yet: a real queue in the WiFi
    if (valid) acksBehind = (int32_t)((videoSeq - 1) - aSeq);
    while (!inFlight.empty())
    {
        SentPacket& p = inFlight.front();
        bool received = false, lost = false;
        if (valid)
        {
            int32_t d = (int32_t)(aSeq - p.seq);
            if (d >= 0)
            {
                received = (d < 64 && ((aMask >> d) & 1)) || ackedRing[p.seq & (kAckRing - 1)] == p.seq;
                if (p.fec || p.h264)
                    // the 3DS may still rebuild it from the parity at the end of its frame:
                    // only lost once packets after the whole frame were confirmed
                    lost = !received && (int32_t)(aSeq - p.frameEnd) >= 2;
                else
                    lost = !received && d >= 3;   // newer packets arrived, this one did not
            }
        }
        // no receipt in time (e.g. it was the last packet before a pause). With parity 600 ms:
        // a queue in the WiFi is no loss, and counting it as one sends everything again
        // H.264 waits longer as well: a wrong "lost" costs a key frame there
        if (!received && now - p.sentMs > (p.h264 ? 400 : p.fec ? 600 : 150)) lost = true;
        if (!received && !lost) break;
        if (lost)
        {
            for (uint16_t t : p.tiles) tileLevel[t] = 255;
            // the 3DS could not repair it: it waits for a key frame
            if (p.h264 && !p.parity) h264ForceKey = true;
            if (p.h264) h264LossSinceKey = true;
            statLost++;
            statsLost++;
            watchLost++;
        }
        else
        {
            statReceived++;
            statsReceived++;
            watchReceived++;
            noteAckDelay(now - p.sentMs, now);
        }
        inFlight.pop_front();
    }

    // Nearly everything "lost": receipts far behind (the WiFi queues our packets) or ahead of
    // what we sent (receipts stuck). Behind: much less data at once so the queue drains;
    // ahead: start over with a full picture.
    if (watchReceived + watchLost >= 200)
    {
        if (watchLost * 10 >= (watchReceived + watchLost) * 9)
        {
            int32_t behind = (int32_t)((videoSeq - 1) - aSeq);
            if (valid && behind < 0)
            {
                log(strf("video: receipts ahead of what was sent (%u vs %u) - starting over", aSeq, videoSeq - 1));
                videoResync = true;
                h264ForceKey = true;
            }
            else
            {
                log(strf("video: the WiFi holds back %d packets - much less data for now", behind));
                queueSqueeze = true;
            }
        }
        watchReceived = watchLost = 0;
    }

    // never let the list grow without bound
    while (inFlight.size() > 2048)
    {
        for (uint16_t t : inFlight.front().tiles) tileLevel[t] = 255;
        inFlight.pop_front();
    }
}

namespace
{

// Encode one 8x8 tile as compactly as possible. Returns the tile type and
// writes the payload into `out` (at most 128 bytes), size into `size`.
unsigned encodeTile(const uint16_t* src, uint8_t* out, size_t& size)
{
    uint16_t pal[16];
    int n = 0;
    for (int y = 0; y < 8 && n <= 16; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            uint16_t c = src[y * kWidth + x];
            int i = 0;
            while (i < n && pal[i] != c) i++;
            if (i == n)
            {
                if (n == 16) { n = 17; break; }
                pal[n++] = c;
            }
        }
    }

    auto indexOf = [&](uint16_t c) { int i = 0; while (pal[i] != c) i++; return i; };

    if (n == 1)
    {
        memcpy(out, &pal[0], 2);
        size = 2;
        return LINK_TILE_SOLID_T;
    }
    if (n == 2)
    {
        memcpy(out, pal, 4);
        for (int y = 0; y < 8; y++)
        {
            uint8_t row = 0;
            for (int x = 0; x < 8; x++)
                if (indexOf(src[y * kWidth + x])) row |= (uint8_t)(1 << x);
            out[4 + y] = row;
        }
        size = 12;
        return LINK_TILE_PAL2;
    }
    if (n <= 4)
    {
        memset(out, 0, 8);
        memcpy(out, pal, n * 2);
        for (int y = 0; y < 8; y++)
        {
            uint16_t row = 0;
            for (int x = 0; x < 8; x++) row |= (uint16_t)(indexOf(src[y * kWidth + x]) << (x * 2));
            memcpy(out + 8 + y * 2, &row, 2);
        }
        size = 24;
        return LINK_TILE_PAL4;
    }
    if (n <= 16)
    {
        memset(out, 0, 32);
        memcpy(out, pal, n * 2);
        for (int y = 0; y < 8; y++)
        {
            uint32_t row = 0;
            for (int x = 0; x < 8; x++) row |= (uint32_t)indexOf(src[y * kWidth + x]) << (x * 4);
            memcpy(out + 32 + y * 4, &row, 4);
        }
        size = 64;
        return LINK_TILE_PAL16;
    }
    for (int y = 0; y < 8; y++) memcpy(out + y * 16, &src[y * kWidth], 16);
    size = 128;
    return LINK_TILE_RAW;
}

// lossy previews: average 2x2 (HALF, 4x4 result) or 4x4 (QUARTER, 2x2 result) blocks
void encodeReduced(const uint16_t* src, int block, uint16_t* out)
{
    int n = 8 / block;
    for (int by = 0; by < n; by++)
        for (int bx = 0; bx < n; bx++)
        {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < block; dy++)
                for (int dx = 0; dx < block; dx++)
                {
                    uint16_t c = src[(by * block + dy) * kWidth + bx * block + dx];
                    r += c >> 11; g += (c >> 5) & 0x3F; b += c & 0x1F;
                }
            unsigned cnt = block * block;
            out[by * n + bx] = (uint16_t)(((r / cnt) << 11) | ((g / cnt) << 5) | (b / cnt));
        }
}

struct EncTile
{
    int t;
    unsigned type;
    size_t size;
    uint8_t level;      // 0 exact, 1 half, 2 quarter
    uint8_t data[128];
};

void makeLevel(EncTile& e, const uint16_t* src, uint8_t level)
{
    e.level = level;
    if (level == 0)
    {
        e.type = encodeTile(src, e.data, e.size);
        return;
    }
    uint16_t px[16];
    encodeReduced(src, level == 1 ? 2 : 4, px);
    e.type = level == 1 ? LINK_TILE_HALF : LINK_TILE_QUARTER;
    e.size = level == 1 ? 32 : 8;
    memcpy(e.data, px, e.size);
    // exact encodings that are already tiny are never worth reducing
    uint8_t tmp[128];
    size_t exactSize;
    unsigned exactType = encodeTile(src, tmp, exactSize);
    if (exactSize <= e.size) { e.type = exactType; e.size = exactSize; memcpy(e.data, tmp, exactSize); e.level = 0; }
}

int tileBase(int t)
{
    return (t / kTilesX) * 8 * kWidth + (t % kTilesX) * 8;
}

int tilePayload(unsigned type)
{
    switch (type)
    {
    case LINK_TILE_RAW: return 128;
    case LINK_TILE_SOLID_T: return 2;
    case LINK_TILE_PAL2: return 12;
    case LINK_TILE_PAL4: return 24;
    case LINK_TILE_PAL16: return 64;
    case LINK_TILE_HALF: return 32;
    case LINK_TILE_QUARTER: return 8;
    case LINK_TILE_META: return 4;
    default: return -1;
    }
}

}

// Tiles packed with LZ4 (see LinkProto.h): each raw packet (up to about twice the size of a
// packet) is packed; a packed packet that is still too big, or one that packing does not make
// smaller, is split at its middle entry until every piece fits - packed or as it is.
void Client::packTilePackets(std::vector<std::vector<uint8_t>>& packets,
                             std::vector<std::vector<uint16_t>>& packetTiles)
{
    constexpr size_t H = sizeof(LinkFramePacket);
    std::vector<std::vector<uint8_t>> out;
    std::vector<std::vector<uint16_t>> outTiles;
    std::vector<char> lz(LZ4_compressBound(4096));
    for (size_t p = 0; p < packets.size(); p++)
    {
        const std::vector<uint8_t>& raw = packets[p];
        // the entries: where each starts, and whether it is a tile (META is not)
        struct Ent { size_t off, len; bool tile; };
        std::vector<Ent> ents;
        for (size_t off = H; off + 2 <= raw.size();)
        {
            uint16_t head;
            memcpy(&head, &raw[off], 2);
            unsigned type = head >> LINK_TILE_TYPE_SHIFT;
            int sz = tilePayload(type);
            if (sz < 0 || off + 2 + sz > raw.size()) break;
            ents.push_back({off, (size_t)(2 + sz), type != LINK_TILE_META});
            off += 2 + sz;
        }
        // tiles of this packet, in entry order
        const std::vector<uint16_t>& tiles = packetTiles[p];
        std::vector<size_t> tileIndex(ents.size());
        for (size_t i = 0, t = 0; i < ents.size(); i++)
        {
            tileIndex[i] = t;
            if (ents[i].tile) t++;
        }
        // entries [a, b) as one packet, split if needed
        std::function<void(size_t, size_t)> packRange = [&](size_t a, size_t b)
        {
            if (a >= b) return;
            size_t from = ents[a].off, to = ents[b - 1].off + ents[b - 1].len;
            size_t rawLen = to - from;
            int c = LZ4_compress_default((const char*)&raw[from], lz.data(), (int)rawLen, (int)lz.size());
            std::vector<uint8_t> pk;
            LinkFramePacket hdr = {};
            hdr.entryCount = (uint16_t)(b - a);
            // MDFX: with the packed size (a packet rebuilt on the 3DS is zero-padded, and LZ4
            // needs the exact size)
            size_t zh = tileLz4x ? 4 : 2;
            if (c > 0 && (size_t)c + zh < rawLen && H + zh + (size_t)c <= LINK_FRAME_MAX_BYTES)
            {
                hdr.magic = tileLz4x ? LINK_FRAMEZX_MAGIC : LINK_FRAMEZ_MAGIC;
                pk.resize(H + zh + c);
                uint16_t rl = (uint16_t)rawLen, pl = (uint16_t)c;
                memcpy(&pk[H], &rl, 2);
                if (tileLz4x) memcpy(&pk[H + 2], &pl, 2);
                memcpy(&pk[H + zh], lz.data(), c);
            }
            else if (H + rawLen <= LINK_FRAME_MAX_BYTES)
            {
                hdr.magic = LINK_FRAME_MAGIC;
                pk.resize(H + rawLen);
                memcpy(&pk[H], &raw[from], rawLen);
            }
            else
            {
                size_t mid = a + (b - a) / 2;
                packRange(a, mid);
                packRange(mid, b);
                return;
            }
            memcpy(pk.data(), &hdr, H);
            statsCmpRaw += rawLen;
            statsCmpLz4 += pk.size() - H;
            size_t t0 = tileIndex[a];
            size_t t1 = b < ents.size() ? tileIndex[b] : tiles.size();
            out.push_back(std::move(pk));
            outTiles.emplace_back(tiles.begin() + t0, tiles.begin() + t1);
        };
        packRange(0, ents.size());
    }
    // how well it packs: the next frames may then take that much more (see sendTiles)
    size_t rawSum = 0, packedSum = 0;
    for (const auto& pk : out) packedSum += pk.size();
    for (const auto& pk : packets) rawSum += pk.size();
    if (rawSum > 4096) lz4Ratio = 0.8 * lz4Ratio + 0.2 * ((double)packedSum / rawSum);
    packets = std::move(out);
    packetTiles = std::move(outTiles);
}

void Client::sendTiles(const std::vector<uint16_t>& frame)
{
    // Rule: every tile whose content changed is sent in the same update, so the 3DS never
    // shows a mix of old and new. The data rate follows the WiFi (less when packets get lost
    // or a queue builds up); a frame may take one picture's time of it. Only if a frame would
    // take longer are tiles sent coarser (half, then quarter resolution). Spare budget
    // sharpens previews and refreshes a few tiles.
    constexpr int kRefreshPerFrame = 12;   // every tile again now and then: all 768 in ~64 frames
    constexpr double kMinRate = 0.5e6, kMaxRate = 4.0e6;   // bytes per second
    constexpr double kMaxFrameSeconds = 0.09;

    if (frame.size() != (size_t)(kWidth * kHeight)) return;
    auto encodeT0 = std::chrono::steady_clock::now();
    if (videoResync.exchange(false) || shownSource.size() != frame.size())
    {
        shownSource.assign(frame.size(), 0);
        tileLevel.assign(kTiles, 255);   // send everything
        inFlight.clear();
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            ackValid = ackNew = false;
        }
        if (videoSeq == 0)   // start where a previous session cannot be ahead
            videoSeq = (uint32_t)(nowMs() * 4);
        budgetBytes = 1.5e6;
        statReceived = statLost = 0;
    }

    // adapt the data rate: more while the WiFi keeps up, less when packets get lost
    if (statReceived + statLost >= 30)
    {
        double loss = (double)statLost / (statReceived + statLost);
        if (loss > 0.05) budgetBytes *= 0.8;
        else if (loss < 0.02 && nowMs() - queueCutMs > 1000) budgetBytes += 50e3;   // (not right after a queue)
        budgetBytes = std::min(kMaxRate, std::max(kMinRate, budgetBytes));
        statReceived = statLost = 0;
    }
    // the buttons come first: if their packets got stuck, make room at once
    if (inputSqueeze.exchange(false)) budgetBytes = std::max(kMinRate, budgetBytes * 0.7);
    // a queue builds up in the WiFi: less, before the picture lags behind by hundreds of ms.
    // Cut from what actually goes out, not from the allowance (which may be far above it)
    double actual = std::max(kMinRate, tilesRateActual);
    if (queueSqueeze.exchange(false))
    {
        budgetBytes = std::max(kMinRate, std::min(budgetBytes, actual) * 0.5);
        queueCutMs = nowMs();
        tileSlower(true);
    }
    else if (acksBehind > 150 && nowMs() - queueCutMs > 200)
    {
        // more than ~150 packets out but not confirmed: a real queue
        budgetBytes = std::max(kMinRate, std::min(budgetBytes, actual) * 0.75);
        queueCutMs = nowMs();
        statsQueueCuts++;
        tileSlower(true);
    }
    const double bytesPerSec = budgetBytes;
    // with the 3DS pacing by time stamps a frame must be out within one picture of a 30 fps
    // game (else it is late and the next one waits): bigger changes come as previews first
    // (packed with LZ4 the same bytes on the air hold that much more; the parity goes out too)
    const size_t budget = (size_t)(bytesPerSec * (tileTs ? std::min(tileDiv / 60.0, 1.0 / 30.0) : kMaxFrameSeconds) /
                                   (tileLz4 ? std::clamp(lz4Ratio, 0.35, 1.0) : 1.0) /
                                   (tileRs ? 4.0 / 3.0 : 1.0));

    auto contentChanged = [&](int t)
    {
        int base = tileBase(t);
        for (int y = 0; y < 8; y++)
            if (memcmp(&frame[base + y * kWidth], &shownSource[base + y * kWidth], 16) != 0) return true;
        return false;
    };

    // 1) mandatory: changed content or unknown state (lost packets)
    std::vector<EncTile> must;
    std::vector<uint8_t> isMust(kTiles, 0);
    size_t total = 0;
    for (int t = 0; t < kTiles; t++)
    {
        if (tileLevel[t] != 255 && !contentChanged(t)) continue;
        isMust[t] = 1;
        EncTile e;
        e.t = t;
        makeLevel(e, &frame[tileBase(t)], 0);
        total += 2 + e.size;
        must.push_back(e);
    }

    // degrade the biggest tiles step by step until everything fits
    for (uint8_t level = 1; level <= 2 && total > budget; level++)
    {
        std::vector<size_t> idx(must.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return must[a].size > must[b].size; });
        for (size_t i : idx)
        {
            if (total <= budget) break;
            EncTile& e = must[i];
            if (e.level >= level) continue;
            size_t before = e.size;
            makeLevel(e, &frame[tileBase(e.t)], level);
            total = total - before + e.size;
        }
    }

    // 2) optional, with what is left: sharpen previews, refresh a few tiles
    std::vector<EncTile> extra;
    size_t room = total < budget ? budget - total : 0;
    std::vector<uint8_t> isRefresh(kTiles, 0);
    for (int i = 0; i < kRefreshPerFrame; i++) isRefresh[(refreshCursor + i) % kTiles] = 1;
    refreshCursor = (refreshCursor + kRefreshPerFrame) % kTiles;
    int lastExtra = -1;
    for (int i = 0; i < kTiles && room > 12; i++)
    {
        int t = (int)((scanStart + i) % kTiles);
        if (isMust[t]) continue;
        if (tileLevel[t] == 0 && !isRefresh[t]) continue;
        EncTile e;
        e.t = t;
        makeLevel(e, &frame[tileBase(t)], 0);
        if (2 + e.size > room && tileLevel[t] == 2) makeLevel(e, &frame[tileBase(t)], 1);   // at least one step sharper
        if (2 + e.size > room || (e.level >= tileLevel[t] && !isRefresh[t])) continue;
        room -= 2 + e.size;
        extra.push_back(e);
        lastExtra = t;
    }
    if (lastExtra >= 0) scanStart = (uint32_t)((lastExtra + 1) % kTiles);

    if (must.empty() && extra.empty()) return;
    auto sendT0 = std::chrono::steady_clock::now();
    tilesSentMs = nowMs();
    statsEncodeMs += std::chrono::duration<double, std::milli>(sendT0 - encodeT0).count();

    // 3) pack: mandatory tiles first, then the extras
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::vector<uint16_t>> packetTiles;
    std::vector<uint8_t> cur;
    std::vector<uint16_t> curTiles;
    uint16_t curEntries = 0;
    auto finish = [&]()
    {
        if (!curEntries) return;
        LinkFramePacket hdr = {};
        hdr.magic = LINK_FRAME_MAGIC;
        hdr.entryCount = curEntries;
        memcpy(cur.data(), &hdr, sizeof(hdr));
        packets.push_back(std::move(cur));
        packetTiles.push_back(std::move(curTiles));
        cur.clear();
        curTiles.clear();
        curEntries = 0;
    };
    // with LZ4 the raw packets may be up to twice as big: each gets packed, and one that
    // does not fit then is split in two halves that fit even unpacked
    const size_t rawLimit = tileLz4 ? 2 * (LINK_FRAME_MAX_BYTES - sizeof(LinkFramePacket)) - 2 + sizeof(LinkFramePacket)
                                    : LINK_FRAME_MAX_BYTES;
    auto add = [&](const EncTile& e)
    {
        size_t need = 2 + e.size;
        if (cur.empty()) cur.resize(sizeof(LinkFramePacket));
        if (cur.size() + need > rawLimit)
        {
            finish();
            cur.resize(sizeof(LinkFramePacket));
        }
        uint16_t head = (uint16_t)(e.t | (e.type << LINK_TILE_TYPE_SHIFT));
        size_t off = cur.size();
        cur.resize(off + need);
        memcpy(&cur[off], &head, 2);
        memcpy(&cur[off + 2], e.data, e.size);
        curEntries++;
        curTiles.push_back((uint16_t)e.t);

        int base = tileBase(e.t);
        for (int y = 0; y < 8; y++)
            memcpy(&shownSource[base + y * kWidth], &frame[base + y * kWidth], 16);
        tileLevel[e.t] = e.level;
    };
    if (tileTs)
    {
        // when the picture was made: the 3DS shows them as evenly paced as they were made
        uint32_t made = madeMs(tileSendEmu);
        if (made == 0) made = 1;
        // nothing changed (only sharper previews, refreshes): the time of the picture it
        // belongs to - the 3DS then does not count it as a new picture in its pacing
        if (must.empty() && tileLastMade != 0) made = tileLastMade;
        tileLastMade = made;
        cur.resize(sizeof(LinkFramePacket) + 6);
        uint16_t head = (uint16_t)(LINK_TILE_META << LINK_TILE_TYPE_SHIFT);
        memcpy(&cur[sizeof(LinkFramePacket)], &head, 2);
        memcpy(&cur[sizeof(LinkFramePacket) + 2], &made, 4);
        curEntries = 1;
    }
    for (const EncTile& e : must) add(e);
    for (const EncTile& e : extra) add(e);
    finish();
    if (packets.empty()) return;
    if (tileLz4) packTilePackets(packets, packetTiles);

    // everything before baseSeq is confirmed, or lost and repaired by this frame
    uint32_t baseSeq = inFlight.empty() ? videoSeq : inFlight.front().seq;

    sockaddr_in to;
    memcpy(&to, videoAddr, sizeof(to));
    uint32_t frameId = ++videoFrameId;
    size_t k = packets.size();
    // Reed-Solomon parity (see LinkProto.h): about a third extra (at least 2) - each byte more
    // takes WiFi time from the buttons
    size_t groups = !tileRs || k > LINK_TILE_MAX_PACKETS
                        ? 0
                        : std::min<size_t>(LINK_TILE_MAX_PARITY, std::max<size_t>(2, (k + 2) / 3));
    uint32_t firstSeq = videoSeq;
    for (size_t i = 0; i < k; i++)
    {
        std::vector<uint8_t>& pk = packets[i];
        LinkFramePacket hdr;
        memcpy(&hdr, pk.data(), sizeof(hdr));
        hdr.frameId = frameId;
        hdr.seq = videoSeq++;
        hdr.baseSeq = baseSeq;
        hdr.flags = (uint16_t)((i << LINK_FRAME_INDEX_SHIFT) | (i + 1 == k ? LINK_FRAME_LAST : 0));
        memcpy(pk.data(), &hdr, sizeof(hdr));
    }
    std::vector<uint8_t> parity(groups * LINK_FRAME_MAX_BYTES, 0);
    for (size_t j = 0; j < groups; j++)
    {
        uint8_t* par = &parity[j * LINK_FRAME_MAX_BYTES];
        for (size_t i = 0; i < k; i++)
        {
            uint8_t c = linkCauchy((int)j, (int)i, (int)groups);
            uint8_t tab[256];
            for (int x = 0; x < 256; x++) tab[x] = linkGfMul(c, (uint8_t)x);
            const std::vector<uint8_t>& pk = packets[i];
            for (size_t b = 0; b < pk.size(); b++) par[b] ^= tab[pk[b]];
        }
    }
    auto t0 = std::chrono::steady_clock::now();
    size_t sentBytes = 0;
    std::vector<uint8_t> ppk;
    for (size_t i = 0; i < k + groups; i++)
    {
        bool isParity = i >= k;
        const std::vector<uint8_t>* out = &packets[std::min(i, k - 1)];
        if (isParity)
        {
            LinkFramePacket hdr = {};
            hdr.magic = LINK_TILEPAR_MAGIC;
            hdr.frameId = frameId;
            hdr.seq = firstSeq;
            hdr.baseSeq = (uint32_t)((k << 8) | (groups << 4) | (i - k));
            ppk.resize(sizeof(hdr) + LINK_FRAME_MAX_BYTES);
            memcpy(ppk.data(), &hdr, sizeof(hdr));
            memcpy(ppk.data() + sizeof(hdr), &parity[(i - k) * LINK_FRAME_MAX_BYTES], LINK_FRAME_MAX_BYTES);
            out = &ppk;
        }
        {
            // own lock: requests (e.g. a save going to the 3DS) must not stop the picture
            std::lock_guard<std::mutex> lock(udpMutex);   // keeps udpSock alive
            if (udpSock == -1) return;
            sendto((sock_t)udpSock, (const char*)out->data(), (int)out->size(), 0, (sockaddr*)&to, sizeof(to));
        }
        if (!isParity)
        {
            SentPacket sp;
            sp.seq = firstSeq + (uint32_t)i;
            sp.sentMs = nowMs();
            sp.tiles = std::move(packetTiles[i]);
            sp.fec = groups > 0;
            sp.frameEnd = firstSeq + (uint32_t)(k - 1);
            inFlight.push_back(std::move(sp));
        }
        sentBytes += out->size();

        // pacing at the data rate, so the 3DS WiFi does not get the whole frame as one burst
        auto due = t0 + std::chrono::microseconds((long long)(sentBytes * 1e6 / bytesPerSec));
        while (std::chrono::steady_clock::now() < due && !stopRequested)
            std::this_thread::yield();
    }
    double tookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sendT0).count();
    statsSendMs += tookMs;
    statsSendMaxMs = std::max(statsSendMaxMs, tookMs);
    if (!must.empty()) tileFrameBytes = 0.8 * tileFrameBytes + 0.2 * (double)sentBytes;
    // what really goes out per second (for cutting when a queue builds up)
    int64_t tnow = nowMs();
    if (tilesRateLastMs != 0 && tnow > tilesRateLastMs)
    {
        double inst = sentBytes * 1000.0 / (double)(tnow - tilesRateLastMs);
        tilesRateActual = 0.8 * tilesRateActual + 0.2 * std::min(inst, 8e6);
    }
    tilesRateLastMs = tnow;
    statsFrames++;
    statsBytes += sentBytes;
}

namespace
{

// R, G, B -> Y, U, V for the encoder: BT.601 TV range, what the 3DS decoder expects (measured
// in Azahar-Link: full range came out ~16 % brighter). Row i: R, G, B and the constant
constexpr float kYuv[3][4] = {{0.256788f, 0.504129f, 0.097906f, 16.0f},
                              {-0.148223f, -0.290993f, 0.439216f, 128.0f},
                              {0.439216f, -0.367788f, -0.071427f, 128.0f}};

// The DS picture in the middle of the 3DS bottom screen (320x240, black around it), turned
// for the 3DS framebuffer (coded column c, row r = screen x r, y 239 - c), as I420.
// The DS picture starts at an even x and y, so no 2x2 colour block mixes it with the black.
void toRotatedI420(const std::vector<uint16_t>& src, std::vector<uint8_t>& yuv)
{
    constexpr int W = LINK_H264_WIDTH, H = LINK_H264_HEIGHT;
    constexpr int ox = LINK_DS_OFFSET_X, oy = LINK_DS_OFFSET_Y;
    yuv.resize(W * H * 3 / 2);
    uint8_t* Y = yuv.data();
    uint8_t* U = Y + W * H;
    uint8_t* V = U + (W / 2) * (H / 2);
    int k[3][4];
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            k[i][j] = (int)(kYuv[i][j] * 65536.0f + (kYuv[i][j] < 0 ? -0.5f : 0.5f));
        k[i][3] = (int)(kYuv[i][3] * 65536.0f) + 32768;
    }
    auto clamp8 = [](int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    const uint8_t blackY = clamp8(k[0][3] >> 16);
    const uint8_t blackU = clamp8(k[1][3] >> 16), blackV = clamp8(k[2][3] >> 16);
    for (int r = 0; r < H; r += 2)        // r = screen x
    {
        const int dx = r - ox;
        const bool inX = dx >= 0 && dx + 1 < kWidth;
        for (int c = 0; c < W; c += 2)    // c = 239 - screen y
        {
            const int dy = (239 - c) - oy;   // the lower of the two rows is dy - 1
            if (!inX || dy - 1 < 0 || dy >= kHeight)
            {
                Y[r * W + c] = Y[r * W + c + 1] = Y[(r + 1) * W + c] = Y[(r + 1) * W + c + 1] = blackY;
                U[(r / 2) * (W / 2) + c / 2] = blackU;
                V[(r / 2) * (W / 2) + c / 2] = blackV;
                continue;
            }
            int sr = 0, sg = 0, sb = 0;
            for (int ry = 0; ry < 2; ry++)
                for (int cx = 0; cx < 2; cx++)
                {
                    const uint16_t p = src[(dy - cx) * kWidth + dx + ry];
                    const int R = ((p >> 11) & 31) * 255 / 31;
                    const int G = ((p >> 5) & 63) * 255 / 63;
                    const int B = (p & 31) * 255 / 31;
                    Y[(r + ry) * W + c + cx] = clamp8((k[0][0] * R + k[0][1] * G + k[0][2] * B + k[0][3]) >> 16);
                    sr += R; sg += G; sb += B;
                }
            U[(r / 2) * (W / 2) + c / 2] = clamp8(((k[1][0] * sr + k[1][1] * sg + k[1][2] * sb) / 4 + k[1][3]) >> 16);
            V[(r / 2) * (W / 2) + c / 2] = clamp8(((k[2][0] * sr + k[2][1] * sg + k[2][2] * sb) / 4 + k[2][3]) >> 16);
        }
    }
}

}

// New 3DS: the bottom screen as H.264 (as in Azahar-Link). Unchanged pictures are not sent;
// a lost packet makes the next frame a key frame (the 3DS shows nothing broken meanwhile).
void Client::sendH264(const std::vector<uint16_t>& frame)
{
    // spread out a bit (the 3DS WiFi does not like bursts), but quick enough that a frame
    // with its parity is there within ~15 ms
    constexpr double kPaceBytesPerSec = 2.0e6;
    if (frame.size() != (size_t)(kWidth * kHeight)) return;
    const auto sendT0 = std::chrono::steady_clock::now();
    if (videoResync.exchange(false) || !h264.IsOpen())
    {
        if (!h264.IsOpen() && !h264.Open(LINK_H264_WIDTH, LINK_H264_HEIGHT, 60, h264Bitrate))
        {
            log("video: the H.264 encoder did not start - tiles instead");
            h264Failed = true;
            h264Capable = false;
            videoResync = true;
            return;
        }
        inFlight.clear();
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            ackValid = ackNew = false;
        }
        if (videoSeq == 0) videoSeq = (uint32_t)(nowMs() * 4);
        h264Last.clear();
        h264ForceKey = true;
        h264SecondKey = true;   // the 3DS decoder needs two (see below)
        statReceived = statLost = 0;
    }

    // a queue in the WiFi: lower at once
    if (queueSqueeze.exchange(false))
    {
        h264Bitrate = std::max(2'000'000, (int)(h264Bitrate * 0.7));
        h264.SetBitrate(h264Bitrate);
    }
    // the data rate follows the WiFi: only heavy loss lowers it (losses come at 20 KB/s too)
    if (statReceived + statLost >= 60)
    {
        double loss = (double)statLost / (statReceived + statLost);
        int rate = h264Bitrate;
        if (loss > 0.08) rate = (int)(rate * 0.8);
        else if (loss < 0.04) rate += 250'000;
        rate = std::clamp(rate, 2'000'000, 8'000'000);
        if (rate != h264Bitrate)
        {
            h264Bitrate = rate;
            h264.SetBitrate(rate);
        }
        statReceived = statLost = 0;
    }

    bool force = h264ForceKey.exchange(false);
    const int64_t now = nowMs();
    if (!force && frame == h264Last)
    {
        // nothing new. Once it stood still for a quarter second, one key frame if something
        // got lost since the last one: the 3DS then shows exactly this picture
        if (h264Settled || !h264LossSinceKey || now - h264ChangedMs < 250) return;
        force = true;
    }
    if (frame != h264Last)
    {
        h264ChangedMs = now;
        h264Settled = false;
    }
    if (force) h264Settled = true;
    h264Last = frame;
    const auto encodeT0 = std::chrono::steady_clock::now();
    toRotatedI420(frame, h264Yuv);
    bool key = false;
    if (!h264.Encode(h264Yuv.data(), force, nowMs(), h264Out, key) || h264Out.empty())
    {
        if (force) h264ForceKey = true;   // try again with the next picture
        return;
    }
    const auto encodeT1 = std::chrono::steady_clock::now();
    statsEncodeMs += std::chrono::duration<double, std::milli>(encodeT1 - encodeT0).count();

    sockaddr_in to;
    memcpy(&to, videoAddr, sizeof(to));
    const uint32_t frameId = ++videoFrameId;
    const size_t parts = (h264Out.size() + LINK_H264_PART - 1) / LINK_H264_PART;
    if (parts > 160)
    {
        h264ForceKey = true;   // too big for the 3DS buffer: should never happen
        return;
    }
    // Reed-Solomon parity: any `groups` lost packets of the frame can be rebuilt on the 3DS.
    // About half extra (small frames: as many as data packets); key frames up to 15
    const size_t maxGroups = key ? LINK_H264_MAX_PARITY : 12;
    const size_t groups = !h264Rs ? 0
        : parts <= 2 ? std::min<size_t>(maxGroups, std::max<size_t>(2, parts))
        : std::min<size_t>(maxGroups, std::max<size_t>(2, (parts + 1) / 2));
    // when the picture was made: the 3DS shows them as evenly paced as they were made
    uint32_t made = madeMs(curEmuFrame);
    if (made == 0) made = 1;
    const size_t lastLen = h264Out.size() - (parts - 1) * LINK_H264_PART;
    std::vector<uint8_t> parity(groups * LINK_H264_PART, 0);
    for (size_t j = 0; j < groups; j++)
    {
        uint8_t* par = &parity[j * LINK_H264_PART];
        for (size_t i = 0; i < parts; i++)
        {
            uint8_t tab[256];
            const uint8_t c = linkCauchy((int)j, (int)i, (int)groups);
            for (int x = 0; x < 256; x++) tab[x] = linkGfMul(c, (uint8_t)x);
            const size_t off = i * LINK_H264_PART;
            const size_t len = std::min<size_t>(LINK_H264_PART, h264Out.size() - off);
            for (size_t b = 0; b < len; b++) par[b] ^= tab[h264Out[off + b]];
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t sentBytes = 0;
    std::vector<uint8_t> pk;
    for (size_t i = 0; i < parts + groups; i++)
    {
        const bool isParity = i >= parts;
        const size_t off = i * LINK_H264_PART;
        const size_t len = isParity ? LINK_H264_PART : std::min<size_t>(LINK_H264_PART, h264Out.size() - off);
        LinkFramePacket hdr = {};
        hdr.magic = LINK_H264_MAGIC;
        hdr.frameId = frameId;
        hdr.seq = videoSeq++;
        hdr.baseSeq = isParity ? (uint32_t)(LINK_H264_PARITY | (parts << 8) | (groups << 4) | (i - parts)) : made;
        hdr.entryCount = (uint16_t)(isParity ? lastLen : len);
        hdr.flags = (uint16_t)((i << LINK_FRAME_INDEX_SHIFT) | (i + 1 == parts ? LINK_FRAME_LAST : 0) |
                               (key ? LINK_FRAME_KEY : 0));
        pk.resize(sizeof(hdr) + len);
        memcpy(pk.data(), &hdr, sizeof(hdr));
        memcpy(pk.data() + sizeof(hdr), isParity ? &parity[(i - parts) * LINK_H264_PART] : &h264Out[off], len);
        {
            std::lock_guard<std::mutex> lock(udpMutex);   // keeps udpSock alive
            if (udpSock == -1) return;
            sendto((sock_t)udpSock, (const char*)pk.data(), (int)pk.size(), 0, (sockaddr*)&to, sizeof(to));
        }
        SentPacket sp;
        sp.seq = hdr.seq;
        sp.sentMs = nowMs();
        sp.h264 = true;
        sp.parity = isParity;
        sp.frameEnd = hdr.seq + (uint32_t)(parts + groups - 1 - i);
        inFlight.push_back(std::move(sp));
        sentBytes += pk.size();
        auto due = t0 + std::chrono::microseconds((long long)(sentBytes * 1e6 / kPaceBytesPerSec));
        while (std::chrono::steady_clock::now() < due && !stopRequested)
            std::this_thread::yield();
    }
    double tookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sendT0).count();
    statsSendMs += tookMs;
    statsSendMaxMs = std::max(statsSendMaxMs, tookMs);
    statsFrames++;
    statsBytes += sentBytes;
    if (key)
    {
        statsKeys++;
        h264KeyMs = nowMs();
        h264LossSinceKey = false;
        // the 3DS hardware decoder shows everything grey (and later frames broken) if its
        // first key frame is all it got: right after the first one, a second
        if (h264SecondKey)
        {
            h264SecondKey = false;
            h264ForceKey = true;
        }
    }
}

// The 3DS's statistics (LINK_CMD_VIDEOSTATS) without waiting: the request goes out in the
// background, its answer is used the next time (10 s later).
bool Client::statsReply(std::vector<uint8_t>& out)
{
    bool have = false;
    if (statsFuture.valid())
    {
        if (statsFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;   // the last one is still on its way
        out = statsFuture.get();
        have = !out.empty();
    }
    statsFuture = std::async(std::launch::async, [this]
    {
        uint16_t st = 0;
        std::vector<uint8_t> o;
        if (TryRequest(LINK_CMD_VIDEOSTATS, nullptr, 0, st, &o) == 1 && st == LINK_OK) return o;
        return std::vector<uint8_t>{};
    });
    return have;
}

// every 10 s some lines in the log: how melonDS runs and how the bottom screen goes
void Client::videoStats(int64_t now)
{
    if (statsSince == 0) { statsSince = now; return; }
    int64_t span = now - statsSince;
    if (span < 10000) return;
    for (const std::string& l : Pacing::Take()) log(l);

    double secs = span / 1000.0;
    log(strf("input: %u packets from the 3DS, longest gap %lld ms, gaps over 100 ms %u",
             inputCount.exchange(0), (long long)inputGapMax.exchange(0), inputGaps - inputGapsLogged));
    inputGapsLogged = inputGaps;
    if (statsFrames > 0)
    {
        double loss = statsReceived + statsLost ? 100.0 * statsLost / (statsReceived + statsLost) : 0.0;
        if (statsAckDelayN > 0)
            log(strf("video: receipts after %.0f ms on average (%lld at most, the WiFi's own %lld ms), "
                     "data cut %lld times for a queue",
                     (double)statsAckDelaySum / statsAckDelayN, (long long)statsAckDelayMax,
                     (long long)std::min(ackMinPrev, ackMinCur), (long long)statsQueueCuts));
        if (h264Capable)
            log(strf("video: H.264 %.1f frames/s, %.1f KB per frame, %.0f KB/s, %.1f %% lost, %llu key frames "
                     "(%llu asked for by the 3DS), target %d kbit/s, sending took %.1f ms on average (%.1f at "
                     "most, of it %.1f encoding)",
                     statsFrames / secs, statsBytes / 1024.0 / statsFrames, statsBytes / 1024.0 / secs, loss,
                     (unsigned long long)statsKeys, (unsigned long long)statsKeyAsks, h264Bitrate / 1000,
                     statsSendMs / statsFrames, statsSendMaxMs, statsEncodeMs / statsFrames));
        else
        log(strf("video: tiles %.1f frames/s, %.1f KB per frame, %.0f KB/s, %.1f %% lost, rate %.0f KB/s",
                 statsFrames / secs, statsBytes / 1024.0 / statsFrames, statsBytes / 1024.0 / secs, loss,
                 budgetBytes / 1024));
        if (statsCmpRaw > 0 && tileLz4 && !h264Capable)
            log(strf("video: tiles packed with LZ4 to %.0f %% of their size (%zu KB instead of %zu KB)",
                     100.0 * statsCmpLz4 / statsCmpRaw, statsCmpLz4 / 1024, statsCmpRaw / 1024));
    }
    statsAckDelaySum = statsAckDelayN = statsAckDelayMax = statsQueueCuts = 0;

    std::vector<uint8_t> out;
    if (statsReply(out) && out.size() >= 84)
    {
        uint32_t v[21] = {};
        memcpy(v, out.data(), sizeof(v));
        if (out.size() >= 96)
        {
            uint32_t r[3];
            memcpy(r, out.data() + 84, sizeof(r));
            if (r[0])
                log(strf("input: round trip 3DS -> PC -> 3DS %.1f ms on average, %.1f ms at most (%u measured) "
                         "- one way about %.1f ms", r[1] / 1000.0, r[2] / 1000.0, r[0], r[1] / 2000.0));
        }
        if (out.size() >= 108)
        {
            uint32_t l[3];
            memcpy(l, out.data() + 96, sizeof(l));
            if (l[0])
                log(strf("video: bottom picture on the 3DS %u ms on average (%u at most) after melonDS had it "
                         "(%u pictures)", l[1], l[2], l[0]));
        }
        if (out.size() >= 116)
        {
            uint32_t w[2];
            memcpy(w, out.data() + 108, sizeof(w));
            if (w[0] || w[1])
                log(strf("video: 3DS dropped %u outdated tile packets, %u because it was behind", w[0], w[1]));
        }
        if (h264Capable)
        {
            // H.264: what the decoder did (the first words of the answer)
            std::string extra;
            if (out.size() >= 36)
            {
                uint32_t f[3];
                memcpy(f, out.data() + 24, sizeof(f));
                extra = f[0] ? strf(", freezes %u (together %u ms, longest %u ms)", f[0], f[1], f[2]) : ", no freezes";
            }
            if (out.size() >= 52)
            {
                uint32_t g[4];
                memcpy(g, out.data() + 36, sizeof(g));
                extra += strf(", shown despite a loss %u, shown late %u, uneven %u, waited %u ms on average",
                              g[0], g[1], g[2], g[3]);
            }
            log(strf("video: 3DS shown %u, dropped (waiting for a key frame) %u, rebuilt packets %u, decoder "
                     "errors %u%s, made 1/2/3/4-6 frames apart in melonDS %u/%u/%u/%u",
                     v[0], v[1], v[2], v[3], extra.c_str(),
                     statsSrcGaps[0], statsSrcGaps[1], statsSrcGaps[2], statsSrcGaps[3]));
            // pictures went out, but the 3DS showed none: twice in a row, and tiles again
            if (v[0] == 0 && statsFrames >= 5)
            {
                if (++h264Silent >= 2)
                {
                    log("video: the 3DS shows no H.264 picture - tiles instead");
                    h264Failed = true;
                }
            }
            else h264Silent = 0;
        }
        else
        {
        int pending = (int)std::count_if(tileLevel.begin(), tileLevel.end(), [](uint8_t l) { return l != 0; });
        log(strf("video: 3DS shown %u (by the safety net %u), rebuilt packets %u, shown late %u, uneven %u, "
                 "waited %u ms on average, stood 1/2/3/4+ refreshes %u/%u/%u/%u, 3DS loop too slow %u, "
                 "made 1/2/3/4-6 frames apart in melonDS %u/%u/%u/%u, pictures replaced before sending %u, "
                 "sending took %.1f ms on average (%.1f at most, of it %.1f making the tiles), steady rate "
                 "%d pictures/s, tiles not yet exact %d",
                 v[18], v[19], v[20], v[10], v[11], v[12], v[14], v[15], v[16], v[17], v[13],
                 statsSrcGaps[0], statsSrcGaps[1], statsSrcGaps[2], statsSrcGaps[3], framesOverwritten.exchange(0),
                 statsFrames ? statsSendMs / statsFrames : 0.0, statsSendMaxMs,
                 statsFrames ? statsEncodeMs / statsFrames : 0.0, 60 / tileDiv, pending));
        }
    }
    statsSince = now;
    std::fill(std::begin(statsSrcGaps), std::end(statsSrcGaps), 0u);
    statsSendMs = statsSendMaxMs = statsEncodeMs = 0;
    statsCmpRaw = statsCmpLz4 = 0;
    statsFrames = statsBytes = statsReceived = statsLost = 0;
    statsKeys = statsKeyAsks = 0;
}

Client& Get()
{
    static Client client;
    return client;
}

}
