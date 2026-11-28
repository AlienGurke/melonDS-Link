// melonDS Link: play time on this PC - see LinkActivity.h

#include <atomic>
#include <chrono>
#include <cstring>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include "NDSCart.h"
#include "NDS_Header.h"
#include "LinkActivity.h"
#include "LinkClient.h"

namespace Link::Activity
{

namespace
{

using melonDS::u8;
using melonDS::u16;
using melonDS::u32;
using melonDS::u64;

constexpr char kMagic[8] = {'3', 'D', 'S', 'L', 'P', 'C', 'P', 'L'};
constexpr u32 kVersion = 1;
constexpr u32 kEntrySize = 24;
constexpr u16 kFlagWith3ds = 1;
constexpr u16 kFlagClosed = 2;

using Clock = std::chrono::steady_clock;

std::atomic<bool> enabled{true};

// only touched by the emulation thread
const melonDS::NDSCart::CartCommon* lastCart = nullptr;
u64 curTid = 0;
u32 curCode = 0;
std::string curName;
bool open = false;
qint64 offset = -1;
u32 seconds = 0;
bool running = false;
Clock::time_point runStart;
Clock::time_point lastWrite;
u16 flags = 0;

QString FileName() { return QString::fromStdString(Folder()) + "/melonds.bin"; }

u32 LocalSeconds()
{
    const QDateTime now = QDateTime::currentDateTime();
    return (u32)(now.toSecsSinceEpoch() + now.offsetFromUtc() - 946684800);
}

bool Connected() { return Link::Get().GetState() == Link::State::Connected; }

void EnsurePcFile()
{
    const QString path = QString::fromStdString(Folder()) + "/pc.txt";
    if (QFile::exists(path)) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    const quint64 id = QRandomGenerator::system()->generate64();
    QTextStream out(&f);
    out << "# this PC for Dashboard-Link / 3DS Link - the name can be changed, the id not\n";
    out << "id=" << QString("%1").arg(id, 16, 16, QLatin1Char('0')).toUpper() << "\n";
    out << "name=" << QSysInfo::machineHostName() << "\n";
}

void NoteTitle(u64 tid, const std::string& name)
{
    if (tid == 0 || name.empty()) return;
    const QString path = QString::fromStdString(Folder()) + "/titles.txt";
    const QString key = QString("%1").arg(tid, 16, 16, QLatin1Char('0')).toUpper();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        while (!f.atEnd())
            if (QString::fromUtf8(f.readLine()).startsWith(key + '\t')) return;
        f.close();
    }
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((key + '\t' + QString::fromStdString(name) + '\n').toUtf8());
}

u32 SecondsNow()
{
    u32 s = seconds;
    if (running)
        s += (u32)std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - runStart).count();
    return s;
}

void Update(bool closing)
{
    if (!open || offset < 0) return;
    if (Connected()) flags |= kFlagWith3ds;
    if (closing) flags |= kFlagClosed;
    lastWrite = Clock::now();
    QFile f(FileName());
    if (!f.open(QIODevice::ReadWrite)) return;
    const u32 s = SecondsNow();
    f.seek(offset + 12);
    f.write((const char*)&s, 4);
    f.seek(offset + 20);
    f.write((const char*)&flags, 2);
}

bool Append()
{
    QFile f(FileName());
    if (!f.open(QIODevice::ReadWrite)) return false;
    char head[16];
    if (f.size() < 16 || f.read(head, 16) != 16 || memcmp(head, kMagic, 8) != 0)
    {
        if (f.size() >= 16) return false;   // not a file of ours: never touched
        f.resize(0);
        f.write(kMagic, 8);
        f.write((const char*)&kVersion, 4);
        f.write((const char*)&kEntrySize, 4);
    }
    offset = 16 + (f.size() - 16) / kEntrySize * kEntrySize;   // a cut-off last entry is overwritten
    char e[kEntrySize] = {};
    const u32 start = LocalSeconds();
    memcpy(e, &curTid, 8);
    memcpy(e + 8, &start, 4);
    memcpy(e + 16, &curCode, 4);
    memcpy(e + 20, &flags, 2);
    f.seek(offset);
    return f.write(e, kEntrySize) == kEntrySize && f.resize(offset + kEntrySize);
}

// game code as the 3DS writes it into DS title ids: the four letters, first one highest
void ReadCart(const melonDS::NDSCart::CartCommon* cart)
{
    curTid = 0;
    curCode = 0;
    curName.clear();
    if (!cart) return;
    const auto& h = cart->GetHeader();
    const u32 code = (u32)(u8)h.GameCode[0] << 24 | (u32)(u8)h.GameCode[1] << 16 |
                     (u32)(u8)h.GameCode[2] << 8 | (u32)(u8)h.GameCode[3];
    if (code == 0) return;
    curCode = code;
    curTid = 0x0004800400000000ULL | code;
    if (const melonDS::NDSBanner* b = cart->Banner())
    {
        QString t = QString::fromUtf16(b->EnglishTitle, 128);
        t = t.left(t.indexOf(QChar(0)) < 0 ? 128 : t.indexOf(QChar(0)));
        curName = t.section('\n', 0, 0).trimmed().toStdString();
    }
    if (curName.empty())
        curName = QString::fromLatin1(h.GameTitle, strnlen(h.GameTitle, 12)).trimmed().toStdString();
}

void Run()
{
    if (!open)
    {
        EnsurePcFile();
        NoteTitle(curTid, curName);
        flags = Connected() ? kFlagWith3ds : 0;
        seconds = 0;
        if (!Append()) return;
        open = true;
    }
    if (!running)
    {
        running = true;
        runStart = Clock::now();
        Update(false);
    }
}

void Hold()
{
    if (!open || !running) return;
    seconds = SecondsNow();
    running = false;
    Update(false);
}

}

void SetEnabled(bool on) { enabled = on; }
bool Enabled() { return enabled; }

std::string Folder()
{
#ifdef _WIN32
    QString base = qEnvironmentVariable("APPDATA");
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    const QString dir = base + "/3DS-Link/activity";
    QDir().mkpath(dir);
    return dir.toStdString();
}

void Frame(const melonDS::NDSCart::CartCommon* cart, bool isRunning)
{
    if (!enabled)
    {
        End();
        lastCart = nullptr;
        return;
    }
    if (cart != lastCart)
    {
        End();
        lastCart = cart;
        ReadCart(cart);
    }
    if (curTid == 0) return;
    if (isRunning) Run();
    else Hold();
    if (open && running && Clock::now() - lastWrite >= std::chrono::seconds(30))
        Update(false);
}

void End()
{
    if (!open) return;
    seconds = SecondsNow();
    running = false;
    Update(true);
    open = false;
    offset = -1;
}

}
