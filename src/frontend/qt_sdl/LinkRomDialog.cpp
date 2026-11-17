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
#include <QListWidget>
#include <QDialogButtonBox>
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
#include "LinkLang.h"

using Link::Tr;
using Link::TrS;

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

QString mmss(double secs)
{
    int s = (int)secs;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

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
    setWindowTitle(Tr("roms.title"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 520);

    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(Tr("roms.intro"));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    table = new QTableWidget(0, 3, this);
    table->setHorizontalHeaderLabels({Tr("roms.col.game"), Tr("roms.col.size"), Tr("roms.col.status")});
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

    chkSkip = new QCheckBox(Tr("roms.skip"), this);
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
    btnRefresh = new QPushButton(Tr("roms.refresh"), this);
    btnCopySel = new QPushButton(Tr("roms.copysel"), this);
    btnCopyAll = new QPushButton(Tr("roms.copyall"), this);
    btnDelete = new QPushButton(Tr("roms.delete"), this);
    btnBackups = new QPushButton(Tr("roms.backups"), this);
    btnStop = new QPushButton(Tr("roms.stop"), this);
    btnClose = new QPushButton(Tr("roms.close"), this);
    buttons->addWidget(btnRefresh);
    buttons->addWidget(btnCopySel);
    buttons->addWidget(btnCopyAll);
    buttons->addWidget(btnDelete);
    buttons->addWidget(btnBackups);
    buttons->addWidget(btnStop);
    buttons->addStretch(1);
    buttons->addWidget(btnClose);
    lay->addLayout(buttons);

    connect(btnRefresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(btnCopySel, &QPushButton::clicked, this, [this] { startCopy(true); });
    connect(btnCopyAll, &QPushButton::clicked, this, [this] { startCopy(false); });
    connect(btnDelete, &QPushButton::clicked, this, [this] { deleteTicked(); });
    connect(btnBackups, &QPushButton::clicked, this, [this] { showBackups(); });
    connect(btnStop, &QPushButton::clicked, this, [this]
    {
        stopReq = true;
        btnStop->setEnabled(false);
        std::lock_guard<std::mutex> lock(m);
        label = TrS("roms.stopping");
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
            label = TrS("roms.stopclose");
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
    btnDelete->setEnabled(!b);
    btnBackups->setEnabled(!b);
    btnStop->setEnabled(b);
    bar->setVisible(b);
}

void LinkRomDialog::refresh()
{
    if (busy) return;
    joinWorker();
    if (Link::Get().GetState() != Link::State::Connected)
    {
        summary->setText(Tr("roms.notconnected"));
        info->clear();
        setBusy(false);
        return;
    }
    stopReq = false;
    workerDone = false;
    {
        std::lock_guard<std::mutex> lock(m);
        label = TrS("roms.reading");
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
                result = TrS("roms.readfail");
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
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        {
            bool t = std::find(ticked.begin(), ticked.end(), rom.name) != ticked.end();
            name->setCheckState(t ? Qt::Checked : Qt::Unchecked);
        }
        table->setItem(r, 0, name);

        auto* size = new QTableWidgetItem(q(human(rom.size)));
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(r, 1, size);

        QString st;
        if (have) st = Tr("roms.onpc");
        else if (part > 0) st = Tr("roms.started").arg((int)(part * 100 / rom.size));
        else st = Tr("roms.missing");
        auto* status = new QTableWidgetItem(st);
        if (have) status->setForeground(QColor(0, 140, 0));
        table->setItem(r, 2, status);
    }

    summary->setText(Tr("roms.summary")
        .arg(onPc).arg(roms.size())
        .arg(missingBytes ? Tr("roms.left").arg(q(human(missingBytes))) : QString())
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
        info->setText(onlySelected ? Tr("roms.nonemissing") : Tr("roms.allthere"));
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
        label = TrS("roms.starting");
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
                QString text = Tr("roms.progress")
                    .arg(QString::number(i + 1), QString::number(queue.size()), q(game), q(human(off)), q(human(size)),
                         q(human((uint64_t)rate)), q(human((uint64_t)(secs > 0 ? wire / secs : 0))),
                         QString::number(off > part ? (int)(100 - wire * 100 / (off - part)) : 0), q(human(all)))
                    .arg(q(human(total)), mmss(eta));
                std::lock_guard<std::mutex> lock(m);
                if (!stopReq) label = text.toStdString();
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
            res = TrS("roms.stoppedlaunch");
        else if (stopReq)
            res = TrS("roms.stopped");
        else if (!err.empty())
            res = err;
        else
        {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            res = Tr("roms.done").arg(copied).arg(mmss(secs)).toStdString();
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

void LinkRomDialog::deleteTicked()
{
    if (busy) return;
    std::vector<Link::Session::RemoteRom> sel;
    uint64_t bytes = 0;
    for (int r = 0; r < table->rowCount() && r < (int)roms.size(); r++)
    {
        const auto& rom = roms[r];
        if (table->item(r, 0)->checkState() != Qt::Checked) continue;
        bool have = Link::Session::IsOnPC(rom.name, rom.size);
        uint64_t part = Link::Session::PartialSize(rom.name);
        if (!have && part == 0) continue;
        sel.push_back(rom);
        bytes += have ? rom.size : part;
    }
    if (sel.empty())
    {
        info->setText(Tr("roms.nonetodelete"));
        return;
    }

    QMessageBox box(QMessageBox::Question, "melonDS Link",
        Tr("roms.askdelete").arg(sel.size()).arg(q(human(bytes))),
        QMessageBox::Yes | QMessageBox::No, this);
    auto* withBackups = new QCheckBox(Tr("roms.delbackups"));
    box.setCheckBox(withBackups);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    int done = 0;
    std::string skipped;
    for (const auto& rom : sel)
    {
        if (Link::Session::Get().DeleteFromPC(rom.name, withBackups->isChecked())) done++;
        else skipped += (skipped.empty() ? "" : ", ") + stripExt(rom.name);
    }
    for (int r = 0; r < table->rowCount(); r++) table->item(r, 0)->setCheckState(Qt::Unchecked);
    fillTable();
    std::string msg = Tr("roms.deleted").arg(done).toStdString();
    if (!skipped.empty()) msg += Tr("roms.notdeleted").arg(q(skipped)).toStdString();
    {
        std::lock_guard<std::mutex> lock(m);
        result = msg;
    }
    info->setText(q(msg));
}

void LinkRomDialog::showBackups()
{
    if (busy) return;
    std::string romName;
    int count = 0;
    for (int r = 0; r < table->rowCount() && r < (int)roms.size(); r++)
        if (table->item(r, 0)->checkState() == Qt::Checked) { romName = roms[r].name; count++; }
    if (count != 1)
    {
        info->setText(Tr("roms.pickone"));
        return;
    }
    const std::string game = stripExt(romName);
    auto list = Link::Session::Backups(game);

    QDialog dlg(this);
    dlg.setWindowTitle(Tr("bak.title").arg(q(game)));
    dlg.resize(480, 360);
    auto* lay = new QVBoxLayout(&dlg);
    auto* text = new QLabel(list.empty() ? Tr("bak.none") : Tr("bak.intro"), &dlg);
    text->setWordWrap(true);
    lay->addWidget(text);
    auto* lw = new QListWidget(&dlg);
    for (const auto& b : list) lw->addItem(q(b.when + "   (" + human(b.size) + ")"));
    if (!list.empty()) lw->setCurrentRow(0);
    lay->addWidget(lw, 1);
    auto* bb = new QDialogButtonBox(&dlg);
    QPushButton* restore = bb->addButton(Tr("bak.restore"), QDialogButtonBox::AcceptRole);
    bb->addButton(Tr("roms.close"), QDialogButtonBox::RejectRole);
    restore->setEnabled(!list.empty() && Link::Get().GetState() == Link::State::Connected);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(lw, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted || lw->currentRow() < 0 || list.empty()) return;
    const auto chosen = list[lw->currentRow()];

    if (QMessageBox::question(this, "melonDS Link", Tr("bak.confirm").arg(q(chosen.when)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    joinWorker();
    stopReq = false;
    workerDone = false;
    {
        std::lock_guard<std::mutex> lock(m);
        result.clear();
        label = TrS("bak.working");
        barCur = 0;
        barTotal = 0;
    }
    setBusy(true);
    btnStop->setEnabled(false);
    std::string dir = romDir;
    worker = std::thread([this, dir, romName, chosen]
    {
        std::string err = Link::Session::Get().RestoreBackup(dir, romName, chosen.path);
        std::lock_guard<std::mutex> lock(m);
        result = err.empty() ? TrS("bak.done") : Tr("bak.failed").arg(q(err)).toStdString();
        label.clear();
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
