/*
    melonDS Link - games started from the 3DS.

    Flow: the user picks a game on the 3DS -> melonDS copies the ROM to its
    cache (once), fetches the current save from the 3DS, boots the game.
    Every time the game saves, the save is written back to the 3DS (via a
    temporary file that atomically replaces the old one) and the last five
    versions are kept as backups next to melonDS.
*/

#ifndef MELONDS_LINKSESSION_H
#define MELONDS_LINKSESSION_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LinkClient.h"

class QProgressDialog;
class QWidget;

namespace Link
{

class Session
{
public:
    static Session& Get();
    ~Session();

    // UI thread, called regularly. `saveDir` is melonDS' "SaveFilePath"
    // setting (empty = saves next to the ROM). Returns the local ROM path once
    // a game is ready to boot (empty otherwise).
    std::string Tick(QWidget* parent, const std::string& saveDir);

    // after the UI booted the ROM: which local file melonDS saves into
    void SetActiveSave(const std::string& localPath);

    // called by SaveManager whenever it wrote a save file (any thread)
    void OnSaveWritten(const std::string& localPath, const uint8_t* data, uint32_t len);

    // folders
    static std::string BaseDir();
    static std::string RomCacheDir();
    static std::string BackupDir(const std::string& gameName);

    // append a line to melonDS-Link/link-log.txt (with date and time)
    static void Log(const std::string& line);

    // keep the newest `keep` backups of a game (also used by tests)
    static void WriteBackup(const std::string& gameName, const uint8_t* data, uint32_t len, int keep = 5);

    // --- ROMs on the 3DS / copied to the PC ---
    struct RemoteRom { std::string name; uint64_t size = 0; };
    // the games the 3DS app lists (its ROM folder and the .nds files in it)
    bool FetchGameList(std::string& romDir, std::vector<RemoteRom>& roms);
    // a ROM is "on the PC" when the copy is complete (exact size)
    static bool IsOnPC(const std::string& name, uint64_t size);
    // bytes of an interrupted copy that will be continued (0 = none)
    static uint64_t PartialSize(const std::string& name);

    // copy a ROM from the 3DS (continues an interrupted copy). `progress` is
    // called regularly with bytes done / over the WiFi and seconds so far; it
    // may return false to stop (the part done is kept). Returns "" on success.
    using CopyProgress = std::function<bool(uint64_t done, uint64_t total, uint64_t wire, double secs)>;
    // keepGoing (optional) is asked between pieces of 16 MB: false stops
    // cleanly (no broken connection, unlike stopping from `progress`)
    std::string CopyRom(const std::string& remotePath, uint64_t size, const CopyProgress& progress,
                        const std::function<bool()>& keepGoing = nullptr);

    // DS ROMs: don't read the padding behind the game data from the SD card
    // (only spot checks) - faster, setting "Link.SkipPadding"
    static std::atomic<bool> SkipPadding;

    // counts up whenever a game is picked on the 3DS: a running preload
    // stops then (it can continue later), the game comes first
    uint32_t LaunchCount() const { return launchCount.load(); }

    // tell the 3DS which games are on the PC (marked "PC" in its list);
    // done in the background
    void RequestOnPcSync() { syncRequested = true; upCv.notify_one(); }

private:
    Session();

    void prepare(LaunchRequest req, std::string saveDir);
    void uploader();
    bool uploadNow(const std::string& remote, const std::vector<uint8_t>& data);
    void syncOnPc();

    std::mutex copyMutex;                     // one ROM copy at a time
    std::atomic<uint32_t> launchCount { 0 };
    std::atomic<bool> syncRequested { false };

    // preparing a launch (worker thread -> UI)
    std::thread prepThread;
    std::atomic<bool> preparing { false };
    std::atomic<bool> prepDone { false };
    std::atomic<bool> cancel { false };
    std::atomic<uint64_t> progressCur { 0 }, progressTotal { 0 }, progressStart { 0 };
    std::mutex prepMutex;
    std::string prepLabel, prepError, readyRom;
    QProgressDialog* dialog = nullptr;

    // the game that is running and where its save lives on the 3DS
    std::mutex activeMutex;
    std::string pendingRemoteSave, pendingGameName, pendingLocalSave;
    std::string activeLocalSave, activeRemoteSave, activeGameName;

    // save upload queue (latest wins)
    std::mutex upMutex;
    std::condition_variable upCv;
    bool upPending = false;
    std::string upRemote, upGame;
    std::vector<uint8_t> upData;
    std::atomic<bool> uploading { false };
    std::atomic<bool> stop { false };
    std::thread upThread;
};

}

#endif
