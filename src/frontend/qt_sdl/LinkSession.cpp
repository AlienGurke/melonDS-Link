/*
    melonDS Link - games started from the 3DS, see LinkSession.h
*/

#include "LinkSession.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>

extern QString emuDirectory;

namespace Link
{

namespace
{

std::string fileNameOf(const std::string& path)
{
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string stripExt(const std::string& name)
{
    size_t p = name.rfind('.');
    return p == std::string::npos ? name : name.substr(0, p);
}

QString q(const std::string& s) { return QString::fromStdString(s); }

bool writeWholeFile(const QString& path, const uint8_t* data, size_t len)
{
    // write next to the target first, then swap, so a crash never leaves half a file
    QString tmp = path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    bool ok = f.write((const char*)data, (qint64)len) == (qint64)len;
    f.close();
    if (!ok) { QFile::remove(tmp); return false; }
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

std::string human(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    return buf;
}

}

Session& Session::Get()
{
    static Session s;
    return s;
}

Session::Session()
{
    Link::Get();   // make sure the client outlives us (static destruction order)
    upThread = std::thread(&Session::uploader, this);
}

Session::~Session()
{
    cancel = true;
    if (prepThread.joinable()) prepThread.join();

    // give a save that is still on its way a chance to reach the 3DS
    for (int i = 0; i < 100; i++)
    {
        {
            std::lock_guard<std::mutex> lock(upMutex);
            if (!upPending && !uploading) break;
        }
        if (Link::Get().GetState() != State::Connected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop = true;
    upCv.notify_all();
    if (upThread.joinable()) upThread.join();
}

std::string Session::BaseDir()
{
    return (emuDirectory + "/melonDS-Link").toStdString();
}

std::string Session::RomCacheDir()
{
    return BaseDir() + "/ROMs";
}

std::string Session::BackupDir(const std::string& gameName)
{
    return BaseDir() + "/Backups/" + gameName;
}

void Session::Log(const std::string& line)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    QDir().mkpath(q(BaseDir()));
    QFile f(q(BaseDir() + "/link-log.txt"));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QString l = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "  " + q(line) + "\n";
    f.write(l.toUtf8());
}

void Session::WriteBackup(const std::string& gameName, const uint8_t* data, uint32_t len, int keep)
{
    QString dir = q(BackupDir(gameName));
    QDir().mkpath(dir);
    QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
    writeWholeFile(dir + "/" + q(gameName) + " " + stamp + ".sav", data, len);

    // newest first (the timestamp sorts), delete everything beyond `keep`
    QStringList files = QDir(dir).entryList({"*.sav"}, QDir::Files, QDir::Name | QDir::Reversed);
    for (int i = keep; i < files.size(); i++)
        QFile::remove(dir + "/" + files[i]);
}

// ---------------------------------------------------------------------------
// launching

std::string Session::Tick(QWidget* parent, const std::string& saveDir)
{
    // a finished preparation: hand the ROM to the UI
    if (prepDone.exchange(false))
    {
        if (prepThread.joinable()) prepThread.join();
        preparing = false;
        if (dialog) { dialog->close(); dialog->deleteLater(); dialog = nullptr; }

        std::lock_guard<std::mutex> lock(prepMutex);
        if (!prepError.empty())
        {
            Link::Get().PostMessage("3DS Link: " + prepError);
            Link::Get().SetStatusText(prepError);
            prepError.clear();
            return "";
        }
        std::string rom = readyRom;
        readyRom.clear();
        return rom;
    }

    // keep the progress dialog up to date
    if (preparing && dialog)
    {
        uint64_t total = progressTotal, cur = progressCur;
        std::string label;
        {
            std::lock_guard<std::mutex> lock(prepMutex);
            label = prepLabel;
        }
        dialog->setLabelText(q(label));
        if (total > 0)
        {
            dialog->setMaximum(1000);
            dialog->setValue((int)(cur * 1000 / total));
        }
        if (dialog->wasCanceled()) cancel = true;
        return "";
    }

    // a new game picked on the 3DS?
    LaunchRequest req;
    if (!preparing && Link::Get().TakeLaunchRequest(req))
    {
        preparing = true;
        prepDone = false;
        cancel = false;
        progressCur = 0;
        progressTotal = 0;
        {
            std::lock_guard<std::mutex> lock(prepMutex);
            prepLabel = "Bereite " + stripExt(fileNameOf(req.romPath)) + " vor...";
        }
        dialog = new QProgressDialog(q(prepLabel), "Abbrechen", 0, 0, parent);
        dialog->setWindowTitle("melonDS Link");
        dialog->setMinimumDuration(300);
        dialog->setAutoClose(false);
        dialog->setAutoReset(false);
        if (prepThread.joinable()) prepThread.join();
        prepThread = std::thread(&Session::prepare, this, req, saveDir);
    }
    return "";
}

void Session::prepare(LaunchRequest req, std::string saveDir)
{
    Client& c = Link::Get();
    const std::string romName = fileNameOf(req.romPath);
    const std::string game = stripExt(romName);
    launchCount++;   // a preload that is running stops and leaves the WiFi to this game
    auto fail = [&](const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(prepMutex);
        prepError = msg;
        prepDone = true;
    };

    // 1) ROM: use the copy on the PC if it is complete, otherwise copy it once
    std::string localRom = RomCacheDir() + "/" + romName;
    uint64_t size = req.romSize;
    if (size == 0)
    {
        bool isDir = false;
        if (!c.Stat(req.romPath, size, isDir) || size == 0)
            return fail("ROM auf dem 3DS nicht gefunden: " + req.romPath);
    }
    if (!IsOnPC(romName, size))
    {
        progressTotal = size;
        auto progress = [&](uint64_t off, uint64_t total, uint64_t wire, double secs) -> bool
        {
            if (cancel) return false;
            progressCur = off;
            uint64_t start = progressStart;
            double rate = secs > 0 ? (off - start) / secs : 0;
            int eta = rate > 0 ? (int)((total - off) / rate) : 0;
            char label[256];
            snprintf(label, sizeof(label), "ROM wird vom 3DS kopiert (nur beim ersten Mal):\n%s\n%s von %s  -  %s/s  -  noch ca. %d:%02d\nWLAN: %s/s, Kompression spart %d%%",
                     game.c_str(), human(off).c_str(), human(total).c_str(), human((uint64_t)rate).c_str(), eta / 60, eta % 60,
                     human((uint64_t)(secs > 0 ? wire / secs : 0)).c_str(),
                     off > start ? (int)(100 - wire * 100 / (off - start)) : 0);
            {
                std::lock_guard<std::mutex> lock(prepMutex);
                prepLabel = label;
            }
            return true;
        };
        progressStart = PartialSize(romName);
        std::string err = CopyRom(req.romPath, size, progress);
        if (cancel) return fail("Abgebrochen (bisher kopierter Teil bleibt erhalten)");
        if (!err.empty()) return fail(err);
    }

    // 2) save: fetch the current one from the 3DS into the place melonDS loads it from
    std::string dir = saveDir.empty() ? RomCacheDir() : saveDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    std::string localSave = dir + "/" + game + ".sav";
    {
        std::lock_guard<std::mutex> lock(prepMutex);
        prepLabel = "Hole Spielstand vom 3DS...";
    }
    uint64_t saveSize = 0;
    bool isDir = false;
    if (c.Stat(req.savePath, saveSize, isDir) && !isDir && saveSize > 0)
    {
        std::vector<uint8_t> data, chunk;
        for (uint64_t off = 0; off < saveSize; )
        {
            uint32_t want = (uint32_t)std::min<uint64_t>(LINK_MAX_PAYLOAD, saveSize - off);
            if (!c.Read(req.savePath, off, want, chunk) || chunk.size() != want)
                return fail("Spielstand konnte nicht vom 3DS gelesen werden");
            data.insert(data.end(), chunk.begin(), chunk.end());
            off += want;
        }
        // keep what was on the PC before, just in case
        QFile old(q(localSave));
        if (old.exists() && old.open(QIODevice::ReadOnly))
        {
            QByteArray prev = old.readAll();
            old.close();
            if (prev.size() != (qsizetype)data.size() || memcmp(prev.data(), data.data(), data.size()) != 0)
                WriteBackup(game, (const uint8_t*)prev.data(), (uint32_t)prev.size());
        }
        QDir().mkpath(q(dir));
        if (!writeWholeFile(q(localSave), data.data(), data.size()))
            return fail("Kann Spielstand nicht speichern: " + localSave);
        WriteBackup(game, data.data(), (uint32_t)data.size());
    }
    // no save on the 3DS yet: the game creates one, which then goes to the 3DS

    {
        std::lock_guard<std::mutex> lock(activeMutex);
        pendingRemoteSave = req.savePath;
        pendingGameName = game;
        pendingLocalSave = localSave;
    }
    c.SetStatusText("Spiel laeuft in melonDS");
    std::lock_guard<std::mutex> lock(prepMutex);
    readyRom = localRom;
    prepDone = true;
}

// ---------------------------------------------------------------------------
// ROMs

bool Session::IsOnPC(const std::string& name, uint64_t size)
{
    QFileInfo info(q(RomCacheDir() + "/" + name));
    return size > 0 && info.exists() && (uint64_t)info.size() == size;
}

uint64_t Session::PartialSize(const std::string& name)
{
    QFileInfo info(q(RomCacheDir() + "/" + name + ".part"));
    return info.exists() ? (uint64_t)info.size() : 0;
}

bool Session::FetchGameList(std::string& romDir, std::vector<RemoteRom>& roms)
{
    roms.clear();
    std::vector<uint8_t> out;
    uint16_t status = 0;
    if (!Link::Get().Request(LINK_CMD_GAMELIST, nullptr, 0, status, &out) || status != LINK_OK || out.size() < 2)
        return false;
    uint16_t dirLen;
    memcpy(&dirLen, out.data(), 2);
    if (2u + dirLen > out.size()) return false;
    romDir.assign((const char*)out.data() + 2, dirLen);
    size_t p = 2 + dirLen;
    while (p + sizeof(LinkDirEntry) <= out.size())
    {
        LinkDirEntry de;
        memcpy(&de, out.data() + p, sizeof(de));
        p += sizeof(de);
        if (p + de.nameLen > out.size()) break;
        RemoteRom r;
        r.name.assign((const char*)out.data() + p, de.nameLen);
        r.size = de.size;
        p += de.nameLen;
        roms.push_back(r);
    }
    std::sort(roms.begin(), roms.end(), [](const RemoteRom& a, const RemoteRom& b)
    {
        return QString::compare(q(a.name), q(b.name), Qt::CaseInsensitive) < 0;
    });
    return true;
}

std::string Session::CopyRom(const std::string& remotePath, uint64_t size, const CopyProgress& progress,
                             const std::function<bool()>& keepGoing)
{
    std::unique_lock<std::mutex> copyLock(copyMutex);
    if (keepGoing && !keepGoing()) return "Abgebrochen";

    Client& c = Link::Get();
    const std::string romName = fileNameOf(remotePath);
    const std::string localRom = RomCacheDir() + "/" + romName;
    if (IsOnPC(romName, size)) return "";

    QDir().mkpath(q(RomCacheDir()));
    QString part = q(localRom) + ".part";
    uint64_t off = PartialSize(romName);
    if (off > size) { QFile::remove(part); off = 0; }   // not from this ROM
    QFile out(part);
    if (!out.open(off > 0 ? QIODevice::Append : (QIODevice::WriteOnly | QIODevice::Truncate)))
        return "Kann ROM nicht speichern: " + localRom;

    const uint64_t startOff = off;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t wireDone = 0;   // bytes over the WiFi in earlier (interrupted) streams
    bool stopped = false;
    auto secsNow = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    auto sink = [&](const uint8_t* data, size_t n) -> bool
    {
        if (out.write((const char*)data, (qint64)n) != (qint64)n) return false;
        off += n;
        if (progress && !progress(off, size, wireDone + c.LastStreamWireBytes(), secsNow()))
        {
            stopped = true;
            return false;
        }
        return true;
    };

    // one continuous stream; after a hiccup continue where it stopped
    int failures = 0;
    auto lastStatus = t0 - std::chrono::seconds(10);
    if (progress && !progress(off, size, 0, 0)) stopped = true;
    while (off < size && !stopped)
    {
        if (c.GetState() != State::Connected)
        {
            if (++failures > 30) { out.close(); return "Verbindung verloren beim Uebertragen der ROM (Fortsetzen spaeter moeglich)"; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (progress && !progress(off, size, wireDone, secsNow())) stopped = true;
            continue;
        }
        if (std::chrono::steady_clock::now() - lastStatus > std::chrono::seconds(5))
        {
            char st[96];
            snprintf(st, sizeof(st), "ROM wird kopiert: %d%%", (int)(off * 100 / size));
            c.SetStatusText(st);
            lastStatus = std::chrono::steady_clock::now();
        }
        // in pieces of 16 MB: in between melonDS can hear the 3DS (a game
        // picked there), costs only a round trip per piece
        const uint64_t piece = 16ull * 1024 * 1024;
        bool ok = false;
        c.ReadStream(remotePath, off, std::min<uint64_t>(piece, size - off), sink, ok, true);
        wireDone += c.LastStreamWireBytes();
        out.flush();
        if (ok && off < size)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (keepGoing && !keepGoing()) stopped = true;
            continue;
        }
        if (!ok && off < size && !stopped)
        {
            if (++failures > 30) { out.close(); return "Verbindung verloren beim Uebertragen der ROM (Fortsetzen spaeter moeglich)"; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    out.close();
    if (stopped) return "Abgebrochen";

    {
        double secs = secsNow();
        int isecs = (int)secs;
        uint64_t moved = size - startOff;
        char summary[220];
        snprintf(summary, sizeof(summary), "3DS Link: ROM kopiert in %d:%02d - Daten %s/s, WLAN %s/s, Kompression sparte %d%%",
                 isecs / 60, isecs % 60, human((uint64_t)(moved / (secs > 0 ? secs : 1))).c_str(),
                 human((uint64_t)(wireDone / (secs > 0 ? secs : 1))).c_str(),
                 moved > 0 ? (int)(100 - wireDone * 100 / moved) : 0);
        c.PostMessage(summary);
        Log(std::string(summary + 11) + " (" + romName + ", " + human(size) +
            (startOff ? ", fortgesetzt ab " + human(startOff) : std::string()) + ")");
        std::string st = c.GetStats();
        if (!st.empty()) Log("  3DS: " + st);
    }
    QFile::remove(q(localRom));
    if (!QFile::rename(part, q(localRom)))
        return "Kann ROM nicht speichern: " + localRom;
    RequestOnPcSync();
    return "";
}

void Session::syncOnPc()
{
    std::string dir;
    std::vector<RemoteRom> roms;
    if (Link::Get().GetState() != State::Connected || !FetchGameList(dir, roms)) return;
    std::string names;
    for (const RemoteRom& r : roms)
        if (IsOnPC(r.name, r.size)) names += r.name + "\n";
    uint16_t status = 0;
    Link::Get().Request(LINK_CMD_SETONPC, names.data(), (uint32_t)names.size(), status);
}

void Session::SetActiveSave(const std::string& localPath)
{
    std::lock_guard<std::mutex> lock(activeMutex);
    activeLocalSave = localPath;
    activeRemoteSave = pendingRemoteSave;
    activeGameName = pendingGameName;
    if (!localPath.empty() && localPath != pendingLocalSave)
        Link::Get().PostMessage("3DS Link: Achtung, melonDS speichert an einem anderen Ort als erwartet - Spielstand-Sync kann fehlen");
}

// ---------------------------------------------------------------------------
// saves back to the 3DS

void Session::OnSaveWritten(const std::string& localPath, const uint8_t* data, uint32_t len)
{
    std::string remote, game;
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        if (activeLocalSave.empty() || localPath != activeLocalSave) return;
        remote = activeRemoteSave;
        game = activeGameName;
    }
    {
        std::lock_guard<std::mutex> lock(upMutex);
        upRemote = remote;
        upGame = game;
        upData.assign(data, data + len);
        upPending = true;
    }
    upCv.notify_one();
}

bool Session::uploadNow(const std::string& remote, const std::vector<uint8_t>& data)
{
    Client& c = Link::Get();
    if (c.GetState() != State::Connected) return false;
    std::string tmp = remote + ".tmp";
    for (size_t off = 0; off < data.size(); )
    {
        uint32_t n = (uint32_t)std::min<size_t>(LINK_MAX_PAYLOAD - 1024, data.size() - off);
        if (!c.Write(tmp, off, data.data() + off, n, true)) return false;
        off += n;
    }
    // a left-over .tmp might have been longer
    uint64_t size = 0;
    bool isDir = false;
    if (!c.Stat(tmp, size, isDir) || size != data.size()) return false;
    return c.Replace(tmp, remote);
}

void Session::uploader()
{
    bool announcedRetry = false;
    while (!stop)
    {
        std::string remote, game;
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(upMutex);
            upCv.wait_for(lock, std::chrono::seconds(2), [this] { return upPending || stop || syncRequested; });
            if (stop) break;
            if (!upPending)
            {
                if (syncRequested.exchange(false))
                {
                    lock.unlock();
                    syncOnPc();
                }
                continue;
            }
            remote = upRemote;
            game = upGame;
            data = upData;
            uploading = true;
            upPending = false;
        }

        // the backup always works, even without a connection
        if (!announcedRetry) WriteBackup(game, data.data(), (uint32_t)data.size());

        bool ok = uploadNow(remote, data);
        if (ok)
        {
            Link::Get().PostMessage("3DS Link: Spielstand auf dem 3DS gesichert");
            Link::Get().SetStatusText("Spielstand gesichert " +
                QDateTime::currentDateTime().toString("HH:mm").toStdString());
            announcedRetry = false;
        }
        else
        {
            if (!announcedRetry)
                Link::Get().PostMessage("3DS Link: Spielstand konnte nicht auf den 3DS - wird nachgeholt (Kopie am PC ist sicher)");
            announcedRetry = true;
            // try again later unless a newer save arrived meanwhile
            std::lock_guard<std::mutex> lock(upMutex);
            if (!upPending)
            {
                upRemote = remote;
                upGame = game;
                upData = data;
                upPending = true;
            }
        }
        uploading = false;
        if (!ok)
        {
            for (int i = 0; i < 20 && !stop; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}
