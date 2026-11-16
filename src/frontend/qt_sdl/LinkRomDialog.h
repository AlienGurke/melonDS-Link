/*
    melonDS Link - "ROMs vom 3DS": which games of the 3DS are already on the
    PC, and copying the missing ones ahead of time (without playing).
*/

#ifndef MELONDS_LINKROMDIALOG_H
#define MELONDS_LINKROMDIALOG_H

#include <QDialog>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LinkSession.h"

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

class LinkRomDialog : public QDialog
{
public:
    // shows the window (only one at a time)
    static void openDlg(QWidget* parent);
    ~LinkRomDialog() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    explicit LinkRomDialog(QWidget* parent);

    void refresh();
    void startCopy(bool onlySelected);
    void deleteTicked();
    void onTimer();
    void fillTable();
    void setBusy(bool busy);
    void joinWorker();

    QTableWidget* table;
    QLabel* summary;
    QLabel* info;
    QProgressBar* bar;
    QCheckBox* chkSkip;
    QPushButton* btnRefresh;
    QPushButton* btnCopySel;
    QPushButton* btnCopyAll;
    QPushButton* btnDelete;
    QPushButton* btnStop;
    QPushButton* btnClose;
    QTimer* timer;

    std::thread worker;
    std::atomic<bool> busy { false };
    std::atomic<bool> stopReq { false };
    std::atomic<bool> workerDone { false };
    bool closeWhenDone = false;

    // shared with the worker
    std::mutex m;
    std::string romDir;
    std::vector<Link::Session::RemoteRom> roms;
    bool listChanged = false;
    std::string label, result;
    uint64_t barCur = 0, barTotal = 0;
};

#endif
