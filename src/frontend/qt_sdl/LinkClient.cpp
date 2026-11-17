/*
    melonDS Link - PC side client, see LinkClient.h
*/

#include "LinkClient.h"
#include "LinkLang.h"
#include "lz4/lz4.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
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
        // the 3DS may need a while for SD card work: wait up to 30 s
        if (!recvAll(s, &r, sizeof(r), 30000)) goto fail;
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

void Client::pollEvents()
{
    uint16_t status;
    std::vector<uint8_t> out;
    if (TryRequest(LINK_CMD_POLLEVENT, nullptr, 0, status, &out) != 1 || status != LINK_OK) return;
    if (out.size() < sizeof(LinkEvent)) return;
    LinkEvent ev;
    memcpy(&ev, out.data(), sizeof(ev));
    if (out.size() < sizeof(ev) + ev.romPathLen + ev.savePathLen) return;
    if (ev.type == LINK_EVENT_ANSWER)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        answerId = (uint32_t)ev.romSize;
        answerChoice = (int)ev.reserved;
        haveAnswer = true;
        return;
    }
    if (ev.type == LINK_EVENT_LANGUAGE)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        remoteLang.assign((const char*)out.data() + sizeof(ev), std::min<size_t>(ev.romPathLen, 3));
        return;
    }
    if (ev.type == LINK_EVENT_STOPGAME) { stopWanted = true; return; }
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
        return;
    }
    if (ev.type == LINK_EVENT_RESUME) { resumeWanted = true; return; }
    if (ev.type == LINK_EVENT_VIDEO)
    {
        bool on = ev.reserved != 0;
        if (on && !videoWanted) videoResync = true;   // the 3DS has nothing: send everything
        videoWanted = on;
        return;
    }
    if (ev.type == LINK_EVENT_SCREENSHOT)
    {
        screenshotWanted = (int)(ev.reserved < LINK_SHOT_MODES ? ev.reserved : LINK_SHOT_BOTH);
        return;
    }
    if (ev.type != LINK_EVENT_LAUNCH) return;

    LaunchRequest r;
    r.romSize = ev.romSize;
    r.romPath.assign((const char*)out.data() + sizeof(ev), ev.romPathLen);
    r.savePath.assign((const char*)out.data() + sizeof(ev) + ev.romPathLen, ev.savePathLen);
    std::lock_guard<std::mutex> lock(launchMutex);
    pendingLaunch = r;
    haveLaunch = true;
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
        bool ok = waitSock(ts, 1, 3000);
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
            for (int i = 0; i < 20 && !stopRequested; i++)
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
            videoResync = true;
            haveVideoAddr = true;
        }

        state = State::Connected;
        connectCount++;
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
        while (!stopRequested)
        {
            int64_t t = nowMs();
            if (t - lastHello >= 1000)
            {
                uint32_t helo = LINK_UDP_HELLO;
                sendto(us, (const char*)&helo, sizeof(helo), 0, (sockaddr*)&udpAddr, sizeof(udpAddr));
                lastHello = t;
            }
            // asking for events doubles as keep-alive
            // never wait for a busy connection here: this loop also reads the
            // input of the 3DS, which must keep flowing (e.g. while a save is
            // being uploaded - the game would otherwise freeze for that time)
            if (t - lastPing >= 250)
            {
                uint16_t st;
                int r = TryRequest(LINK_CMD_PING, nullptr, 0, st);
                if (r == 0) break;
                if (r == 1) pollEvents();
                lastPing = t;
            }
            {
                std::unique_lock<std::mutex> lock(reqMutex, std::try_to_lock);
                if (lock.owns_lock() && tcpSock == -1) break;
            }

            if (!waitSock(us, 0, 50)) continue;

            LinkInputPacket pkt;
            sockaddr_in from = {};
            socklen_t flen = sizeof(from);
            int n;
            while ((n = recvfrom(us, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&from, &flen)) == (int)sizeof(pkt))
            {
                if (pkt.magic != LINK_MAGIC) continue;
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
                if (pkt.flags & LINK_INPUT_VIDEOACK)
                {
                    std::lock_guard<std::mutex> vlock(videoMutex);
                    ackSeq = pkt.videoAckSeq;
                    ackMask = pkt.videoAckMask;
                    ackNew = true;
                }
            }
        }

        haveVideoAddr = false;
        closeSockets();
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            lastInput = InputSnapshot();
        }
        if (!stopRequested && everConnected)
            pushMessage(TrS("client.lost"));
    }

    state = State::Disconnected;
}

void Client::SubmitBottomScreen(const uint32_t* pixels, bool bgra)
{
    std::lock_guard<std::mutex> lock(videoMutex);
    pendingFrame.resize(256 * 192);
    for (int i = 0; i < 256 * 192; i++)
    {
        uint32_t p = pixels[i];
        uint32_t r, g, b;
        if (bgra) { b = p & 0xFF; g = (p >> 8) & 0xFF; r = (p >> 16) & 0xFF; }
        else      { r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF; }
        pendingFrame[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    framePending = true;
    videoCv.notify_one();
}

void Client::videoWorker()
{
    std::vector<uint16_t> frame;
    while (!stopRequested)
    {
        bool gotFrame = false;
        {
            std::unique_lock<std::mutex> lock(videoMutex);
            videoCv.wait_for(lock, std::chrono::milliseconds(30),
                             [this] { return framePending || stopRequested; });
            if (stopRequested) break;
            if (framePending)
            {
                frame.swap(pendingFrame);
                framePending = false;
                gotFrame = true;
            }
        }
        if (!haveVideoAddr) continue;

        processAcks();

        if (gotFrame)
        {
            lastFrame = frame;
            sendTiles(frame);
        }
        else if (!lastFrame.empty())
        {
            // nothing new (e.g. emulation paused): still repair lost tiles and
            // replace half-resolution previews with the exact picture
            sendTiles(lastFrame);
        }
    }
}

// Decide for packets in flight whether the 3DS got them; lost packets get
// their tiles queued for resending right away.
void Client::processAcks()
{
    bool valid;
    uint32_t aSeq;
    uint64_t aMask;
    {
        std::lock_guard<std::mutex> lock(videoMutex);
        valid = ackValid || ackNew;
        ackValid = valid;
        ackNew = false;
        aSeq = ackSeq;
        aMask = ackMask;
    }

    const int64_t now = nowMs();
    while (!inFlight.empty())
    {
        SentPacket& p = inFlight.front();
        bool received = false, lost = false;
        if (valid)
        {
            int32_t d = (int32_t)(aSeq - p.seq);
            if (d >= 0)
            {
                received = d < 64 && ((aMask >> d) & 1);
                lost = !received && d >= 3;   // newer packets arrived, this one did not
            }
        }
        // no receipt in time (e.g. it was the last packet before a pause)
        if (!received && now - p.sentMs > 150) lost = true;
        if (!received && !lost) break;
        if (lost)
        {
            for (uint16_t t : p.tiles) tileLevel[t] = 255;
            statLost++;
        }
        else statReceived++;
        inFlight.pop_front();
    }

    // never let the list grow without bound
    while (inFlight.size() > 512)
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
            uint16_t c = src[y * 256 + x];
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
                if (indexOf(src[y * 256 + x])) row |= (uint8_t)(1 << x);
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
            for (int x = 0; x < 8; x++) row |= (uint16_t)(indexOf(src[y * 256 + x]) << (x * 2));
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
            for (int x = 0; x < 8; x++) row |= (uint32_t)indexOf(src[y * 256 + x]) << (x * 4);
            memcpy(out + 32 + y * 4, &row, 4);
        }
        size = 64;
        return LINK_TILE_PAL16;
    }
    for (int y = 0; y < 8; y++) memcpy(out + y * 16, &src[y * 256], 16);
    size = 128;
    return LINK_TILE_RAW;
}

}

namespace
{

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
                    uint16_t c = src[(by * block + dy) * 256 + bx * block + dx];
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
    if (level == 0) e.type = encodeTile(src, e.data, e.size);
    else if (level == 1) { encodeReduced(src, 2, (uint16_t*)e.data); e.type = LINK_TILE_HALF; e.size = 32; }
    else { encodeReduced(src, 4, (uint16_t*)e.data); e.type = LINK_TILE_QUARTER; e.size = 8; }
    // exact encodings that are already tiny are never worth reducing
    if (level > 0)
    {
        uint8_t tmp[128];
        size_t exactSize;
        unsigned exactType = encodeTile(src, tmp, exactSize);
        if (exactSize <= e.size) { e.type = exactType; e.size = exactSize; memcpy(e.data, tmp, exactSize); e.level = 0; }
    }
}

}

void Client::sendTiles(const std::vector<uint16_t>& frame)
{
    // Rule: every tile whose content changed is sent in the same update, so
    // the 3DS never shows a mix of old and new. If that does not fit the
    // budget, tiles are sent coarser (half, then quarter resolution). Spare
    // budget is used to sharpen previews and to refresh a few tiles.
    constexpr int kRefreshPerFrame = 4;
    constexpr double kMinBudget = 12 * 1024, kMaxBudget = 64 * 1024;

    if (frame.size() != 256 * 192) return;
    if (videoResync.exchange(false) || shownSource.size() != frame.size())
    {
        shownSource.assign(frame.size(), 0);
        tileLevel.assign(LINK_TILE_COUNT, 255);  // send everything
        inFlight.clear();
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            ackValid = ackNew = false;
        }
        if (videoSeq == 0)                     // start where a previous session cannot be ahead
            videoSeq = (uint32_t)(nowMs() * 4);
        budgetBytes = 24 * 1024;
        statReceived = statLost = 0;
    }

    // adapt the data rate: more while the WiFi keeps up, less when packets get lost
    if (statReceived + statLost >= 30)
    {
        double loss = (double)statLost / (statReceived + statLost);
        if (loss > 0.05) budgetBytes *= 0.75;
        else if (loss < 0.02) budgetBytes += 2048;
        budgetBytes = std::min(kMaxBudget, std::max(kMinBudget, budgetBytes));
        statReceived = statLost = 0;
    }
    const size_t budget = (size_t)budgetBytes;
    const double bytesPerSec = std::min(6.0e6, std::max(2.0e6, budgetBytes * 120.0));

    auto tileBase = [](int t) { return (t / LINK_TILES_X) * 8 * 256 + (t % LINK_TILES_X) * 8; };
    auto contentChanged = [&](int t)
    {
        int base = tileBase(t);
        for (int y = 0; y < 8; y++)
            if (memcmp(&frame[base + y * 256], &shownSource[base + y * 256], 16) != 0) return true;
        return false;
    };

    // 1) mandatory: changed content or unknown state (lost packets)
    std::vector<EncTile> must;
    std::vector<uint8_t> isMust(LINK_TILE_COUNT, 0);
    size_t total = 0;
    for (int t = 0; t < LINK_TILE_COUNT; t++)
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
    std::vector<uint8_t> isRefresh(LINK_TILE_COUNT, 0);
    for (int i = 0; i < kRefreshPerFrame; i++)
        isRefresh[(refreshCursor + i) % LINK_TILE_COUNT] = 1;
    refreshCursor = (refreshCursor + kRefreshPerFrame) % LINK_TILE_COUNT;
    int lastExtra = -1;
    for (int i = 0; i < LINK_TILE_COUNT && room > 12; i++)
    {
        int t = (int)((scanStart + i) % LINK_TILE_COUNT);
        if (isMust[t]) continue;
        if (tileLevel[t] == 0 && !isRefresh[t]) continue;
        EncTile e;
        e.t = t;
        makeLevel(e, &frame[tileBase(t)], 0);
        if (2 + e.size > room && tileLevel[t] == 2)
            makeLevel(e, &frame[tileBase(t)], 1);          // at least one step sharper
        if (2 + e.size > room || (e.level >= tileLevel[t] && !isRefresh[t])) continue;
        room -= 2 + e.size;
        extra.push_back(e);
        lastExtra = t;
    }
    if (lastExtra >= 0) scanStart = (uint32_t)((lastExtra + 1) % LINK_TILE_COUNT);

    if (must.empty() && extra.empty()) return;

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
    auto add = [&](const EncTile& e)
    {
        size_t need = 2 + e.size;
        if (cur.empty()) cur.resize(sizeof(LinkFramePacket));
        if (cur.size() + need > LINK_FRAME_MAX_BYTES)
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
            memcpy(&shownSource[base + y * 256], &frame[base + y * 256], 16);
        tileLevel[e.t] = e.level;
    };
    for (const EncTile& e : must) add(e);
    for (const EncTile& e : extra) add(e);
    finish();
    if (packets.empty()) return;

    // everything before baseSeq is confirmed, or lost and repaired by this frame
    // (processAcks ran just before; lost tiles were mandatory above)
    uint32_t baseSeq = inFlight.empty() ? videoSeq : inFlight.front().seq;

    sockaddr_in to;
    memcpy(&to, videoAddr, sizeof(to));
    uint32_t frameId = ++videoFrameId;
    auto t0 = std::chrono::steady_clock::now();
    size_t sentBytes = 0;
    for (size_t i = 0; i < packets.size(); i++)
    {
        std::vector<uint8_t>& pk = packets[i];
        LinkFramePacket hdr;
        memcpy(&hdr, pk.data(), sizeof(hdr));
        hdr.frameId = frameId;
        hdr.seq = videoSeq++;
        hdr.baseSeq = baseSeq;
        hdr.flags = (uint16_t)((i << LINK_FRAME_INDEX_SHIFT) | (i + 1 == packets.size() ? LINK_FRAME_LAST : 0));
        memcpy(pk.data(), &hdr, sizeof(hdr));
        {
            // own lock: requests (e.g. a save going to the 3DS) must not stop the picture
            std::lock_guard<std::mutex> lock(udpMutex);   // keeps udpSock alive
            if (udpSock == -1) return;
            sendto((sock_t)udpSock, (const char*)pk.data(), (int)pk.size(), 0, (sockaddr*)&to, sizeof(to));
        }
        inFlight.push_back({hdr.seq, nowMs(), std::move(packetTiles[i])});
        sentBytes += pk.size();

        // pacing, so the 3DS WiFi does not get the whole frame as one burst
        auto due = t0 + std::chrono::microseconds((long long)(sentBytes * 1e6 / bytesPerSec));
        while (std::chrono::steady_clock::now() < due && !stopRequested)
            std::this_thread::yield();
    }
}

Client& Get()
{
    static Client client;
    return client;
}

}
