/*
    melonDS Link - "ROMs vom 3DS", see LinkRomDialog.h
*/

#include "LinkRomDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

#include "Config.h"

namespace
{

QPointer<LinkRomDialog> current;

std::string human(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024) snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    return buf;
}

QString q(const std::string& s) { return QString::fromStdString(s); }

std::string stripExt(const std::string& name)
{
    size_t p = name.rfind('.');
    return p == std::string::npos ? name : name.substr(0, p);
}

}

void LinkRomDialog::openDlg(QWidget* parent)
{
    if (!current) current = new LinkRomDialog(parent);
    current->show();
    current->raise();
    current->activateWindow();
}

LinkRomDialog::LinkRomDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("melonDS Link - ROMs vom 3DS");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 520);

    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel("Hier kannst du Spiele vorab auf den PC kopieren. Danach starten sie sofort, "
                             "wenn du sie auf dem 3DS auswaehlst. Auf dem 3DS steht bei kopierten Spielen \"PC\".\n"
                             "Waehrend des Kopierens bleibt der untere 3DS-Bildschirm stehen - am besten nicht nebenbei spielen.");
    intro->setWordWrap(true);
    lay->addWidget(intro);

    table = new QTableWidget(0, 3, this);
    table->setHorizontalHeaderLabels({"Spiel", "Groesse", "Status"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table, 1);

    summary = new QLabel(this);
    lay->addWidget(summary);
    info = new QLabel(this);
    info->setWordWrap(true);
    lay->addWidget(info);
    bar = new QProgressBar(this);
    bar->setRange(0, 1000);
    bar->setValue(0);
    bar->setVisible(false);
    lay->addWidget(bar);

    chkSkip = new QCheckBox("Fuellbereich ueberspringen (schneller): hinter den Spieldaten liest der 3DS nur Stichproben "
                            "statt alles. Findet er dort doch etwas, liest er ab da wieder alles.", this);
    chkSkip->setChecked(Link::Session::SkipPadding.load());
    connect(chkSkip, &QCheckBox::toggled, this, [](bool on)
    {
        Link::Session::SkipPadding = on;
        Config::Table cfg = Config::GetGlobalTable();
        cfg.SetBool("Link.SkipPadding", on);
        Config::Save();
    });
    lay->addWidget(chkSkip);

    auto* buttons = new QHBoxLayout();
    btnRefresh = new QPushButton("Aktualisieren", this);
    btnCopySel = new QPushButton("Angehakte kopieren", this);
    btnCopyAll = new QPushButton("Alle fehlenden kopieren", this);
    btnStop = new QPushButton("Anhalten", this);
    btnClose = new QPushButton("Schliessen", this);
    buttons->addWidget(btnRefresh);
    buttons->addWidget(btnCopySel);
    buttons->addWidget(btnCopyAll);
    buttons->addWidget(btnStop);
    buttons->addStretch(1);
    buttons->addWidget(btnClose);
    lay->addLayout(buttons);

    connect(btnRefresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(btnCopySel, &QPushButton::clicked, this, [this] { startCopy(true); });
    connect(btnCopyAll, &QPushButton::clicked, this, [this] { startCopy(false); });
    connect(btnStop, &QPushButton::clicked, this, [this]
    {
        stopReq = true;
        btnStop->setEnabled(false);
        std::lock_guard<std::mutex> lock(m);
        label = "Wird angehalten (nach dem aktuellen Abschnitt, ein paar Sekunden)...\n"
                "Der bisher kopierte Teil bleibt erhalten und wird beim naechsten Mal fortgesetzt.";
    });
    connect(btnClose, &QPushButton::clicked, this, [this] { close(); });

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { onTimer(); });
    timer->start(200);

    setBusy(false);
    refresh();
}

LinkRomDialog::~LinkRomDialog()
{
    stopReq = true;
    joinWorker();
}

void LinkRomDialog::joinWorker()
{
    if (worker.joinable()) worker.join();
}

void LinkRomDialog::closeEvent(QCloseEvent* event)
{
    if (busy)
    {
        // let the copy stop cleanly first; the window closes by itself then
        stopReq = true;
        closeWhenDone = true;
        btnStop->setEnabled(false);
        {
            std::lock_guard<std::mutex> lock(m);
            label = "Wird angehalten und dann geschlossen...";
        }
        event->ignore();
        return;
    }
    event->accept();
}

void LinkRomDialog::setBusy(bool b)
{
    busy = b;
    bool connected = Link::Get().GetState() == Link::State::Connected;
    btnRefresh->setEnabled(!b);
    btnCopySel->setEnabled(!b && connected);
    btnCopyAll->setEnabled(!b && connected);
    btnStop->setEnabled(b);
    bar->setVisible(b);
}

void LinkRomDialog::refresh()
{
    if (busy) return;
    joinWorker();
    if (Link::Get().GetState() != Link::State::Connected)
    {
        summary->setText("Nicht mit dem 3DS verbunden (3DS Link > Mit 3DS verbinden).");
        info->clear();
        setBusy(false);
        return;
    }
    stopReq = false;
    workerDone = false;
    {
        std::lock_guard<std::mutex> lock(m);
        label = "Lese die Spieleliste vom 3DS...";
        result.clear();
    }
    setBusy(true);
    bar->setVisible(false);
    worker = std::thread([this]
    {
        std::string dir;
        std::vector<Link::Session::RemoteRom> list;
        bool ok = Link::Session::Get().FetchGameList(dir, list);
        {
            std::lock_guard<std::mutex> lock(m);
            if (ok)
            {
                romDir = dir;
                roms = list;
                listChanged = true;
                label.clear();
            }
            else
                result = "Die Spieleliste konnte nicht gelesen werden. Ist auf dem 3DS die neueste melonDS-Link-App?";
        }
        workerDone = true;
    });
}

void LinkRomDialog::fillTable()
{
    // keep the ticks across refreshes
    std::vector<std::string> ticked;
    for (int r = 0; r < table->rowCount(); r++)
        if (table->item(r, 0)->checkState() == Qt::Checked)
            ticked.push_back(table->item(r, 0)->data(Qt::UserRole).toString().toStdString());

    table->setRowCount((int)roms.size());
    int onPc = 0;
    uint64_t missingBytes = 0;
    for (int r = 0; r < (int)roms.size(); r++)
    {
        const auto& rom = roms[r];
        bool have = Link::Session::IsOnPC(rom.name, rom.size);
        uint64_t part = have ? 0 : Link::Session::PartialSize(rom.name);
        if (part > rom.size) part = 0;
        if (have) onPc++;
        else missingBytes += rom.size - part;

        auto* name = new QTableWidgetItem(q(stripExt(rom.name)));
        name->setData(Qt::UserRole, q(rom.name));
        name->setFlags(Qt::ItemIsEnabled | (have ? Qt::NoItemFlags : Qt::ItemIsUserCheckable));
        if (!have)
        {
            bool t = std::find(ticked.begin(), ticked.end(), rom.name) != ticked.end();
            name->setCheckState(t ? Qt::Checked : Qt::Unchecked);
        }
        table->setItem(r, 0, name);

        auto* size = new QTableWidgetItem(q(human(rom.size)));
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(r, 1, size);

        QString st;
        if (have) st = QString::fromUtf8("\xE2\x9C\x93 auf dem PC");
        else if (part > 0) st = QString("angefangen (%1%)").arg((int)(part * 100 / rom.size));
        else st = "fehlt";
        auto* status = new QTableWidgetItem(st);
        if (have) status->setForeground(QColor(0, 140, 0));
        table->setItem(r, 2, status);
    }

    summary->setText(QString("%1 von %2 Spielen auf dem PC%3   (Ordner auf dem 3DS: %4)")
        .arg(onPc).arg(roms.size())
        .arg(missingBytes ? q(", noch " + human(missingBytes) + " zu kopieren") : QString())
        .arg(q(romDir)));
}

void LinkRomDialog::startCopy(bool onlySelected)
{
    if (busy) return;
    joinWorker();

    std::vector<Link::Session::RemoteRom> queue;
    for (int r = 0; r < table->rowCount() && r < (int)roms.size(); r++)
    {
        const auto& rom = roms[r];
        if (Link::Session::IsOnPC(rom.name, rom.size)) continue;
        if (onlySelected && table->item(r, 0)->checkState() != Qt::Checked) continue;
        queue.push_back(rom);
    }
    if (queue.empty())
    {
        info->setText(onlySelected ? "Nichts angehakt (bereits kopierte Spiele lassen sich nicht anhaken)."
                                   : "Alle Spiele sind schon auf dem PC.");
        return;
    }

    uint64_t total = 0;
    for (const auto& rom : queue)
    {
        uint64_t part = Link::Session::PartialSize(rom.name);
        total += rom.size - (part <= rom.size ? part : 0);
    }

    stopReq = false;
    workerDone = false;
    {
        std::lock_guard<std::mutex> lock(m);
        result.clear();
        label = "Starte...";
        barCur = 0;
        barTotal = total;
    }
    setBusy(true);
    std::string dir = romDir;
    worker = std::thread([this, queue, dir, total]
    {
        auto& s = Link::Session::Get();
        const uint32_t launches = s.LaunchCount();
        uint64_t doneBefore = 0;
        int copied = 0;
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        Link::Session::Log("Vorladen gestartet: " + std::to_string(queue.size()) + " Spiel(e), " + human(total));

        for (size_t i = 0; i < queue.size(); i++)
        {
            const auto& rom = queue[i];
            if (stopReq || s.LaunchCount() != launches) break;
            uint64_t part = Link::Session::PartialSize(rom.name);
            if (part > rom.size) part = 0;
            const std::string game = stripExt(rom.name);

            auto progress = [&](uint64_t off, uint64_t size, uint64_t wire, double secs) -> bool
            {
                double rate = secs > 0 ? (off - part) / secs : 0;
                uint64_t all = doneBefore + (off - part);
                double allSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                double allRate = allSecs > 0 ? all / allSecs : 0;
                int eta = allRate > 0 ? (int)((total - all) / allRate) : 0;
                char buf[400];
                snprintf(buf, sizeof(buf),
                         "Kopiere %d von %d: %s\n%s von %s  -  %s/s  -  WLAN %s/s, Kompression spart %d%%\n"
                         "Insgesamt %s von %s  -  noch ca. %d:%02d",
                         (int)i + 1, (int)queue.size(), game.c_str(), human(off).c_str(), human(size).c_str(),
                         human((uint64_t)rate).c_str(), human((uint64_t)(secs > 0 ? wire / secs : 0)).c_str(),
                         off > part ? (int)(100 - wire * 100 / (off - part)) : 0,
                         human(all).c_str(), human(total).c_str(), eta / 60, eta % 60);
                std::lock_guard<std::mutex> lock(m);
                if (!stopReq) label = buf;
                barCur = all;
                return true;
            };
            auto keepGoing = [&] { return !stopReq && s.LaunchCount() == launches; };

            err = s.CopyRom(dir + "/" + rom.name, rom.size, progress, keepGoing);
            doneBefore += rom.size - part;
            if (!err.empty()) break;
            copied++;
        }

        std::string res;
        if (s.LaunchCount() != launches)
            res = "Angehalten, weil auf dem 3DS ein Spiel gestartet wurde. Der bisher kopierte Teil bleibt erhalten.";
        else if (stopReq)
            res = "Angehalten. Der bisher kopierte Teil bleibt erhalten und wird beim naechsten Mal fortgesetzt.";
        else if (!err.empty())
            res = err;
        else
        {
            int secs = (int)std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char buf[160];
            snprintf(buf, sizeof(buf), "Fertig: %d Spiel(e) kopiert in %d:%02d.", copied, secs / 60, secs % 60);
            res = buf;
        }
        Link::Session::Log("Vorladen beendet: " + res);
        {
            std::lock_guard<std::mutex> lock(m);
            result = res;
            label.clear();
            listChanged = true;   // statuses changed
        }
        workerDone = true;
    });
}

void LinkRomDialog::onTimer()
{
    std::string lab, res;
    bool changed;
    uint64_t cur, tot;
    {
        std::lock_guard<std::mutex> lock(m);
        lab = label;
        res = result;
        changed = listChanged;
        listChanged = false;
        cur = barCur;
        tot = barTotal;
    }
    if (changed) fillTable();
    if (busy && tot > 0) bar->setValue((int)(cur * 1000 / tot));

    if (busy && workerDone)
    {
        joinWorker();
        setBusy(false);
        if (closeWhenDone) { close(); return; }
    }
    if (!busy)
    {
        // the connection may have come or gone
        bool connected = Link::Get().GetState() == Link::State::Connected;
        btnCopySel->setEnabled(connected);
        btnCopyAll->setEnabled(connected);
    }
    info->setText(q(lab.empty() ? res : lab));
}
