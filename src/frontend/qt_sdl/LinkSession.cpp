/*
    melonDS Link - games started from the 3DS, see LinkSession.h
*/

#include "LinkSession.h"
#include "LinkLang.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <QCryptographicHash>
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

std::string hashOf(const uint8_t* data, size_t len)
{
    return QCryptographicHash::hash(QByteArrayView((const char*)data, (qsizetype)len),
                                    QCryptographicHash::Sha1).toHex().toStdString();
}

bool readWholeFile(const QString& path, std::vector<uint8_t>& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray all = f.readAll();
    out.assign(all.begin(), all.end());
    return true;
}

QString mmss(double secs)
{
    int s = (int)secs;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

std::string human(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    return buf;
}

}

std::atomic<bool> Session::SkipPadding { false };
std::function<bool()> Session::EmuActive;

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

std::string Session::DebugDir()
{
    return BaseDir() + "/Debug";
}

std::string Session::ScreenshotDir()
{
    return BaseDir() + "/Screenshots";
}

// which save was on both sides the last time they were the same: tells
// which side changed since then
std::string Session::SyncedHash(const std::string& game)
{
    QFile f(q(BaseDir() + "/Sync/" + game + ".txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return "";
    return QString::fromUtf8(f.readAll()).trimmed().toStdString();
}

void Session::SetSyncedHash(const std::string& game, const std::string& hash)
{
    QString path = q(BaseDir() + "/Sync/" + game + ".txt");
    if (hash.empty()) { QFile::remove(path); return; }
    QDir().mkpath(q(BaseDir() + "/Sync"));
    QByteArray b = QByteArray::fromStdString(hash + "\n");
    writeWholeFile(path, (const uint8_t*)b.data(), b.size());
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
    QDir().mkpath(q(DebugDir()));
    // the log used to be directly in melonDS-Link: move it along once
    static bool moved = false;
    if (!moved)
    {
        moved = true;
        QString old = q(BaseDir() + "/link-log.txt");
        if (QFile::exists(old) && !QFile::exists(q(DebugDir() + "/link-log.txt")))
            QFile::rename(old, q(DebugDir() + "/link-log.txt"));
    }
    QFile f(q(DebugDir() + "/link-log.txt"));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QString l = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "  " + q(line) + "\n";
    f.write(l.toUtf8());
}

void Session::WriteBackup(const std::string& gameName, const uint8_t* data, uint32_t len, int keep)
{
    QString dir = q(BackupDir(gameName));
    QDir().mkpath(dir);
    // two backups in the same millisecond must not overwrite each other
    QString file;
    for (int i = 0; i < 50; i++)
    {
        QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
        file = dir + "/" + q(gameName) + " " + stamp + ".sav";
        if (!QFile::exists(file)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writeWholeFile(file, data, len);

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
            // the 3DS waits for the game it picked: tell it that nothing came of it
            std::string running;
            {
                std::lock_guard<std::mutex> ul(upMutex);
                running = sessionName;
            }
            SendSession(running, true);
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
            prepLabel = Tr("prep.preparing").arg(q(stripExt(fileNameOf(req.romPath)))).toStdString();
        }
        lastSaveDir = saveDir;
        dialog = new QProgressDialog(q(prepLabel), Tr("prep.cancel"), 0, 0, parent);
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
            return fail(Tr("prep.romnotfound").arg(q(req.romPath)).toStdString());
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
            QString label = Tr("prep.copying")
                .arg(q(game), q(human(off)), q(human(total)), q(human((uint64_t)rate)), mmss(eta),
                     q(human((uint64_t)(secs > 0 ? wire / secs : 0))))
                .arg(off > start ? (int)(100 - wire * 100 / (off - start)) : 0);
            {
                std::lock_guard<std::mutex> lock(prepMutex);
                prepLabel = label.toStdString();
            }
            return true;
        };
        progressStart = PartialSize(romName);
        std::string err = CopyRom(req.romPath, size, progress);
        if (cancel) return fail(TrS("prep.cancelled"));
        if (!err.empty()) return fail(err);
    }

    // 2) save: bring the PC copy (where melonDS loads it from) in line with the 3DS
    std::string dir = saveDir.empty() ? RomCacheDir() : saveDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    std::string localSave = dir + "/" + game + ".sav";
    std::string err = syncSave(req.savePath, localSave, game);
    if (!err.empty()) return fail(err);

    {
        std::lock_guard<std::mutex> lock(activeMutex);
        pendingRemoteSave = req.savePath;
        pendingGameName = game;
        pendingLocalSave = localSave;
    }
    c.SetStatusText(c.Tr3DS("prep.running"));
    std::lock_guard<std::mutex> lock(prepMutex);
    readyRom = localRom;
    prepDone = true;
}

bool Session::readRemote(const std::string& path, bool& exists, std::vector<uint8_t>& data)
{
    Client& c = Link::Get();
    exists = false;
    data.clear();
    uint64_t size = 0;
    bool isDir = false;
    if (!c.Stat(path, size, isDir)) return c.GetState() == State::Connected;   // not there
    if (isDir) return true;
    exists = true;
    std::vector<uint8_t> chunk;
    for (uint64_t off = 0; off < size; )
    {
        uint32_t want = (uint32_t)std::min<uint64_t>(LINK_MAX_PAYLOAD, size - off);
        if (!c.Read(path, off, want, chunk) || chunk.size() != want) return false;
        data.insert(data.end(), chunk.begin(), chunk.end());
        off += want;
    }
    return true;
}

int Session::askOn3ds(const std::string& text)
{
    Client& c = Link::Get();
    static std::atomic<uint32_t> nextId { (uint32_t)QDateTime::currentMSecsSinceEpoch() };
    const uint32_t id = nextId++;
    uint32_t conn = c.ConnectionCount();
    if (!c.Ask(id, text)) return -1;
    auto t0 = std::chrono::steady_clock::now();
    while (true)
    {
        int choice;
        if (c.TakeAnswer(id, choice)) return choice;
        if (cancel) { c.CancelAsk(id); return -2; }
        // after a reconnect the 3DS app may have forgotten the question
        if (c.GetState() == State::Connected && c.ConnectionCount() != conn)
        {
            conn = c.ConnectionCount();
            c.Ask(id, text);
        }
        // nobody answers: keep the 3DS save (the PC one is kept as a backup)
        if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(10))
        {
            c.CancelAsk(id);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string Session::syncSave(const std::string& remote, const std::string& localSave, const std::string& game)
{
    Client& c = Link::Get();
    {
        std::lock_guard<std::mutex> lock(prepMutex);
        prepLabel = TrS("prep.fetchsave");
    }
    bool haveRemote = false;
    std::vector<uint8_t> remoteData;
    if (!readRemote(remote, haveRemote, remoteData)) return TrS("prep.savereadfail");

    std::vector<uint8_t> localData;
    bool haveLocal = readWholeFile(q(localSave), localData);
    std::string hR = haveRemote ? hashOf(remoteData.data(), remoteData.size()) : "";
    std::string hL = haveLocal ? hashOf(localData.data(), localData.size()) : "";
    std::string last = SyncedHash(game);

    bool usePc = false;
    if (haveLocal && hL != hR && hL != last)
    {
        // the PC save changed since the last sync (played without the 3DS,
        // or no record yet): the user decides, on the 3DS
        const std::string lang = c.RemoteLanguage();
        auto T3 = [&](const char* key) { return TrIn(lang, key); };
        QString title;
        if (!haveRemote) title = T3("ask.no3ds");
        else if (last.empty()) title = T3("ask.differ");
        else if (hR == last) title = T3("ask.newer");
        else title = T3("ask.both");
        QString when = QFileInfo(q(localSave)).lastModified().toString("dd.MM.yyyy HH:mm");
        QString text = q(game) + "\n\n" + title + "\n" + T3("ask.pcdate").arg(when) + "\n\n" + T3("ask.note") + "\n" +
                       T3("ask.usepc") + "\n" + (haveRemote ? T3("ask.use3ds") : T3("ask.nosave"));
        {
            std::lock_guard<std::mutex> lock(prepMutex);
            prepLabel = TrS("prep.asking");
        }
        int choice = askOn3ds(text.toStdString());
        if (choice == -2) return TrS("copy.cancelled");
        if (choice < 0) return TrS("prep.savereadfail");
        usePc = choice == 0;
        Log(std::string("Spielstand-Frage (") + game + "): " + (usePc ? "PC" : "3DS"));
    }

    if (usePc)
    {
        {
            std::lock_guard<std::mutex> lock(prepMutex);
            prepLabel = TrS("prep.uploadpc");
        }
        if (haveRemote) WriteBackup(game, remoteData.data(), (uint32_t)remoteData.size());
        WriteBackup(game, localData.data(), (uint32_t)localData.size());
        if (!uploadNow(remote, localData)) return TrS("prep.uploadfail");
        SetSyncedHash(game, hL);
        return "";
    }

    // the 3DS save counts
    if (haveLocal && hL != hR) WriteBackup(game, localData.data(), (uint32_t)localData.size());
    if (haveRemote)
    {
        QDir().mkpath(QFileInfo(q(localSave)).absolutePath());
        if (hL != hR && !writeWholeFile(q(localSave), remoteData.data(), remoteData.size()))
            return Tr("prep.savewritefail").arg(q(localSave)).toStdString();
        if (hL != hR) WriteBackup(game, remoteData.data(), (uint32_t)remoteData.size());
        SetSyncedHash(game, hR);
    }
    else
    {
        // no save on the 3DS: the game starts fresh (the old PC one is in the backups)
        if (haveLocal) QFile::remove(q(localSave));
        SetSyncedHash(game, "");
    }
    return "";
}

std::string Session::RemoteSavePath(const std::string& romDir, const std::string& romName)
{
    std::string base = stripExt(romName);
    std::string save = romDir + "/saves/" + base + ".sav";
    std::string alt = romDir + "/" + base + ".sav";
    uint64_t size = 0;
    bool isDir = false;
    if (!Link::Get().Stat(save, size, isDir) && Link::Get().Stat(alt, size, isDir)) return alt;
    return save;
}

std::vector<Session::Backup> Session::Backups(const std::string& game)
{
    std::vector<Backup> out;
    QDir dir(q(BackupDir(game)));
    for (const QFileInfo& fi : dir.entryInfoList({"*.sav"}, QDir::Files, QDir::Name | QDir::Reversed))
    {
        Backup b;
        b.path = fi.absoluteFilePath().toStdString();
        b.size = (uint64_t)fi.size();
        // "<game> yyyy-MM-dd_HH-mm-ss-zzz.sav"
        QString stamp = fi.completeBaseName().right(23);
        QDateTime t = QDateTime::fromString(stamp, "yyyy-MM-dd_HH-mm-ss-zzz");
        b.when = (t.isValid() ? t : fi.lastModified()).toString("dd.MM.yyyy HH:mm:ss").toStdString();
        out.push_back(b);
    }
    return out;
}

void Session::SendBackupList(const std::string& romPath)
{
    std::thread([this, romPath]
    {
        auto list = Backups(stripExt(fileNameOf(romPath)));
        uint32_t token;
        {
            std::lock_guard<std::mutex> lock(bakMutex);
            token = ++bakToken;
            bakPaths.clear();
            for (const auto& b : list) bakPaths.push_back(b.path);
        }
        std::string text = romPath;
        for (const auto& b : list) text += "\n" + b.when + "  (" + human(b.size) + ")";
        std::vector<uint8_t> payload(4 + text.size());
        memcpy(payload.data(), &token, 4);
        memcpy(payload.data() + 4, text.data(), text.size());
        uint16_t status = 0;
        Link::Get().Request(LINK_CMD_BACKUPLIST, payload.data(), (uint32_t)payload.size(), status);
    }).detach();
}

void Session::RestoreFrom3ds(const std::string& romPath, int index, uint32_t token)
{
    std::thread([this, romPath, index, token]
    {
        Client& c = Link::Get();
        std::string path;
        {
            std::lock_guard<std::mutex> lock(bakMutex);
            if (token == bakToken && index >= 0 && index < (int)bakPaths.size()) path = bakPaths[index];
        }
        std::string err;
        if (path.empty()) err = TrS("bak.gone");
        else
        {
            // the game may just have been ended for this: give it a moment
            for (int i = 0; i < 50 && EmuActive && EmuActive(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            size_t slash = romPath.find_last_of('/');
            std::string dir = slash == std::string::npos ? "" : romPath.substr(0, slash);
            err = RestoreBackup(dir, fileNameOf(romPath), path);
        }
        if (err.empty()) c.SetStatusText(c.Tr3DS("bak.done3ds"));
        else c.SetStatusText(c.Tr3DS("bak.failed3ds") + ": " + err);
        c.PostMessage(err.empty() ? TrS("bak.done") : Tr("bak.failed").arg(q(err)).toStdString());
    }).detach();
}

std::string Session::RestoreBackup(const std::string& romDir, const std::string& romName, const std::string& backupPath)
{
    const std::string game = stripExt(romName);
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        if (!activeGameName.empty() && activeGameName == game && (!EmuActive || EmuActive()))
            return TrS("bak.running");
    }
    if (preparing) return TrS("bak.running");
    if (Link::Get().GetState() != State::Connected) return TrS("connect.first");
    // a save still on its way (e.g. written when the game was ended) goes first,
    // so it cannot land on top of the backup afterwards
    for (int i = 0; i < 300; i++)
    {
        {
            std::lock_guard<std::mutex> lock(upMutex);
            if (!upPending && !uploading) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::vector<uint8_t> data;
    if (!readWholeFile(q(backupPath), data) || data.empty()) return backupPath;

    std::string remote = RemoteSavePath(romDir, romName);
    bool haveRemote = false;
    std::vector<uint8_t> current;
    if (!readRemote(remote, haveRemote, current)) return TrS("prep.savereadfail");
    // keep what is on the 3DS now (a new backup, so it is the newest one)
    if (haveRemote) WriteBackup(game, current.data(), (uint32_t)current.size(), 6);
    if (!uploadNow(remote, data)) return TrS("prep.uploadfail");

    // the PC copy follows, so the next start sees both sides equal
    std::string dir = lastSaveDir.empty() ? RomCacheDir() : lastSaveDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    QString localSave = q(dir + "/" + game + ".sav");
    if (QFile::exists(localSave) || QFile::exists(q(RomCacheDir() + "/" + romName)))
    {
        QDir().mkpath(q(dir));
        writeWholeFile(localSave, data.data(), data.size());
    }
    SetSyncedHash(game, hashOf(data.data(), data.size()));
    Log("Backup auf den 3DS übertragen: " + backupPath);
    return "";
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

bool Session::DeleteFromPC(const std::string& name, bool backups)
{
    const std::string game = stripExt(name);
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        if (!activeGameName.empty() && activeGameName == game) return false;
    }
    std::unique_lock<std::mutex> copyLock(copyMutex, std::try_to_lock);
    if (!copyLock.owns_lock()) return false;   // being copied right now

    const QString dir = q(RomCacheDir()) + "/";
    QFile::remove(dir + q(name));
    QFile::remove(dir + q(name) + ".part");
    QFile::remove(dir + q(game) + ".sav");     // the PC copy; fetched again from the 3DS at the next start
    if (backups) QDir(q(BackupDir(game))).removeRecursively();
    if (backups) SetSyncedHash(game, "");
    Log("Vom PC gelöscht: " + name + (backups ? " (mit Backups)" : ""));
    RequestOnPcSync();
    return true;
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
    if (keepGoing && !keepGoing()) return TrS("copy.cancelled");

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
        return Tr("copy.cantstore").arg(q(localRom)).toStdString();

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
            if (++failures > 30) { out.close(); return TrS("copy.lost"); }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (progress && !progress(off, size, wireDone, secsNow())) stopped = true;
            continue;
        }
        if (std::chrono::steady_clock::now() - lastStatus > std::chrono::seconds(5))
        {
            c.SetStatusText(QString::fromStdString(c.Tr3DS("copy.status")).arg((int)(off * 100 / size)).toStdString());
            lastStatus = std::chrono::steady_clock::now();
        }
        // in pieces of 16 MB: in between melonDS can hear the 3DS (a game
        // picked there), costs only a round trip per piece
        const uint64_t piece = 16ull * 1024 * 1024;
        bool ok = false;
        c.ReadStream(remotePath, off, std::min<uint64_t>(piece, size - off), sink, ok, true, SkipPadding.load());
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
            if (++failures > 30) { out.close(); return TrS("copy.lost"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    out.close();
    if (stopped) return TrS("copy.cancelled");

    {
        double secs = secsNow();
        uint64_t moved = size - startOff;
        std::string summary = Tr("copy.summary")
            .arg(mmss(secs), q(human((uint64_t)(moved / (secs > 0 ? secs : 1)))),
                 q(human((uint64_t)(wireDone / (secs > 0 ? secs : 1)))))
            .arg(moved > 0 ? (int)(100 - wireDone * 100 / moved) : 0).toStdString();
        c.PostMessage(summary);
        Log(summary + " (" + romName + ", " + human(size) +
            (startOff ? ", fortgesetzt ab " + human(startOff) : std::string()) +
            (SkipPadding ? ", Füllbereich überspringen an" : "") + ")");
        std::string st = c.GetStats();
        if (!st.empty()) Log("  3DS: " + st);
    }
    QFile::remove(q(localRom));
    if (!QFile::rename(part, q(localRom)))
        return Tr("copy.cantstore").arg(q(localRom)).toStdString();
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

void Session::SendSession(const std::string& running, bool launchDone)
{
    {
        std::lock_guard<std::mutex> lock(upMutex);
        sessionName = running;
        sessionPending = true;
        sessionDone = sessionDone || launchDone;
    }
    upCv.notify_one();
}

void Session::sendSessionNow()
{
    std::string name;
    bool done;
    {
        std::lock_guard<std::mutex> lock(upMutex);
        if (!sessionPending) return;
        name = sessionName;
        done = sessionDone;
        sessionPending = false;
        sessionDone = false;
    }
    std::vector<uint8_t> payload(1 + name.size());
    payload[0] = done ? LINK_SESSION_LAUNCH_DONE : 0;
    memcpy(payload.data() + 1, name.data(), name.size());
    uint16_t status = 0;
    if (Link::Get().GetState() != State::Connected ||
        !Link::Get().Request(LINK_CMD_SESSION, payload.data(), (uint32_t)payload.size(), status))
    {
        // try again with the next connection (the UI sends it on reconnect anyway)
    }
}

void Session::SetActiveSave(const std::string& localPath)
{
    std::lock_guard<std::mutex> lock(activeMutex);
    activeLocalSave = localPath;
    activeRemoteSave = pendingRemoteSave;
    activeGameName = pendingGameName;
    if (!localPath.empty() && localPath != pendingLocalSave)
        Link::Get().PostMessage(TrS("save.otherplace"));
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
    // the .tmp stays on the 3DS between uploads (it is the previous save after
    // each replace) and is only overwritten - much faster on the SD card
    for (size_t off = 0; off < data.size(); )
    {
        uint32_t n = (uint32_t)std::min<size_t>(LINK_MAX_PAYLOAD - 1024, data.size() - off);
        if (!c.Write(tmp, off, data.data() + off, n, true)) return false;
        off += n;
    }
    {
        // cut it to the exact size (it may have been longer)
        std::vector<uint8_t> req(sizeof(LinkSetSizeReq) + tmp.size());
        LinkSetSizeReq r = {};
        r.size = data.size();
        memcpy(req.data(), &r, sizeof(r));
        memcpy(req.data() + sizeof(r), tmp.data(), tmp.size());
        uint16_t status = 0;
        if (!c.Request(LINK_CMD_SETSIZE, req.data(), (uint32_t)req.size(), status) || status != LINK_OK) return false;
    }
    // check what arrived
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
            upCv.wait_for(lock, std::chrono::seconds(2), [this] { return upPending || stop || syncRequested || sessionPending; });
            if (stop) break;
            if (sessionPending)
            {
                // small and the 3DS waits for it: before anything else
                lock.unlock();
                sendSessionNow();
                continue;
            }
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

        if (!announcedRetry)
        {
            Link::Get().PostMessage(TrS("save.uploading"));
            Link::Get().SetStatusText(Link::Get().Tr3DS("save.uploading3ds"));
        }
        auto u0 = std::chrono::steady_clock::now();
        bool ok = uploadNow(remote, data);
        {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - u0).count();
            char line[160];
            snprintf(line, sizeof(line), "Spielstand %s: %s in %.1f s (%s)", ok ? "auf den 3DS geschrieben" : "Upload fehlgeschlagen",
                     human(data.size()).c_str(), secs, game.c_str());
            Log(line);
            if (ok)
            {
                // where the time went on the 3DS (receiving vs. writing to the SD card)
                std::string st = Link::Get().GetStats();
                size_t p = st.find("Upload:");
                if (p != std::string::npos) Log("  3DS " + st.substr(p));
            }
        }
        if (ok)
        {
            SetSyncedHash(game, hashOf(data.data(), data.size()));
            Link::Get().PostMessage(TrS("save.done"));
            Link::Get().SetStatusText(q(Link::Get().Tr3DS("save.done3ds")).arg(QDateTime::currentDateTime().toString("HH:mm")).toStdString());
            announcedRetry = false;
        }
        else
        {
            if (!announcedRetry)
                Link::Get().PostMessage(TrS("save.retry"));
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
