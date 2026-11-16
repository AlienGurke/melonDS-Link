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
    auto fail = [&](const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(prepMutex);
        prepError = msg;
        prepDone = true;
    };

    double lastRate = 0;

    // 1) ROM: use the cached copy if it has the right size, otherwise copy it once
    QDir().mkpath(q(RomCacheDir()));
    std::string localRom = RomCacheDir() + "/" + romName;
    QFileInfo info(q(localRom));
    if (!info.exists() || (uint64_t)info.size() != req.romSize || req.romSize == 0)
    {
        uint64_t size = req.romSize;
        bool isDir = false;
        if (size == 0 && (!c.Stat(req.romPath, size, isDir) || size == 0))
            return fail("ROM auf dem 3DS nicht gefunden: " + req.romPath);

        QString part = q(localRom) + ".part";
        QFile out(part);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail("Kann ROM nicht speichern: " + localRom);

        progressTotal = size;
        auto t0 = std::chrono::steady_clock::now();
        auto lastStatus = t0 - std::chrono::seconds(1);
        uint64_t off = 0;
        uint64_t wireDone = 0;   // bytes over the WiFi in earlier (interrupted) streams
        auto sink = [&](const uint8_t* data, size_t n) -> bool
        {
            if (cancel) return false;
            if (out.write((const char*)data, (qint64)n) != (qint64)n) return false;
            off += n;
            progressCur = off;

            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - t0).count();
            double rate = secs > 0 ? off / secs : 0;
            int eta = rate > 0 ? (int)((size - off) / rate) : 0;
            char label[256];
            uint64_t wireNow = wireDone + c.LastStreamWireBytes();
            double wireRate = secs > 0 ? wireNow / secs : 0;
            snprintf(label, sizeof(label), "ROM wird vom 3DS kopiert (nur beim ersten Mal):\n%s\n%s von %s  -  %s/s  -  noch ca. %d:%02d\nWLAN: %s/s, Kompression spart %d%%",
                     game.c_str(), human(off).c_str(), human(size).c_str(), human((uint64_t)rate).c_str(), eta / 60, eta % 60,
                     human((uint64_t)wireRate).c_str(), off > 0 ? (int)(100 - wireNow * 100 / off) : 0);
            {
                std::lock_guard<std::mutex> lock(prepMutex);
                prepLabel = label;
            }
            lastRate = rate;
            return true;
        };

        // one continuous stream; after a hiccup continue where it stopped
        int failures = 0;
        while (off < size)
        {
            if (cancel) { out.close(); QFile::remove(part); return fail("Abgebrochen"); }
            if (c.GetState() != State::Connected)
            {
                if (++failures > 30) { out.close(); QFile::remove(part); return fail("Verbindung verloren beim Uebertragen der ROM"); }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            // the 3DS status line (sent between streams would block it; tell it once)
            if (std::chrono::steady_clock::now() - lastStatus > std::chrono::seconds(5))
            {
                char st[64];
                snprintf(st, sizeof(st), "ROM wird kopiert: %d%%", (int)(off * 100 / size));
                c.SetStatusText(st);
                lastStatus = std::chrono::steady_clock::now();
            }
            bool ok = false;
            c.ReadStream(req.romPath, off, size - off, sink, ok, true);
            wireDone += c.LastStreamWireBytes();
            if (!ok && off < size && !cancel)
            {
                if (++failures > 30) { out.close(); QFile::remove(part); return fail("Verbindung verloren beim Uebertragen der ROM"); }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        out.close();
        QFile::remove(q(localRom));
        if (!QFile::rename(part, q(localRom)))
            return fail("Kann ROM nicht speichern: " + localRom);
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
            upCv.wait_for(lock, std::chrono::seconds(2), [this] { return upPending || stop; });
            if (stop) break;
            if (!upPending) continue;
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
