/*
    melonDS Link - PC side client, see LinkClient.h
*/

#include "LinkClient.h"

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
}

void Client::Disconnect()
{
    stopRequested = true;
    if (thread.joinable()) thread.join();
    closeSockets();
    state = State::Disconnected;
    std::lock_guard<std::mutex> lock(inputMutex);
    lastInput = InputSnapshot();
}

void Client::closeSockets()
{
    std::lock_guard<std::mutex> lock(reqMutex);
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
        if (!recvAll(s, &r, sizeof(r))) goto fail;
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
        pushMessage("3DS Link: Adresse ungültig: " + host);
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

        {
            std::lock_guard<std::mutex> lock(reqMutex);
            tcpSock = (intptr_t)ts;
        }

        // --- HELLO ---
        uint32_t ver = LINK_VERSION;
        uint16_t status = 0;
        std::vector<uint8_t> resp;
        if (!Request(LINK_CMD_HELLO, &ver, sizeof(ver), status, &resp) ||
            resp.size() < sizeof(LinkHelloResp))
        {
            closeSockets();
            continue;
        }
        LinkHelloResp hello;
        memcpy(&hello, resp.data(), sizeof(hello));
        if (status == LINK_ERR_VERSION)
        {
            pushMessage("3DS Link: Versionen passen nicht zusammen - bitte beide Programme aktualisieren");
            closeSockets();
            state = State::Disconnected;
            return;
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
            udpSock = (intptr_t)us;
        }

        state = State::Connected;
        pushMessage(std::string("3DS Link: verbunden mit ") + hello.name + " (" + host + ")");
        everConnected = true;
        SetStatusText("melonDS verbunden");

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
            if (t - lastPing >= 2000)
            {
                uint16_t st;
                if (!Request(LINK_CMD_PING, nullptr, 0, st)) break;
                lastPing = t;
            }
            {
                std::lock_guard<std::mutex> lock(reqMutex);
                if (tcpSock == -1) break;
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
            }
        }

        closeSockets();
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            lastInput = InputSnapshot();
        }
        if (!stopRequested && everConnected)
            pushMessage("3DS Link: Verbindung verloren - versuche neu zu verbinden...");
    }

    state = State::Disconnected;
}

Client& Get()
{
    static Client client;
    return client;
}

}
