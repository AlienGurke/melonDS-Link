/*
    melonDS Link - PC side client for the link3ds homebrew.

    One global client (instance 0 uses it). A worker thread keeps the TCP
    control connection and the UDP input stream alive; the emulator thread
    only reads the latest input snapshot, so it never blocks on the network.
*/

#ifndef MELONDS_LINKCLIENT_H
#define MELONDS_LINKCLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LinkProto.h"

namespace Link
{

enum class State
{
    Disconnected,
    Connecting,
    Connected,
};

struct InputSnapshot
{
    bool valid = false;        // a recent packet exists
    uint32_t keys = 0;         // raw 3DS HID keys
    int16_t cpadX = 0, cpadY = 0;
    uint16_t touchX = 0, touchY = 0;
    int16_t cstickX = 0, cstickY = 0;
    uint16_t dsTouchX = 0, dsTouchY = 0;
    uint32_t flags = 0;        // LINK_INPUT_*
};

class Client
{
public:
    Client();
    ~Client();

    // start connecting in the background (returns immediately)
    void Connect(const std::string& host);
    void Disconnect();

    State GetState() const { return state.load(); }
    std::string GetHost();

    // latest input from the 3DS; invalid if nothing arrived for 500 ms
    InputSnapshot GetInput();

    // DS bottom screen for the 3DS. `pixels` is 256x192, 32 bit per pixel;
    // bgra=true: bytes B,G,R,A (software renderer), false: R,G,B,A (OpenGL).
    // Cheap: converts and hands the frame to the sender thread.
    void SubmitBottomScreen(const uint32_t* pixels, bool bgra);
    bool WantsVideo() const { return state.load() == State::Connected; }

    // messages for the on-screen display (connected, lost, ...)
    bool PopMessage(std::string& msg);

    // synchronous request on the control channel (thread-safe, blocking).
    // returns false on transport errors; `status` gets the LinkStatus.
    bool Request(uint16_t cmd, const void* payload, uint32_t len,
                 uint16_t& status, std::vector<uint8_t>* out = nullptr);

    // convenience wrappers for later steps (saves / ROMs)
    bool Stat(const std::string& path, uint64_t& size, bool& isDir);
    bool Read(const std::string& path, uint64_t offset, uint32_t length, std::vector<uint8_t>& out);
    bool Write(const std::string& path, uint64_t offset, const void* data, uint32_t length, bool create);
    bool SetStatusText(const std::string& text);

private:
    void worker(std::string host);
    void pushMessage(const std::string& msg);
    void closeSockets();
    void videoWorker();
    void sendTiles(const std::vector<uint16_t>& frame);

    std::atomic<State> state { State::Disconnected };
    std::atomic<bool> stopRequested { false };
    std::thread thread;

    std::mutex hostMutex;
    std::string currentHost;

    std::mutex inputMutex;
    InputSnapshot lastInput;
    uint32_t lastPacketId = 0;
    int64_t lastPacketTime = 0;

    std::mutex msgMutex;
    std::deque<std::string> messages;

    // video: latest frame waiting to be sent + what the 3DS should have
    std::mutex videoMutex;
    std::condition_variable videoCv;
    std::vector<uint16_t> pendingFrame;     // 256x192 RGB565
    bool framePending = false;
    std::atomic<bool> videoResync { true };  // resend everything (new connection)
    std::thread videoThread;
    // per tile: which picture content the 3DS shows (exact source pixels) and
    // in which quality (0 exact, 1 half, 2 quarter, 255 unknown -> resend)
    std::vector<uint16_t> shownSource;
    std::vector<uint8_t> tileLevel;
    std::vector<uint16_t> lastFrame;        // for refining while nothing new comes in
    struct SentPacket { uint32_t seq; int64_t sentMs; std::vector<uint16_t> tiles; };
    std::deque<SentPacket> inFlight;        // sent, not yet confirmed by the 3DS
    uint32_t videoFrameId = 0;
    uint32_t videoSeq = 0;
    uint32_t refreshCursor = 0;
    uint32_t scanStart = 0;
    // latest receipt from the 3DS (guarded by videoMutex)
    // adaptive data rate (AIMD on the packet loss reported by the 3DS)
    double budgetBytes = 24 * 1024;
    uint32_t statReceived = 0, statLost = 0;
    bool ackNew = false;
    bool ackValid = false;
    uint32_t ackSeq = 0;
    uint64_t ackMask = 0;
    void processAcks();
    uint8_t videoAddr[16] = {};             // sockaddr_in of the 3DS video port
    std::atomic<bool> haveVideoAddr { false };

    std::mutex reqMutex;   // serialises Request()
    intptr_t tcpSock = -1;
    intptr_t udpSock = -1;
    uint32_t seq = 1;
};

Client& Get();

}

#endif
