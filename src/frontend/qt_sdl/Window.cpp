/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "NDS.h"
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <vector>
#include <string>
#include <algorithm>

#include <QProcess>
#include <QApplication>
#include <QMessageBox>
#include <QMenuBar>
#include <QMimeDatabase>
#include <QFileDialog>
#include <QInputDialog>
#include <thread>
#include <QPaintEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>

#include "main.h"
#include "CheatsDialog.h"
#include "DateTimeDialog.h"
#include "EmuSettingsDialog.h"
#include "InputConfig/InputConfigDialog.h"
#include "VideoSettingsDialog.h"
#include "CameraSettingsDialog.h"
#include "AudioSettingsDialog.h"
#include "FirmwareSettingsDialog.h"
#include "PathSettingsDialog.h"
#include "MPSettingsDialog.h"
#include "WifiSettingsDialog.h"
#include "InterfaceSettingsDialog.h"
#include "ROMInfoDialog.h"
#include "RAMInfoDialog.h"
#include "TitleManagerDialog.h"
#include "PowerManagement/PowerManagementDialog.h"

#include "Platform.h"
#include "Config.h"
#include "version.h"
#include "Savestate.h"
#include "MPInterface.h"
#include "LANDialog.h"

//#include "main_shaders.h"

#include "EmuInstance.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "Window.h"
#include "AboutDialog.h"
#include "LinkClient.h"
#include "LinkSession.h"
#include "LinkRomDialog.h"
#include "LinkLang.h"
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include "SaveManager.h"

using namespace melonDS;




extern CameraManager* camManager[2];
extern bool camStarted[2];


QString NdsRomMimeType = "application/x-nintendo-ds-rom";
QStringList NdsRomExtensions { ".nds", ".srl", ".dsi", ".ids" };

QString GbaRomMimeType = "application/x-gba-rom";
QStringList GbaRomExtensions { ".gba", ".agb" };


// This list of supported archive formats is based on libarchive(3) version 3.6.2 (2022-12-09).
QStringList ArchiveMimeTypes
{
#ifdef ARCHIVE_SUPPORT_ENABLED
    "application/zip",
    "application/x-7z-compressed",
    "application/vnd.rar", // *.rar
    "application/x-tar",

    "application/x-compressed-tar", // *.tar.gz
    "application/x-xz-compressed-tar",
    "application/x-bzip-compressed-tar",
    "application/x-lz4-compressed-tar",
    "application/x-zstd-compressed-tar",

    "application/x-tarz", // *.tar.Z
    "application/x-lzip-compressed-tar",
    "application/x-lzma-compressed-tar",
    "application/x-lrzip-compressed-tar",
    "application/x-tzo", // *.tar.lzo
#endif
};

QStringList ArchiveExtensions
{
#ifdef ARCHIVE_SUPPORT_ENABLED
    ".zip", ".7z", ".rar", ".tar",

    ".tar.gz", ".tgz",
    ".tar.xz", ".txz",
    ".tar.bz2", ".tbz2",
    ".tar.lz4", ".tlz4",
    ".tar.zst", ".tzst",

    ".tar.Z", ".taz",
    ".tar.lz",
    ".tar.lzma", ".tlz",
    ".tar.lrz", ".tlrz",
    ".tar.lzo", ".tzo"
#endif
};

// AAAAAAA
static bool FileExtensionInList(const QString& filename, const QStringList& extensions, Qt::CaseSensitivity cs = Qt::CaseInsensitive)
{
    return std::any_of(extensions.cbegin(), extensions.cend(), [&](const auto& ext) {
        return filename.endsWith(ext, cs);
    });
}

static bool MimeTypeInList(const QMimeType& mimetype, const QStringList& superTypeNames)
{
    return std::any_of(superTypeNames.cbegin(), superTypeNames.cend(), [&](const auto& superTypeName) {
        return mimetype.inherits(superTypeName);
    });
}


static bool NdsRomByExtension(const QString& filename)
{
    return FileExtensionInList(filename, NdsRomExtensions);
}

static bool GbaRomByExtension(const QString& filename)
{
    return FileExtensionInList(filename, GbaRomExtensions);
}

static bool SupportedArchiveByExtension(const QString& filename)
{
    return FileExtensionInList(filename, ArchiveExtensions);
}


static bool NdsRomByMimetype(const QMimeType& mimetype)
{
    return mimetype.inherits(NdsRomMimeType);
}

static bool GbaRomByMimetype(const QMimeType& mimetype)
{
    return mimetype.inherits(GbaRomMimeType);
}

static bool SupportedArchiveByMimetype(const QMimeType& mimetype)
{
    return MimeTypeInList(mimetype, ArchiveMimeTypes);
}

static bool ZstdNdsRomByExtension(const QString& filename)
{
    return filename.endsWith(".zst", Qt::CaseInsensitive) &&
        NdsRomByExtension(filename.left(filename.size() - 4));
}

static bool ZstdGbaRomByExtension(const QString& filename)
{
    return filename.endsWith(".zst", Qt::CaseInsensitive) &&
        GbaRomByExtension(filename.left(filename.size() - 4));
}

static bool FileIsSupportedFiletype(const QString& filename, bool insideArchive = false)
{
    if (ZstdNdsRomByExtension(filename) || ZstdGbaRomByExtension(filename))
        return true;

    if (NdsRomByExtension(filename) || GbaRomByExtension(filename) || SupportedArchiveByExtension(filename))
        return true;

    const auto matchmode = insideArchive ? QMimeDatabase::MatchExtension : QMimeDatabase::MatchDefault;
    const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, matchmode);
    return NdsRomByMimetype(mimetype) || GbaRomByMimetype(mimetype) || SupportedArchiveByMimetype(mimetype);
}



MainWindow::MainWindow(int id, EmuInstance* inst, QWidget* parent) :
    QMainWindow(parent),
    windowID(id),
    emuInstance(inst),
    globalCfg(inst->globalCfg),
    localCfg(inst->localCfg),
    windowCfg(localCfg.GetTable("Window"+std::to_string(id), "Window0")),
    emuThread(inst->getEmuThread()),
    enabledSaved(false),
    focused(true)
{

    showOSD = windowCfg.GetBool("ShowOSD");

    setWindowTitle("melonDS " MELONDS_VERSION);
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);

#if QT_VERSION_MAJOR == 6 && WIN32
    // The "windows11" theme has pretty massive padding around menubar items, this makes Config and Help not fit in a window at 1x screen sizing
    // So let's reduce the padding a bit.
    if (QApplication::style()->name() == "windows11")
        setStyleSheet("QMenuBar::item { padding: 4px 8px; }");
#endif

    //hasMenu = (!parent);
    hasMenu = true;

    if (hasMenu)
    {
        QMenuBar * menubar = new QMenuBar();
        {
            QMenu * menu = menubar->addMenu("File");

            actOpenROM = menu->addAction("Open ROM...");
            connect(actOpenROM, &QAction::triggered, this, &MainWindow::onOpenFile);
            actOpenROM->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));

            /*actOpenROMArchive = menu->addAction("Open ROM inside archive...");
            connect(actOpenROMArchive, &QAction::triggered, this, &MainWindow::onOpenFileArchive);
            actOpenROMArchive->setShortcut(QKeySequence(Qt::Key_O | Qt::CTRL | Qt::SHIFT));*/

            recentMenu = menu->addMenu("Open recent");
            loadRecentFilesMenu(true);

            //actBootFirmware = menu->addAction("Launch DS menu");
            actBootFirmware = menu->addAction("Boot firmware");
            connect(actBootFirmware, &QAction::triggered, this, &MainWindow::onBootFirmware);

            menu->addSeparator();

            actCurrentCart = menu->addAction("DS slot: " + emuInstance->cartLabel());
            actCurrentCart->setEnabled(false);

            actInsertCart = menu->addAction("Insert cart...");
            connect(actInsertCart, &QAction::triggered, this, &MainWindow::onInsertCart);

            actEjectCart = menu->addAction("Eject cart");
            connect(actEjectCart, &QAction::triggered, this, &MainWindow::onEjectCart);

            menu->addSeparator();

            actCurrentGBACart = menu->addAction("GBA slot: " + emuInstance->gbaCartLabel());
            actCurrentGBACart->setEnabled(false);

            actInsertGBACart = menu->addAction("Insert ROM cart...");
            connect(actInsertGBACart, &QAction::triggered, this, &MainWindow::onInsertGBACart);

            {
                QMenu * submenu = menu->addMenu("Insert add-on cart");
                QAction *act;

                int addons[] = {
                    GBAAddon_RAMExpansion,
                    GBAAddon_RumblePak,
                    GBAAddon_SolarSensorBoktai1,
                    GBAAddon_SolarSensorBoktai2,
                    GBAAddon_SolarSensorBoktai3,
                    GBAAddon_MotionPakHomebrew,
                    GBAAddon_MotionPakRetail,
                    GBAAddon_GuitarGrip,
                    -1
                };

                for (int i = 0; addons[i] != -1; i++)
                {
                    int addon = addons[i];
                    act = submenu->addAction(emuInstance->gbaAddonName(addon));
                    act->setData(QVariant(addon));
                    connect(act, &QAction::triggered, this, &MainWindow::onInsertGBAAddon);
                    actInsertGBAAddon.append(act);
                }
            }

            actEjectGBACart = menu->addAction("Eject cart");
            connect(actEjectGBACart, &QAction::triggered, this, &MainWindow::onEjectGBACart);

            menu->addSeparator();

            actImportSavefile = menu->addAction("Import savefile");
            connect(actImportSavefile, &QAction::triggered, this, &MainWindow::onImportSavefile);

            menu->addSeparator();

            {
                QMenu * submenu = menu->addMenu("Save state");

                for (int i = 1; i < 9; i++)
                {
                    actSaveState[i] = submenu->addAction(QString("%1").arg(i));
                    actSaveState[i]->setShortcut(QKeySequence(Qt::ShiftModifier | (Qt::Key_F1 + i - 1)));
                    actSaveState[i]->setData(QVariant(i));
                    connect(actSaveState[i], &QAction::triggered, this, &MainWindow::onSaveState);
                }

                actSaveState[0] = submenu->addAction("File...");
                actSaveState[0]->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_F9));
                actSaveState[0]->setData(QVariant(0));
                connect(actSaveState[0], &QAction::triggered, this, &MainWindow::onSaveState);
            }
            {
                QMenu * submenu = menu->addMenu("Load state");

                for (int i = 1; i < 9; i++)
                {
                    actLoadState[i] = submenu->addAction(QString("%1").arg(i));
                    actLoadState[i]->setShortcut(QKeySequence(Qt::Key_F1 + i - 1));
                    actLoadState[i]->setData(QVariant(i));
                    connect(actLoadState[i], &QAction::triggered, this, &MainWindow::onLoadState);
                }

                actLoadState[0] = submenu->addAction("File...");
                actLoadState[0]->setShortcut(QKeySequence(Qt::Key_F9));
                actLoadState[0]->setData(QVariant(0));
                connect(actLoadState[0], &QAction::triggered, this, &MainWindow::onLoadState);
            }

            actUndoStateLoad = menu->addAction("Undo state load");
            actUndoStateLoad->setShortcut(QKeySequence(Qt::Key_F12));
            connect(actUndoStateLoad, &QAction::triggered, this, &MainWindow::onUndoStateLoad);

            menu->addSeparator();
            actOpenConfig = menu->addAction("Open melonDS directory");
            connect(actOpenConfig, &QAction::triggered, this, [&]()
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(emuDirectory));
            });

            menu->addSeparator();

            actQuit = menu->addAction("Quit");
            connect(actQuit, &QAction::triggered, this, &MainWindow::onQuit);
            actQuit->setShortcut(QKeySequence(QKeySequence::StandardKey::Quit));
        }
        {
            QMenu * menu = menubar->addMenu("System");

            actPause = menu->addAction("Pause");
            actPause->setCheckable(true);
            connect(actPause, &QAction::triggered, this, &MainWindow::onPause);

            actReset = menu->addAction("Reset");
            connect(actReset, &QAction::triggered, this, &MainWindow::onReset);

            actStop = menu->addAction("Stop");
            connect(actStop, &QAction::triggered, this, &MainWindow::onStop);

            actFrameStep = menu->addAction("Frame step");
            connect(actFrameStep, &QAction::triggered, this, &MainWindow::onFrameStep);

            menu->addSeparator();

            actPowerManagement = menu->addAction("Power management");
            connect(actPowerManagement, &QAction::triggered, this, &MainWindow::onOpenPowerManagement);

            actDateTime = menu->addAction("Date and time");
            connect(actDateTime, &QAction::triggered, this, &MainWindow::onOpenDateTime);

            menu->addSeparator();

            actEnableCheats = menu->addAction("Enable cheats");
            actEnableCheats->setCheckable(true);
            connect(actEnableCheats, &QAction::triggered, this, &MainWindow::onEnableCheats);

            //if (inst == 0)
            {
                actSetupCheats = menu->addAction("Setup cheat codes");
                actSetupCheats->setMenuRole(QAction::NoRole);
                connect(actSetupCheats, &QAction::triggered, this, &MainWindow::onSetupCheats);

                menu->addSeparator();
                actROMInfo = menu->addAction("ROM info");
                connect(actROMInfo, &QAction::triggered, this, &MainWindow::onROMInfo);

                actRAMInfo = menu->addAction("RAM search");
                connect(actRAMInfo, &QAction::triggered, this, &MainWindow::onRAMInfo);

                actTitleManager = menu->addAction("Manage DSi titles");
                connect(actTitleManager, &QAction::triggered, this, &MainWindow::onOpenTitleManager);
            }

            {
                menu->addSeparator();
                QMenu * submenu = menu->addMenu("Multiplayer");

                actMPNewInstance = submenu->addAction("Launch new instance");
                connect(actMPNewInstance, &QAction::triggered, this, &MainWindow::onMPNewInstance);

                submenu->addSeparator();

                actLANStartHost = submenu->addAction("Host LAN game");
                connect(actLANStartHost, &QAction::triggered, this, &MainWindow::onLANStartHost);

                actLANStartClient = submenu->addAction("Join LAN game");
                connect(actLANStartClient, &QAction::triggered, this, &MainWindow::onLANStartClient);

                /*submenu->addSeparator();

                actNPStartHost = submenu->addAction("NETPLAY HOST");
                connect(actNPStartHost, &QAction::triggered, this, &MainWindow::onNPStartHost);

                actNPStartClient = submenu->addAction("NETPLAY CLIENT");
                connect(actNPStartClient, &QAction::triggered, this, &MainWindow::onNPStartClient);

                actNPTest = submenu->addAction("NETPLAY GO");
                connect(actNPTest, &QAction::triggered, this, &MainWindow::onNPTest);*/
            }
        }
        {
            QMenu * menu = menubar->addMenu("View");

            {
                QMenu * submenu = menu->addMenu("Screen size");

                for (int i = 0; i < 4; i++)
                {
                    int data = i + 1;
                    actScreenSize[i] = submenu->addAction(QString("%1x").arg(data));
                    actScreenSize[i]->setData(QVariant(data));
                    connect(actScreenSize[i], &QAction::triggered, this, &MainWindow::onChangeScreenSize);
                }
            }
            {
                QMenu * submenu = menu->addMenu("Screen rotation");
                grpScreenRotation = new QActionGroup(submenu);

                for (int i = 0; i < screenRot_MAX; i++)
                {
                    int data = i * 90;
                    actScreenRotation[i] = submenu->addAction(QString("%1°").arg(data));
                    actScreenRotation[i]->setActionGroup(grpScreenRotation);
                    actScreenRotation[i]->setData(QVariant(i));
                    actScreenRotation[i]->setCheckable(true);
                }

                connect(grpScreenRotation, &QActionGroup::triggered, this, &MainWindow::onChangeScreenRotation);
            }
            {
                QMenu * submenu = menu->addMenu("Screen gap");
                grpScreenGap = new QActionGroup(submenu);

                const int screengap[] = {0, 1, 8, 64, 90, 128};

                for (int i = 0; i < 6; i++)
                {
                    int data = screengap[i];
                    actScreenGap[i] = submenu->addAction(QString("%1 px").arg(data));
                    actScreenGap[i]->setActionGroup(grpScreenGap);
                    actScreenGap[i]->setData(QVariant(data));
                    actScreenGap[i]->setCheckable(true);
                }

                connect(grpScreenGap, &QActionGroup::triggered, this, &MainWindow::onChangeScreenGap);
            }
            {
                QMenu * submenu = menu->addMenu("Screen layout");
                grpScreenLayout = new QActionGroup(submenu);

                const char *screenlayout[] = {"Natural", "Vertical", "Horizontal", "Hybrid"};

                for (int i = 0; i < screenLayout_MAX; i++)
                {
                    actScreenLayout[i] = submenu->addAction(QString(screenlayout[i]));
                    actScreenLayout[i]->setActionGroup(grpScreenLayout);
                    actScreenLayout[i]->setData(QVariant(i));
                    actScreenLayout[i]->setCheckable(true);
                }

                connect(grpScreenLayout, &QActionGroup::triggered, this, &MainWindow::onChangeScreenLayout);

                submenu->addSeparator();

                actScreenSwap = submenu->addAction("Swap screens");
                actScreenSwap->setCheckable(true);
                connect(actScreenSwap, &QAction::triggered, this, &MainWindow::onChangeScreenSwap);
            }
            {
                QMenu * submenu = menu->addMenu("Screen sizing");
                grpScreenSizing = new QActionGroup(submenu);

                const char *screensizing[] = {"Even", "Emphasize top", "Emphasize bottom", "Auto", "Top only",
                                              "Bottom only"};

                for (int i = 0; i < screenSizing_MAX; i++)
                {
                    actScreenSizing[i] = submenu->addAction(QString(screensizing[i]));
                    actScreenSizing[i]->setActionGroup(grpScreenSizing);
                    actScreenSizing[i]->setData(QVariant(i));
                    actScreenSizing[i]->setCheckable(true);
                }

                connect(grpScreenSizing, &QActionGroup::triggered, this, &MainWindow::onChangeScreenSizing);

                submenu->addSeparator();

                actIntegerScaling = submenu->addAction("Force integer scaling");
                actIntegerScaling->setCheckable(true);
                connect(actIntegerScaling, &QAction::triggered, this, &MainWindow::onChangeIntegerScaling);
            }
            {
                QMenu * submenu = menu->addMenu("Aspect ratio");
                grpScreenAspectTop = new QActionGroup(submenu);
                grpScreenAspectBot = new QActionGroup(submenu);
                actScreenAspectTop = new QAction *[AspectRatiosNum];
                actScreenAspectBot = new QAction *[AspectRatiosNum];

                for (int i = 0; i < 2; i++)
                {
                    QActionGroup * group = grpScreenAspectTop;
                    QAction **actions = actScreenAspectTop;

                    if (i == 1)
                    {
                        group = grpScreenAspectBot;
                        submenu->addSeparator();
                        actions = actScreenAspectBot;
                    }

                    for (int j = 0; j < AspectRatiosNum; j++)
                    {
                        auto ratio = aspectRatios[j];
                        QString label = QString("%1 %2").arg(i ? "Bottom" : "Top", ratio.label);
                        actions[j] = submenu->addAction(label);
                        actions[j]->setActionGroup(group);
                        actions[j]->setData(QVariant(ratio.id));
                        actions[j]->setCheckable(true);
                    }

                    connect(group, &QActionGroup::triggered, this, &MainWindow::onChangeScreenAspect);
                }
            }

            menu->addSeparator();

            actNewWindow = menu->addAction("Open new window");
            connect(actNewWindow, &QAction::triggered, this, &MainWindow::onOpenNewWindow);

            menu->addSeparator();

            {
                // melonDS Link: fullscreen without menu bar and taskbar (F11, Esc leaves it)
                QAction* actFullscreen = menu->addAction("Fullscreen");
                actFullscreen->setShortcut(QKeySequence(Qt::Key_F11));
                actFullscreen->setShortcutContext(Qt::WindowShortcut);
                addAction(actFullscreen);   // keeps working while the menu bar is hidden
                connect(actFullscreen, &QAction::triggered, this, [this] { toggleFullscreen(); });

                actLeaveFullscreen = new QAction(this);
                actLeaveFullscreen->setEnabled(false);   // Esc stays free for key mappings otherwise
                actLeaveFullscreen->setShortcut(QKeySequence(Qt::Key_Escape));
                actLeaveFullscreen->setShortcutContext(Qt::WindowShortcut);
                addAction(actLeaveFullscreen);
                connect(actLeaveFullscreen, &QAction::triggered, this, [this] { if (isFullScreen()) toggleFullscreen(); });
                menu->addSeparator();
            }

            actScreenFiltering = menu->addAction("Screen filtering");
            actScreenFiltering->setCheckable(true);
            connect(actScreenFiltering, &QAction::triggered, this, &MainWindow::onChangeScreenFiltering);

            actShowOSD = menu->addAction("Show OSD");
            actShowOSD->setCheckable(true);
            connect(actShowOSD, &QAction::triggered, this, &MainWindow::onChangeShowOSD);
        }
        {
            QMenu * menu = menubar->addMenu("Config");

            actEmuSettings = menu->addAction("Emu settings");
            connect(actEmuSettings, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);

#ifdef __APPLE__
            actPreferences = menu->addAction("Preferences...");
            connect(actPreferences, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);
            actPreferences->setMenuRole(QAction::PreferencesRole);
#endif

            actInputConfig = menu->addAction("Input and hotkeys");
            connect(actInputConfig, &QAction::triggered, this, &MainWindow::onOpenInputConfig);

            actVideoSettings = menu->addAction("Video settings");
            connect(actVideoSettings, &QAction::triggered, this, &MainWindow::onOpenVideoSettings);

            actCameraSettings = menu->addAction("Camera settings");
            connect(actCameraSettings, &QAction::triggered, this, &MainWindow::onOpenCameraSettings);

            actAudioSettings = menu->addAction("Audio settings");
            connect(actAudioSettings, &QAction::triggered, this, &MainWindow::onOpenAudioSettings);

            actMPSettings = menu->addAction("Multiplayer settings");
            connect(actMPSettings, &QAction::triggered, this, &MainWindow::onOpenMPSettings);

            actWifiSettings = menu->addAction("Wifi settings");
            connect(actWifiSettings, &QAction::triggered, this, &MainWindow::onOpenWifiSettings);

            actFirmwareSettings = menu->addAction("Firmware settings");
            connect(actFirmwareSettings, &QAction::triggered, this, &MainWindow::onOpenFirmwareSettings);

            actInterfaceSettings = menu->addAction("Interface settings");
            connect(actInterfaceSettings, &QAction::triggered, this, &MainWindow::onOpenInterfaceSettings);

            actPathSettings = menu->addAction("Path settings");
            connect(actPathSettings, &QAction::triggered, this, &MainWindow::onOpenPathSettings);

            menu->addSeparator();

            actLimitFramerate = menu->addAction("Limit framerate");
            actLimitFramerate->setCheckable(true);
            connect(actLimitFramerate, &QAction::triggered, this, &MainWindow::onChangeLimitFramerate);

            actAudioSync = menu->addAction("Audio sync");
            actAudioSync->setCheckable(true);
            connect(actAudioSync, &QAction::triggered, this, &MainWindow::onChangeAudioSync);
        }
        {
            QMenu * menu = menubar->addMenu("3DS Link");
            static bool langReady = false;
            if (!langReady)
            {
                langReady = true;
                std::string lang = globalCfg.GetString("Link.Language");
                Link::InitLanguages(lang.empty() ? "en" : lang);
            }

            actLinkConnect = menu->addAction("");
            connect(actLinkConnect, &QAction::triggered, this, &MainWindow::onLinkConnect);

            actLinkDisconnect = menu->addAction("");
            linkTexts.push_back({actLinkDisconnect, "menu.disconnect"});
            connect(actLinkDisconnect, &QAction::triggered, this, &MainWindow::onLinkDisconnect);

            actLinkStatusInfo = menu->addAction("");
            actLinkStatusInfo->setEnabled(false);
            actLinkStatusInfo->setVisible(false);

            menu->addSeparator();
            QAction* actLinkRoms = menu->addAction("");
            linkTexts.push_back({actLinkRoms, "menu.roms"});
            connect(actLinkRoms, &QAction::triggered, this, [this] { LinkRomDialog::openDlg(this); });
            QAction* actLinkFolder = menu->addAction("");
            linkTexts.push_back({actLinkFolder, "menu.folder"});
            connect(actLinkFolder, &QAction::triggered, this, []
            {
                QString dir = QString::fromStdString(Link::Session::BaseDir());
                QDir().mkpath(dir + "/Backups");
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            });
            QAction* actLinkShots = menu->addAction("");
            linkTexts.push_back({actLinkShots, "menu.screenshots"});
            connect(actLinkShots, &QAction::triggered, this, []
            {
                QString dir = QString::fromStdString(Link::Session::ScreenshotDir());
                QDir().mkpath(dir);
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            });

            actLinkPauseOnLoss = menu->addAction("");
            actLinkPauseOnLoss->setCheckable(true);
            actLinkPauseOnLoss->setChecked(!globalCfg.GetBool("Link.NoPauseOnLoss"));
            linkTexts.push_back({actLinkPauseOnLoss, "menu.pauseonloss"});
            connect(actLinkPauseOnLoss, &QAction::toggled, this, [this](bool on)
            {
                globalCfg.SetBool("Link.NoPauseOnLoss", !on);
                Config::Save();
            });
            actLinkPauseOnLid = menu->addAction("");
            actLinkPauseOnLid->setCheckable(true);
            actLinkPauseOnLid->setChecked(!globalCfg.GetBool("Link.NoPauseOnLid"));
            linkTexts.push_back({actLinkPauseOnLid, "menu.pauseonlid"});
            connect(actLinkPauseOnLid, &QAction::toggled, this, [this](bool on)
            {
                globalCfg.SetBool("Link.NoPauseOnLid", !on);
                Config::Save();
            });
            actLinkBlackout = menu->addAction("");
            actLinkBlackout->setCheckable(true);
            actLinkBlackout->setChecked(globalCfg.GetBool("Link.BlackWhenClosed"));
            linkTexts.push_back({actLinkBlackout, "menu.blackout"});
            connect(actLinkBlackout, &QAction::toggled, this, [this](bool on)
            {
                globalCfg.SetBool("Link.BlackWhenClosed", on);
                Config::Save();
            });
            actLinkAutoConnect = menu->addAction("");
            actLinkAutoConnect->setCheckable(true);
            actLinkAutoConnect->setChecked(globalCfg.GetBool("Link.AutoConnect"));
            linkTexts.push_back({actLinkAutoConnect, "menu.autoconnect"});
            connect(actLinkAutoConnect, &QAction::toggled, this, [this](bool on)
            {
                globalCfg.SetBool("Link.AutoConnect", on);
                Config::Save();
            });
            // the picture on the 3DS: 20 a second for 30 fps games too, the WiFi priority class
            {
                Link::Get().optionTile20 = globalCfg.GetBool("Link.Tile20");
                Link::Get().wifiPrio = std::clamp(globalCfg.GetInt("Link.WifiPrio"), 0, 2);
                QMenu* videoMenu = menu->addMenu("");
                linkMenus.push_back({videoMenu, "menu.video"});
                connect(videoMenu, &QMenu::aboutToShow, this, [this, videoMenu]
                {
                    videoMenu->clear();
                    QAction* t20 = videoMenu->addAction(Link::Tr("menu.video.tile20"));
                    t20->setCheckable(true);
                    t20->setChecked(Link::Get().optionTile20);
                    connect(t20, &QAction::toggled, this, [this](bool on)
                    {
                        Link::Get().optionTile20 = on;
                        globalCfg.SetBool("Link.Tile20", on);
                        Config::Save();
                    });
                    videoMenu->addSeparator();
                    QAction* head = videoMenu->addAction(Link::Tr("menu.video.prio"));
                    head->setEnabled(false);
                    auto* group = new QActionGroup(videoMenu);
                    static const char* keys[] = {"menu.video.prio.video", "menu.video.prio.background", "menu.video.prio.normal"};
                    for (int p = 0; p < 3; p++)
                    {
                        QAction* a = videoMenu->addAction(Link::Tr(keys[p]));
                        a->setCheckable(true);
                        a->setChecked(Link::Get().wifiPrio == p);
                        group->addAction(a);
                        connect(a, &QAction::triggered, this, [this, p]
                        {
                            Link::Get().wifiPrio = p;
                            globalCfg.SetInt("Link.WifiPrio", p);
                            Config::Save();
                        });
                    }
                });
            }
            // save backups: how many of the newest, and for how many days played the first one
            // (settings stored +1, so 0 = never set = the default)
            {
                int keep = globalCfg.GetInt("Link.BackupKeepPlus1");
                int days = globalCfg.GetInt("Link.BackupDaysPlus1");
                Link::Session::backupKeep = keep > 0 ? keep - 1 : 3;
                Link::Session::backupDays = days > 0 ? days - 1 : 7;
                QAction* actKeep = menu->addAction("");
                QAction* actDays = menu->addAction("");
                auto label = [actKeep, actDays]
                {
                    actKeep->setText(Link::Tr("menu.backupkeep").arg(Link::Session::backupKeep.load()));
                    actDays->setText(Link::Tr("menu.backupdays").arg(Link::Session::backupDays.load()));
                };
                label();
                linkRelabel.push_back(label);
                connect(actKeep, &QAction::triggered, this, [this, label]
                {
                    bool ok = false;
                    int n = QInputDialog::getInt(this, Link::Tr("menu.backups.title"), Link::Tr("menu.backupkeep.ask"),
                                                 Link::Session::backupKeep.load(), 1, 1000, 1, &ok);
                    if (!ok) return;
                    Link::Session::backupKeep = n;
                    globalCfg.SetInt("Link.BackupKeepPlus1", n + 1);
                    Config::Save();
                    label();
                });
                connect(actDays, &QAction::triggered, this, [this, label]
                {
                    bool ok = false;
                    int n = QInputDialog::getInt(this, Link::Tr("menu.backups.title"), Link::Tr("menu.backupdays.ask"),
                                                 Link::Session::backupDays.load(), 0, 3650, 1, &ok);
                    if (!ok) return;
                    Link::Session::backupDays = n;
                    globalCfg.SetInt("Link.BackupDaysPlus1", n + 1);
                    Config::Save();
                    label();
                });
            }
            // 3DS battery and WiFi on the PC screen
            linkStatusMode = std::clamp(globalCfg.GetInt("Link.StatusMode"), 0, 8);
            linkStatusMenu = menu->addMenu("");
            linkMenus.push_back({linkStatusMenu, "menu.status"});
            connect(linkStatusMenu, &QMenu::aboutToShow, this, [this]
            {
                linkStatusMenu->clear();
                auto* group = new QActionGroup(linkStatusMenu);
                static const char* keys[] = {"status.mode.off", "status.mode.always", "status.mode.moving"};
                // menu order; 8 = wandering over the side (added later, so the number is 8)
                static const int order[] = {0, 1, 2, 8, 3, 4, 5, 6, 7};
                for (int m : order)
                {
                    static const int mins[] = {5, 10, 20, 30, 60};
                    QString label = m == 8 ? Link::Tr("status.mode.wander")
                                  : m < 3 ? Link::Tr(keys[m]) : Link::Tr("status.mode.every").arg(mins[m - 3]);
                    QAction* a = linkStatusMenu->addAction(label);
                    a->setCheckable(true);
                    a->setChecked(m == linkStatusMode);
                    group->addAction(a);
                    connect(a, &QAction::triggered, this, [this, m]
                    {
                        linkStatusMode = m;
                        globalCfg.SetInt("Link.StatusMode", m);
                        Config::Save();
                    });
                    if (m == 8) linkStatusMenu->addSeparator();
                }
                // which lines (one below the other)
                linkStatusMenu->addSeparator();
                QAction* bat = linkStatusMenu->addAction(Link::Tr("status.show.battery"));
                bat->setCheckable(true);
                bat->setChecked(linkStatusBattery);
                connect(bat, &QAction::toggled, this, [this](bool on)
                {
                    linkStatusBattery = on;
                    globalCfg.SetBool("Link.StatusNoBattery", !on);
                    Config::Save();
                });
                QAction* wifi = linkStatusMenu->addAction(Link::Tr("status.show.wifi"));
                wifi->setCheckable(true);
                wifi->setChecked(linkStatusWifi);
                connect(wifi, &QAction::toggled, this, [this](bool on)
                {
                    linkStatusWifi = on;
                    globalCfg.SetBool("Link.StatusNoWifi", !on);
                    Config::Save();
                });
            });
            linkStatusBattery = !globalCfg.GetBool("Link.StatusNoBattery");
            linkStatusWifi = !globalCfg.GetBool("Link.StatusNoWifi");

            // which messages melonDS shows (all by default; errors always)
            static const struct { Link::Note note; const char* cfg; const char* key; } kNotes[] = {
                {Link::Note::Connection, "Link.NoNotify.Connection", "notes.connection"},
                {Link::Note::Save, "Link.NoNotify.Save", "notes.save"},
                {Link::Note::Rom, "Link.NoNotify.Rom", "notes.rom"},
                {Link::Note::Screenshot, "Link.NoNotify.Screenshot", "notes.screenshot"},
                {Link::Note::Pause, "Link.NoNotify.Pause", "notes.pause"},
                {Link::Note::Battery, "Link.NoNotify.Battery", "notes.battery"},
            };
            for (const auto& n : kNotes) Link::SetNoteShown(n.note, !globalCfg.GetBool(n.cfg));
            linkNotesMenu = menu->addMenu("");
            linkMenus.push_back({linkNotesMenu, "menu.notes"});
            connect(linkNotesMenu, &QMenu::aboutToShow, this, [this]
            {
                linkNotesMenu->clear();
                for (const auto& n : kNotes)
                {
                    QAction* a = linkNotesMenu->addAction(Link::Tr(n.key));
                    a->setCheckable(true);
                    a->setChecked(Link::NoteShown(n.note));
                    Link::Note note = n.note;
                    const char* cfg = n.cfg;
                    connect(a, &QAction::toggled, this, [this, note, cfg](bool on)
                    {
                        Link::SetNoteShown(note, on);
                        globalCfg.SetBool(cfg, !on);
                        Config::Save();
                    });
                }
            });

            menu->addSeparator();
            QMenu* langMenu = menu->addMenu("");
            linkMenus.push_back({langMenu, "menu.language"});
            connect(langMenu, &QMenu::aboutToShow, this, [this, langMenu]
            {
                langMenu->clear();
                auto* group = new QActionGroup(langMenu);
                for (const auto& [code, name] : Link::Languages())
                {
                    QAction* a = langMenu->addAction(name);
                    a->setCheckable(true);
                    a->setChecked(code == Link::Language());
                    group->addAction(a);
                    std::string c = code;
                    connect(a, &QAction::triggered, this, [this, c]
                    {
                        Link::SetLanguage(c);
                        globalCfg.SetString("Link.Language", c);
                        Config::Save();
                        retranslateLink();
                    });
                }
            });

            QMenu* debugMenu = menu->addMenu("");
            linkMenus.push_back({debugMenu, "menu.debug"});
            QAction* actLinkSpeed = debugMenu->addAction("");
            linkTexts.push_back({actLinkSpeed, "menu.speedtest"});
            connect(actLinkSpeed, &QAction::triggered, this, [this]
            {
                if (Link::Get().GetState() != Link::State::Connected)
                {
                    QMessageBox::information(this, "melonDS Link", Link::Tr("connect.first"));
                    return;
                }
                if (QMessageBox::question(this, "melonDS Link",
                        Link::Tr("test.askspeed")) != QMessageBox::Yes)
                    return;
                emuInstance->osdAddMessage(0, "%s", Link::TrS("test.speedrunning").c_str());
                std::thread([this]
                {
                    struct Cfg { uint32_t buf, piece, sleep; bool block; };
                    // non-blocking (current method) vs. blocking sends like ftpd
                    std::vector<Cfg> cfgs = {
                        {32768, 65536, 500, false}, {262144, 65536, 500, false},
                        {32768, 65536, 500, true}, {65536, 65536, 500, true}, {131072, 65536, 500, true},
                        {262144, 65536, 500, true}, {262144, 262144, 500, true}, {32768, 16384, 500, true},
                    };
                    QString table;
                    Link::Session::Log("WLAN-Test gestartet");
                    double bestRate = 0;
                    Cfg best = cfgs[0];
                    auto runOne = [&](const Cfg& c) -> double
                    {
                        LinkTuning t = {};
                        t.megabytes = 8;
                        t.sndbuf = c.buf;
                        t.piece = c.piece;
                        t.sleepUs = c.sleep;
                        t.flags = LINK_TUNE_SET | (c.block ? LINK_TUNE_BLOCKING : 0);
                        double rate = 0;
                        bool ok = Link::Get().SpeedTest(t, rate);
                        std::string line = Link::Tr("test.speedline")
                            .arg(Link::Tr(c.block ? "test.blocking" : "test.polling"), -12)
                            .arg(c.buf / 1024, 3).arg(c.piece / 1024, 3)
                            .arg(ok ? QString::number((int)(rate / 1024)) + " KB/s" : Link::Tr("test.error")).toStdString();
                        table += QString::fromStdString(line) + "\n";
                        Link::Session::Log(std::string("  ") + line);
                        Link::Get().PostMessage(std::string("3DS Link: ") + line);
                        if (ok && rate > bestRate) { bestRate = rate; best = c; }
                        return ok ? rate : 0;
                    };
                    for (const Cfg& c : cfgs)
                    {
                        if (Link::Get().GetState() != Link::State::Connected) break;
                        runOne(c);
                    }

                    QString text;
                    if (bestRate > 0)
                    {
                        LinkTuning t = {};
                        t.sndbuf = best.buf;
                        t.piece = best.piece;
                        t.sleepUs = best.sleep;
                        t.flags = LINK_TUNE_SET | (best.block ? LINK_TUNE_BLOCKING : 0);
                        Link::Get().SetTuning(t);
                        Link::Get().SetDefaultTuning(t);
                        QString sum = Link::Tr("test.best")
                            .arg(Link::Tr(best.block ? "test.blocking" : "test.polling"))
                            .arg(best.buf / 1024).arg(best.piece / 1024).arg((int)(bestRate / 1024));
                        Link::Session::Log(sum.toStdString());
                        text = Link::Tr("test.results") + "\n\n" + table + "\n" + sum + "\n\n" + Link::Tr("test.vary");
                        QMetaObject::invokeMethod(this, [this, best]
                        {
                            globalCfg.SetInt("Link.SendBuf", (int)best.buf);
                            globalCfg.SetInt("Link.SendPiece", (int)best.piece);
                            globalCfg.SetInt("Link.SendPauseUs", (int)best.sleep);
                            globalCfg.SetInt("Link.SendBlocking", best.block ? 1 : 0);
                            Config::Save();
                        }, Qt::QueuedConnection);
                    }
                    else
                        text = Link::Tr("test.failed");
                    QMetaObject::invokeMethod(this, [this, text]
                    {
                        QMessageBox box(QMessageBox::Information, "melonDS Link", text, QMessageBox::Ok, this);
                        box.setStyleSheet("QLabel{font-family: Consolas, monospace;}");
                        box.exec();
                    }, Qt::QueuedConnection);
                }).detach();
            });

            QAction* actLinkSd = debugMenu->addAction("");
            linkTexts.push_back({actLinkSd, "menu.sdtest"});
            connect(actLinkSd, &QAction::triggered, this, [this]
            {
                if (Link::Get().GetState() != Link::State::Connected)
                {
                    QMessageBox::information(this, "melonDS Link", Link::Tr("connect.first"));
                    return;
                }
                emuInstance->osdAddMessage(0, "%s", Link::TrS("test.sdrunning").c_str());
                std::thread([this]
                {
                    uint16_t status = 0;
                    std::vector<uint8_t> out;
                    bool ok = Link::Get().Request(LINK_CMD_SDTEST, nullptr, 0, status, &out) && status == LINK_OK;
                    std::string res = ok ? std::string(out.begin(), out.end())
                                         : Link::TrS("test.failed");
                    Link::Session::Log("SD-Test: " + res);
                    QString text = QString::fromStdString(res);
                    text.replace(": ", ":\n").replace(", ", "\n");
                    QMetaObject::invokeMethod(this, [this, text]
                    {
                        QMessageBox::information(this, Link::Tr("test.sdtitle"), text + "\n\n" + Link::Tr("test.inlog"));
                    }, Qt::QueuedConnection);
                }).detach();
            });

            QAction* actLinkHelp = menu->addAction("");
            linkTexts.push_back({actLinkHelp, "menu.help"});
            connect(actLinkHelp, &QAction::triggered, this, [this]
            {
                // "# " = heading, "- " = point; built into a small page
                QString html;
                bool inList = false;
                for (const QString& line : Link::Tr("help.text").split('\n'))
                {
                    QString t = line.toHtmlEscaped();
                    if (line.startsWith("# "))
                    {
                        if (inList) { html += "</ul>"; inList = false; }
                        html += "<h3>" + t.mid(2) + "</h3>";
                    }
                    else if (line.startsWith("- "))
                    {
                        if (!inList) { html += "<ul>"; inList = true; }
                        html += "<li>" + t.mid(2) + "</li>";
                    }
                    else if (!line.isEmpty()) html += "<p>" + t + "</p>";
                }
                if (inList) html += "</ul>";
                QDialog dlg(this);
                dlg.setWindowTitle(Link::Tr("help.title"));
                dlg.resize(680, 620);
                auto* lay = new QVBoxLayout(&dlg);
                auto* view = new QTextBrowser(&dlg);
                view->setHtml(html);
                lay->addWidget(view);
                auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
                connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                lay->addWidget(bb);
                dlg.exec();
            });

            QAction* actLinkDebugFolder = debugMenu->addAction("");
            linkTexts.push_back({actLinkDebugFolder, "menu.debugfolder"});
            connect(actLinkDebugFolder, &QAction::triggered, this, []
            {
                QString dir = QString::fromStdString(Link::Session::DebugDir());
                QDir().mkpath(dir);
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            });

            // games picked on the 3DS are prepared and booted from here (instance 0)
            if (windowID == 0 && emuInstance->getInstanceID() == 0)
            {
                SaveManager::WriteHook = [](const std::string& path, const melonDS::u8* data, melonDS::u32 len)
                {
                    Link::Session::Get().OnSaveWritten(path, data, len);
                };
                {
                    LinkTuning t = {};
                    t.sndbuf = (uint32_t)std::max(0, globalCfg.GetInt("Link.SendBuf"));
                    t.piece = (uint32_t)std::max(0, globalCfg.GetInt("Link.SendPiece"));
                    t.sleepUs = (uint32_t)std::max(0, globalCfg.GetInt("Link.SendPauseUs"));
                    if (t.sndbuf) t.flags = LINK_TUNE_SET | (globalCfg.GetInt("Link.SendBlocking") ? LINK_TUNE_BLOCKING : 0);
                    else
                    {
                        // no own test yet: the best on average in the tests so far
                        t.sndbuf = 262144;
                        t.piece = 262144;
                        t.sleepUs = 500;
                        t.flags = LINK_TUNE_SET | LINK_TUNE_BLOCKING;
                    }
                    Link::Get().SetDefaultTuning(t);
                }
                Link::Session::SkipPadding = globalCfg.GetBool("Link.SkipPadding");
                Link::Session::EmuActive = [this] { return emuThread->emuIsActive(); };
                // connect right away with the last address (setting)
                if (globalCfg.GetBool("Link.AutoConnect"))
                {
                    QString host = globalCfg.GetQString("Link.Host");
                    if (!host.isEmpty())
                    {
                        Link::Get().Connect(host.toStdString());
                        if (Link::NoteShown(Link::Note::Connection)) emuInstance->osdAddMessage(0, "%s", Link::Tr("connect.connecting").arg(host).toStdString().c_str());
                    }
                }
                linkTimer = new QTimer(this);
                connect(linkTimer, &QTimer::timeout, this, &MainWindow::onLinkTick);
                linkTimer->start(200);
            }

            connect(menu, &QMenu::aboutToShow, this, [this]
            {
                retranslateLink();
            });
            retranslateLink();
        }
        {
            QMenu * menu = menubar->addMenu("Help");
            actAbout = menu->addAction("About...");
            connect(actAbout, &QAction::triggered, this, [&]
            {
                auto dialog = AboutDialog(this);
                dialog.exec();
            });
        }

        setMenuBar(menubar);

        if (localCfg.GetString("Firmware.Username") == "Arisotura")
            actMPNewInstance->setText("Fart");
    }

#ifdef Q_OS_MAC
    QPoint screenCenter = screen()->availableGeometry().center();
    QRect frameGeo = frameGeometry();
    frameGeo.moveCenter(screenCenter);
    move(frameGeo.topLeft());
#endif

    std::string geom = windowCfg.GetString("Geometry");
    if (!geom.empty())
    {
        QByteArray raw = QByteArray::fromStdString(geom);
        QByteArray dec = QByteArray::fromBase64(raw, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!dec.isEmpty())
            restoreGeometry(dec);
        // if the window was closed in fullscreen do not restore this
        setWindowState(windowState() & ~Qt::WindowFullScreen);
    }
    show();

    panel = nullptr;
    createScreenPanel();

    if (hasMenu)
    {
        actEjectCart->setEnabled(false);
        actEjectGBACart->setEnabled(false);

        if (globalCfg.GetInt("Emu.ConsoleType") == 1)
        {
            actInsertGBACart->setEnabled(false);
            for (auto act: actInsertGBAAddon)
                act->setEnabled(false);
        }

        for (int i = 0; i < 9; i++)
        {
            actSaveState[i]->setEnabled(false);
            actLoadState[i]->setEnabled(false);
        }
        actUndoStateLoad->setEnabled(false);
        actImportSavefile->setEnabled(false);

        actPause->setEnabled(false);
        actReset->setEnabled(false);
        actStop->setEnabled(false);
        actFrameStep->setEnabled(false);

        //actDateTime->setEnabled(true);
        actPowerManagement->setEnabled(false);

        actEnableCheats->setEnabled(false);
        actSetupCheats->setEnabled(false);
        actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());

        actEnableCheats->setChecked(localCfg.GetBool("EnableCheats"));

        actROMInfo->setEnabled(false);
        actRAMInfo->setEnabled(false);

        actScreenRotation[windowCfg.GetInt("ScreenRotation")]->setChecked(true);

        int screenGap = windowCfg.GetInt("ScreenGap");
        for (int i = 0; i < 6; i++)
        {
            if (actScreenGap[i]->data().toInt() == screenGap)
            {
                actScreenGap[i]->setChecked(true);
                break;
            }
        }

        actScreenLayout[windowCfg.GetInt("ScreenLayout")]->setChecked(true);
        actScreenSizing[windowCfg.GetInt("ScreenSizing")]->setChecked(true);
        actIntegerScaling->setChecked(windowCfg.GetBool("IntegerScaling"));

        actScreenSwap->setChecked(windowCfg.GetBool("ScreenSwap"));

        int aspectTop = windowCfg.GetInt("ScreenAspectTop");
        int aspectBot = windowCfg.GetInt("ScreenAspectBot");
        for (int i = 0; i < AspectRatiosNum; i++)
        {
            if (aspectTop == aspectRatios[i].id)
                actScreenAspectTop[i]->setChecked(true);
            if (aspectBot == aspectRatios[i].id)
                actScreenAspectBot[i]->setChecked(true);
        }

        actScreenFiltering->setChecked(windowCfg.GetBool("ScreenFilter"));
        actShowOSD->setChecked(showOSD);

        actLimitFramerate->setChecked(emuInstance->doLimitFPS);
        actAudioSync->setChecked(emuInstance->doAudioSync);

        if (emuInstance->instanceID > 0)
        {
            actEmuSettings->setEnabled(false);
            actVideoSettings->setEnabled(false);
            actMPSettings->setEnabled(false);
            actWifiSettings->setEnabled(false);
            actInterfaceSettings->setEnabled(false);

#ifdef __APPLE__
            actPreferences->setEnabled(false);
#endif // __APPLE__
        }

        if (emuThread->emuIsActive())
            onEmuStart();
    }

    QObject::connect(qApp, &QApplication::applicationStateChanged, this, &MainWindow::onAppStateChanged);
    onUpdateInterfaceSettings();

    updateMPInterface(MPInterface::GetType());
}

MainWindow::~MainWindow()
{
    if (hasMenu)
    {
        delete[] actScreenAspectTop;
        delete[] actScreenAspectBot;
    }
}

void MainWindow::osdAddMessage(unsigned int color, const char* msg)
{
    if (!showOSD) return;
    panel->osdAddMessage(color, msg);
}

void MainWindow::saveEnabled(bool enabled)
{
    if (enabledSaved) return;
    windowCfg.SetBool("Enabled", enabled);
    enabledSaved = true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (emuInstance)
    {
        if (windowID == 0)
            emuInstance->saveEnabledWindows();
        else
            saveEnabled(false);
    }

    // explicitly close children windows, so the OpenGL contexts get closed properly
    auto childwins = findChildren<MainWindow *>(nullptr, Qt::FindDirectChildrenOnly);
    for (auto child : childwins)
        child->close();

    if (!emuInstance) return;

    QByteArray geom = saveGeometry();
    QByteArray enc = geom.toBase64(QByteArray::Base64Encoding);
    windowCfg.SetString("Geometry", enc.toStdString());
    Config::Save();

    emuInstance->deleteWindow(windowID, false);

    // emuInstance may be deleted
    // prevent use after free from us
    emuInstance = nullptr;
    QMainWindow::closeEvent(event);
}

void MainWindow::createScreenPanel()
{
    auto oldpanel = panel;
    panel = nullptr;
    if (oldpanel) delete oldpanel;

    hasOGL = globalCfg.GetBool("Screen.UseGL") ||
            (globalCfg.GetInt("3D.Renderer") != renderer3D_Software);

    if (hasOGL)
    {
        ScreenPanelGL* panelGL = new ScreenPanelGL(this);
        panelGL->show();

        // make sure no GL context is in use by the emu thread
        // otherwise we may fail to create a shared context
        if (windowID != 0)
            emuThread->borrowGL();

        // Check that creating the context hasn't failed
        if (panelGL->createContext() == false)
        {
            Log(Platform::LogLevel::Error, "Failed to create OpenGL context, falling back to Software Renderer.\n");
            hasOGL = false;

            globalCfg.SetBool("Screen.UseGL", false);
            globalCfg.SetInt("3D.Renderer", renderer3D_Software);

            delete panelGL;
            panelGL = nullptr;
        }

        if (windowID != 0)
            emuThread->returnGL();

        panel = panelGL;
    }

    if (!hasOGL)
    {
        ScreenPanelNative* panelNative = new ScreenPanelNative(this);
        panel = panelNative;
        panel->show();
    }
    setCentralWidget(panel);

    if (hasMenu)
        actScreenFiltering->setEnabled(hasOGL);
    panel->osdSetEnabled(showOSD);

    connect(emuThread, SIGNAL(windowUpdate()), panel, SLOT(repaint()));

    connect(this, SIGNAL(screenLayoutChange()), panel, SLOT(onScreenLayoutChanged()));
    emit screenLayoutChange();
    applyMouseHide();
}

GL::Context* MainWindow::getOGLContext()
{
    if (!hasOGL) return nullptr;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->getContext();
}

void MainWindow::initOpenGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->initOpenGL();
}

void MainWindow::deinitOpenGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->deinitOpenGL();
}

void MainWindow::setGLSwapInterval(int intv)
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->setSwapInterval(intv);
}

void MainWindow::makeCurrentGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->makeCurrentGL();
}

void MainWindow::releaseGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->releaseGL();
}

void MainWindow::drawScreen()
{
    if (!panel) return;
    return panel->drawScreen();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    // TODO!! REMOVE ME IN RELEASE BUILDS!!
    //if (event->key() == Qt::Key_F11) emuInstance->getNDS()->debug(0);

    emuInstance->onKeyPress(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    emuInstance->onKeyRelease(event);
}


void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    QString filename = urls.at(0).toLocalFile();

    if (FileIsSupportedFiletype(filename))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    if (!verifySetup())
        return;

    const QStringList file = splitArchivePath(urls.at(0).toLocalFile(), false);
    if (file.isEmpty())
        return;

    const QString filename = file.last();
    const bool romInsideArchive = file.size() > 1;
    const auto matchMode = romInsideArchive ? QMimeDatabase::MatchExtension : QMimeDatabase::MatchDefault;
    const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, matchMode);

    bool isNdsRom = NdsRomByExtension(filename) || NdsRomByMimetype(mimetype);
    bool isGbaRom = GbaRomByExtension(filename) || GbaRomByMimetype(mimetype);
    isNdsRom |= ZstdNdsRomByExtension(filename);
    isGbaRom |= ZstdGbaRomByExtension(filename);

    QString errorstr;
    if (isNdsRom)
    {
        if (!emuThread->bootROM(file, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return;
        }

        const QString barredFilename = file.join('|');
        recentFileList.removeAll(barredFilename);
        recentFileList.prepend(barredFilename);
        updateRecentFilesMenu();

        updateCartInserted(false);
    }
    else if (isGbaRom)
    {
        if (!emuThread->insertCart(file, true, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return;
        }

        updateCartInserted(true);
    }
    else
    {
        QMessageBox::critical(this, "melonDS", "The file could not be recognized as a DS or GBA ROM.");
        return;
    }
}

void MainWindow::focusInEvent(QFocusEvent* event)
{
    onFocusIn();
}

void MainWindow::focusOutEvent(QFocusEvent* event)
{
    onFocusOut();
}

void MainWindow::onFocusIn()
{
    focused = true;
    if (emuInstance)
        emuInstance->updateAudioMuteByWindowFocus();
}

void MainWindow::onFocusOut()
{
    // focusOutEvent is called through the window close event handler
    // prevent use after free
    focused = false;
    if (emuInstance)
        emuInstance->updateAudioMuteByWindowFocus();
}

void MainWindow::onAppStateChanged(Qt::ApplicationState state)
{
    if (state == Qt::ApplicationInactive)
    {
        emuInstance->keyReleaseAll();
        if (pauseOnLostFocus && emuThread->emuIsRunning())
            emuThread->emuPause();
    }
    else if (state == Qt::ApplicationActive)
    {
        if (pauseOnLostFocus && !pausedManually)
            emuThread->emuUnpause();
    }
}

bool MainWindow::verifySetup()
{
    QString res = emuInstance->verifySetup();
    if (!res.isEmpty())
    {
         QMessageBox::critical(this, "melonDS", res);
         return false;
    }

    return true;
}

bool MainWindow::preloadROMs(QStringList file, QStringList gbafile, bool boot)
{
    QString errorstr;

    if (file.isEmpty() && gbafile.isEmpty() && !boot)
        return false;

    if (!verifySetup())
    {
        return false;
    }

    bool gbaloaded = false;
    if (!gbafile.isEmpty())
    {
        if (!emuThread->insertCart(gbafile, true, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return false;
        }

        gbaloaded = true;
    }

    bool ndsloaded = false;
    if (!file.isEmpty())
    {
        if (boot)
        {
            if (!emuThread->bootROM(file, errorstr))
            {
                QMessageBox::critical(this, "melonDS", errorstr);
                return false;
            }
        }
        else
        {
            if (!emuThread->insertCart(file, false, errorstr))
            {
                QMessageBox::critical(this, "melonDS", errorstr);
                return false;
            }
        }

        recentFileList.removeAll(file.join("|"));
        recentFileList.prepend(file.join("|"));
        updateRecentFilesMenu();
        ndsloaded = true;
    }
    else if (boot)
    {
        if (!emuThread->bootFirmware(errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return false;
        }
    }

    updateCartInserted(false);
    if (gbaloaded)
        updateCartInserted(true);

    return true;
}

QStringList MainWindow::splitArchivePath(const QString& filename, bool useMemberSyntax)
{
    if (filename.isEmpty()) return {};

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (useMemberSyntax)
    {
        const QStringList filenameParts = filename.split('|');
        if (filenameParts.size() > 2)
        {
            QMessageBox::warning(this, "melonDS", "This path contains too many '|'.");
            return {};
        }

        if (filenameParts.size() == 2)
        {
            const QString archive = filenameParts.at(0);
            if (!QFileInfo(archive).exists())
            {
                QMessageBox::warning(this, "melonDS", "This archive does not exist.");
                return {};
            }

            const QString subfile = filenameParts.at(1);
            if (!Archive::ListArchive(archive).contains(subfile))
            {
                QMessageBox::warning(this, "melonDS", "This archive does not contain the desired file.");
                return {};
            }

            return filenameParts;
        }
    }
#endif

    if (!QFileInfo(filename).exists())
    {
        QMessageBox::warning(this, "melonDS", "This ROM file does not exist.");
        return {};
    }

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (SupportedArchiveByExtension(filename)
        || SupportedArchiveByMimetype(QMimeDatabase().mimeTypeForFile(filename)))
    {
        const QString subfile = pickFileFromArchive(filename);
        if (subfile.isEmpty())
            return {};

        return { filename, subfile };
    }
#endif

    return { filename };
}

QString MainWindow::pickFileFromArchive(QString archiveFileName)
{
    QVector<QString> archiveROMList = Archive::ListArchive(archiveFileName);

    if (archiveROMList.size() <= 1)
    {
        if (!archiveROMList.isEmpty() && archiveROMList.at(0) == "OK")
            QMessageBox::warning(this, "melonDS", "This archive is empty.");
        else
            QMessageBox::critical(this, "melonDS", "This archive could not be read. It may be corrupt or you don't have the permissions.");
        return QString();
    }

    archiveROMList.removeFirst();

    const auto notSupportedRom = [&](const auto& filename){
        if (NdsRomByExtension(filename) || GbaRomByExtension(filename))
            return false;
        const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, QMimeDatabase::MatchExtension);
        return !(NdsRomByMimetype(mimetype) || GbaRomByMimetype(mimetype));
    };

    archiveROMList.erase(std::remove_if(archiveROMList.begin(), archiveROMList.end(), notSupportedRom),
                         archiveROMList.end());

    if (archiveROMList.isEmpty())
    {
        QMessageBox::warning(this, "melonDS", "This archive does not contain any supported ROMs.");
        return QString();
    }

    if (archiveROMList.size() == 1)
        return archiveROMList.first();

    bool ok;
    const QString toLoad = QInputDialog::getItem(
        this, "melonDS",
        "This archive contains multiple files. Select which ROM you want to load.",
        archiveROMList.toList(), 0, false, &ok
    );

    if (ok) return toLoad;

    // User clicked on cancel

    return QString();
}

QStringList MainWindow::pickROM(bool gba)
{
    emuThread->emuPause();

    const QString console = gba ? "GBA" : "DS";
    const QStringList& romexts = gba ? GbaRomExtensions : NdsRomExtensions;

    QString rawROMs = romexts.join(" *");
    QString extraFilters = ";;" + console + " ROMs (*" + rawROMs;
    QString allROMs = rawROMs;

    QString zstdROMs = "*" + romexts.join(".zst *") + ".zst";
    extraFilters += ");;Zstandard-compressed " + console + " ROMs (" + zstdROMs + ")";
    allROMs += " " + zstdROMs;

#ifdef ARCHIVE_SUPPORT_ENABLED
    QString archives = "*" + ArchiveExtensions.join(" *");
    extraFilters += ";;Archives (" + archives + ")";
    allROMs += " " + archives;
#endif
    extraFilters += ";;All files (*.*)";

    const QString filename = QFileDialog::getOpenFileName(
        this, "Open " + console + " ROM",
        globalCfg.GetQString("LastROMFolder"),
        "All supported files (*" + allROMs + ")" + extraFilters
    );

    if (filename.isEmpty())
    {
        emuThread->emuUnpause();
        return {};
    }

    globalCfg.SetQString("LastROMFolder", QFileInfo(filename).dir().path());
    auto ret = splitArchivePath(filename, false);
    emuThread->emuUnpause();
    return ret;
}

void MainWindow::updateCartInserted(bool gba)
{
    bool inserted;
    QString label;
    if (gba)
    {
        inserted = emuInstance->gbaCartInserted() && (emuInstance->getConsoleType() == 0);
        label = "GBA slot: " + emuInstance->gbaCartLabel();

        emuInstance->doOnAllWindows([=](MainWindow* win)
        {
            if (!win->hasMenu) return;
            win->actCurrentGBACart->setText(label);
            win->actEjectGBACart->setEnabled(inserted);
        });
    }
    else
    {
        inserted = emuInstance->cartInserted();
        label = "DS slot: " + emuInstance->cartLabel();

        emuInstance->doOnAllWindows([=](MainWindow* win)
        {
            if (!win->hasMenu) return;
            win->actCurrentCart->setText(label);
            win->actEjectCart->setEnabled(inserted);
            win->actImportSavefile->setEnabled(inserted);
            win->actEnableCheats->setEnabled(inserted);
            win->actSetupCheats->setEnabled(inserted);
            win->actROMInfo->setEnabled(inserted);
            win->actRAMInfo->setEnabled(inserted);
        });
    }
}

void MainWindow::onOpenFile()
{
    if (!verifySetup())
        return;

    QStringList file = pickROM(false);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->bootROM(file, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    QString filename = file.join('|');
    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    updateCartInserted(false);
}

void MainWindow::onClearRecentFiles()
{
    recentFileList.clear();
    globalCfg.GetArray("RecentROM").Clear();
    updateRecentFilesMenu();
}

void MainWindow::loadRecentFilesMenu(bool loadcfg)
{
    if (loadcfg)
    {
        recentFileList.clear();

        Config::Array recentROMs = globalCfg.GetArray("RecentROM");
        int numrecent = std::min(kMaxRecentROMs, (int) recentROMs.Size());
        for (int i = 0; i < numrecent; ++i)
        {
            std::string item = recentROMs.GetString(i);
            if (!item.empty())
                recentFileList.push_back(QString::fromStdString(item));
        }
    }

    recentMenu->clear();

    for (int i = 0; i < recentFileList.size(); ++i)
    {
        if (i >= kMaxRecentROMs) break;

        QString item_full = recentFileList.at(i);
        QString item_display = item_full;
        int itemlen = item_full.length();
        const int maxlen = 100;
        if (itemlen > maxlen)
        {
            int cut_start = 0;
            while (item_full[cut_start] != '/' && item_full[cut_start] != '\\' &&
                   cut_start < itemlen)
                cut_start++;

            int cut_end = itemlen-1;
            while (((item_full[cut_end] != '/' && item_full[cut_end] != '\\') ||
                    (cut_start+4+(itemlen-cut_end) < maxlen)) &&
                   cut_end > 0)
                cut_end--;

            item_display.truncate(cut_start+1);
            item_display += "...";
            item_display += QString(item_full).remove(0, cut_end);
        }

        QAction *actRecentFile_i = recentMenu->addAction(QString("%1.  %2").arg(i+1).arg(item_display));
        actRecentFile_i->setData(item_full);
        connect(actRecentFile_i, &QAction::triggered, this, &MainWindow::onClickRecentFile);
    }

    while (recentFileList.size() > 10)
        recentFileList.removeLast();

    recentMenu->addSeparator();

    QAction *actClearRecentList = recentMenu->addAction("Clear");
    connect(actClearRecentList, &QAction::triggered, this, &MainWindow::onClearRecentFiles);

    if (recentFileList.empty())
        actClearRecentList->setEnabled(false);
}

void MainWindow::updateRecentFilesMenu()
{
    Config::Array recentroms = globalCfg.GetArray("RecentROM");
    recentroms.Clear();

    for (int i = 0; i < recentFileList.size(); ++i)
    {
        if (i >= kMaxRecentROMs) break;

        recentroms.SetQString(i, recentFileList.at(i));
    }

    Config::Save();
    loadRecentFilesMenu(false);

    emuInstance->broadcastCommand(InstCmd_UpdateRecentFiles);
}

void MainWindow::onClickRecentFile()
{
    QAction *act = (QAction *)sender();
    QString filename = act->data().toString();

    if (!verifySetup())
        return;

    const QStringList file = splitArchivePath(filename, true);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->bootROM(file, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    updateCartInserted(false);
}

void MainWindow::onBootFirmware()
{
    if (!verifySetup())
        return;

    QString errorstr;
    if (!emuThread->bootFirmware(errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }
}

void MainWindow::onInsertCart()
{
    QStringList file = pickROM(false);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->insertCart(file, false, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(false);
}

void MainWindow::onEjectCart()
{
    emuThread->ejectCart(false);
    updateCartInserted(false);
}

void MainWindow::onInsertGBACart()
{
    QStringList file = pickROM(true);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->insertCart(file, true, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(true);
}

void MainWindow::onInsertGBAAddon()
{
    QAction* act = (QAction*)sender();
    int type = act->data().toInt();

    QString errorstr;
    if (!emuThread->insertGBAAddon(type, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(true);
}

void MainWindow::onEjectGBACart()
{
    emuThread->ejectCart(true);
    updateCartInserted(true);
}

void MainWindow::onSaveState()
{
    int slot = ((QAction*)sender())->data().toInt();

    QString filename;
    if (slot > 0)
    {
        filename = QString::fromStdString(emuInstance->getSavestateName(slot));
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        emuThread->emuPause();
        filename = QFileDialog::getSaveFileName(this,
                                                         "Save state",
                                                         globalCfg.GetQString("LastROMFolder"),
                                                         "melonDS savestates (*.mln);;Any file (*.*)");
        emuThread->emuUnpause();
        if (filename.isEmpty())
            return;
    }

    if (emuThread->saveState(filename))
    {
        if (slot > 0) emuInstance->osdAddMessage(0, "State saved to slot %d", slot);
        else          emuInstance->osdAddMessage(0, "State saved to file");

        actLoadState[slot]->setEnabled(true);
    }
    else
    {
        emuInstance->osdAddMessage(0xFFA0A0, "State save failed");
    }
}

void MainWindow::onLoadState()
{
    int slot = ((QAction*)sender())->data().toInt();

    QString filename;
    if (slot > 0)
    {
        filename = QString::fromStdString(emuInstance->getSavestateName(slot));
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        emuThread->emuPause();
        filename = QFileDialog::getOpenFileName(this,
                                                         "Load state",
                                                         globalCfg.GetQString("LastROMFolder"),
                                                         "melonDS savestates (*.ml*);;Any file (*.*)");
        emuThread->emuUnpause();
        if (filename.isEmpty())
            return;
    }

    if (!Platform::FileExists(filename.toStdString()))
    {
        if (slot > 0) emuInstance->osdAddMessage(0xFFA0A0, "State slot %d is empty", slot);
        else          emuInstance->osdAddMessage(0xFFA0A0, "State file does not exist");

        return;
    }

    if (emuThread->loadState(filename))
    {
        if (slot > 0) emuInstance->osdAddMessage(0, "State loaded from slot %d", slot);
        else          emuInstance->osdAddMessage(0, "State loaded from file");

        actUndoStateLoad->setEnabled(true);
    }
    else
    {
        emuInstance->osdAddMessage(0xFFA0A0, "State load failed");
    }
}

void MainWindow::onUndoStateLoad()
{
    emuThread->undoStateLoad();

    emuInstance->osdAddMessage(0, "State load undone");
}

void MainWindow::onImportSavefile()
{
    QString path = QFileDialog::getOpenFileName(this,
                                            "Select savefile",
                                            globalCfg.GetQString("LastROMFolder"),
                                            "Savefiles (*.sav *.bin *.dsv);;Any file (*.*)");

    if (path.isEmpty())
        return;

    if (!Platform::FileExists(path.toStdString()))
    {
        QMessageBox::critical(this, "melonDS", "Could not open the given savefile.");
        return;
    }

    if (emuThread->emuIsActive())
    {
        if (QMessageBox::warning(this,
                        "melonDS",
                        "The emulation will be reset and the current savefile overwritten.",
                        QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok)
        {
            return;
        }
    }

    if (!emuThread->importSavefile(path))
    {
        QMessageBox::critical(this, "melonDS", "Could not import the given savefile.");
        return;
    }
}

void MainWindow::onQuit()
{
    close();
}


void MainWindow::onPause(bool checked)
{
    if (!emuThread->emuIsActive()) return;

    if (checked)
    {
        emuThread->emuPause();
        pausedManually = true;
    }
    else
    {
        emuThread->emuUnpause();
        pausedManually = false;
    }
}

void MainWindow::onReset()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuReset();
}

void MainWindow::onStop()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuStop(true);
}

void MainWindow::onFrameStep()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuFrameStep();
}

void MainWindow::onOpenDateTime()
{
    DateTimeDialog* dlg = DateTimeDialog::openDlg(this);
    connect(dlg, &DateTimeDialog::finished, this, &MainWindow::onDateTimeDialogFinished);
}

void MainWindow::onDateTimeDialogFinished(int res)
{
    if (!res) return;
    if (!emuThread->emuIsActive()) return;

    emuInstance->setDateTime();
}

void MainWindow::onOpenPowerManagement()
{
    PowerManagementDialog* dlg = PowerManagementDialog::openDlg(this);
}

void MainWindow::onEnableCheats(bool checked)
{
    localCfg.SetBool("EnableCheats", checked);
    emuThread->enableCheats(checked);

    emuInstance->doOnAllWindows([=](MainWindow* win)
    {
        win->actEnableCheats->setChecked(checked);
    }, windowID);
}

void MainWindow::onSetupCheats()
{
    emuThread->emuPause();

    CheatsDialog* dlg = CheatsDialog::openDlg(this);
    connect(dlg, &CheatsDialog::finished, this, &MainWindow::onCheatsDialogFinished);
}

void MainWindow::onCheatsDialogFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onROMInfo()
{
    auto cart = emuInstance->nds->NDSCartSlot.GetCart();
    if (cart)
        ROMInfoDialog* dlg = ROMInfoDialog::openDlg(this);
}

void MainWindow::onRAMInfo()
{
    RAMInfoDialog* dlg = RAMInfoDialog::openDlg(this);
}

void MainWindow::onOpenTitleManager()
{
    TitleManagerDialog* dlg = TitleManagerDialog::openDlg(this);
}

void MainWindow::onMPNewInstance()
{
    createEmuInstance();
}

void MainWindow::onLANStartHost()
{
    if (!lanWarning(true)) return;
    LANStartHostDialog::openDlg(this);
}

void MainWindow::onLANStartClient()
{
    if (!lanWarning(false)) return;
    LANStartClientDialog::openDlg(this);
}

QString MainWindow::linkStatusText()
{
    auto cs = Link::Get().GetConsoleState();
    QString bat = cs.battery >= 0 ? QString("%1 %").arg(cs.battery) : QString("?");
    if (cs.charging) bat += " " + Link::Tr("status.charging");
    static const char* wifiKeys[] = {"status.wifi0", "status.wifi1", "status.wifi2", "status.wifi3"};
    QString wifi = cs.wifi >= 0 ? Link::Tr(wifiKeys[cs.wifi]) : QString("?");
    return Link::Tr("status.line").arg(bat, wifi);
}

std::string MainWindow::linkStatusLines()
{
    auto cs = Link::Get().GetConsoleState();
    std::string out;
    if (linkStatusBattery)
    {
        QString bat = cs.battery >= 0 ? QString("%1 %").arg(cs.battery) : QString("?");
        if (cs.charging) bat += " " + Link::Tr("status.charging");
        out += Link::Tr("status.battery").arg(bat).toStdString();
    }
    if (linkStatusWifi)
    {
        static const char* wifiKeys[] = {"status.wifi0", "status.wifi1", "status.wifi2", "status.wifi3"};
        QString wifi = cs.wifi >= 0 ? Link::Tr(wifiKeys[cs.wifi]) : QString("?");
        if (!out.empty()) out += "\n";
        out += Link::Tr("status.wifi").arg(wifi).toStdString();
    }
    return out;
}

void MainWindow::retranslateLink()
{
    for (auto& [a, key] : linkTexts) a->setText(Link::Tr(key));
    for (auto& relabel : linkRelabel) relabel();
    for (auto& [m, key] : linkMenus) m->setTitle(Link::Tr(key));
    auto st = Link::Get().GetState();
    actLinkDisconnect->setEnabled(st != Link::State::Disconnected);
    if (actLinkStatusInfo)
    {
        bool show = st == Link::State::Connected && Link::Get().GetConsoleState().known;
        actLinkStatusInfo->setVisible(show);
        if (show) actLinkStatusInfo->setText(linkStatusText());
    }
    QString host = QString::fromStdString(Link::Get().GetHost());
    if (st == Link::State::Connected) actLinkConnect->setText(Link::Tr("menu.connected").arg(host));
    else if (st == Link::State::Connecting) actLinkConnect->setText(Link::Tr("menu.connecting").arg(host));
    else actLinkConnect->setText(Link::Tr("menu.connect"));
}

void MainWindow::onLinkConnect()
{
    QString last = globalCfg.GetQString("Link.Host");
    bool ok = false;
    QString host = QInputDialog::getText(this, "melonDS Link", Link::Tr("connect.prompt"),
        QLineEdit::Normal, last, &ok).trimmed();
    if (!ok || host.isEmpty()) return;

    globalCfg.SetQString("Link.Host", host);
    Config::Save();

    Link::Get().Connect(host.toStdString());
    if (Link::NoteShown(Link::Note::Connection)) emuInstance->osdAddMessage(0, "%s", Link::Tr("connect.connecting").arg(host).toStdString().c_str());
}

std::string MainWindow::linkRunningGame()
{
    // the game started from the 3DS, while it is still the one in melonDS
    std::string game = Link::Session::Get().ActiveGame();
    if (game.empty() || !emuThread->emuIsActive() || emuInstance->getBaseAssetName() != game) return "";
    return game;
}

void MainWindow::onLinkTick()
{
    auto st = Link::Get().GetState();

    // the 3DS went away (app closed, WiFi gone) in the middle of a game: pause,
    // so nothing happens without a controller. Not when disconnected on purpose.
    static Link::State lastState = Link::State::Disconnected;
    if (lastState == Link::State::Connected && st == Link::State::Connecting &&
        actLinkPauseOnLoss && actLinkPauseOnLoss->isChecked() &&
        !linkRunningGame().empty() && emuThread->emuIsRunning())
    {
        emuThread->emuPause();
        linkPausedByLoss = true;
        if (Link::NoteShown(Link::Note::Pause)) emuInstance->osdAddMessage(0, "%s", Link::TrS("pause.paused").c_str());
    }
    if (st == Link::State::Disconnected)
    {
        linkPausedByLoss = false;
        linkPausedByLid = false;
        panel->setBlackout(false);
    }
    lastState = st;

    if (st == Link::State::Disconnected) return;

    // continue when the player is back in the game on the 3DS
    if (Link::Get().TakeResumeRequest() && linkPausedByLoss)
    {
        linkPausedByLoss = false;
        if (emuThread->emuIsActive() && !emuThread->emuIsRunning())
        {
            emuThread->emuUnpause();
            if (Link::NoteShown(Link::Note::Pause)) emuInstance->osdAddMessage(0, "%s", Link::TrS("pause.resumed").c_str());
        }
    }
    // "End the game on the PC" from the 3DS menu (the save is written on the way out)
    if (Link::Get().TakeStopRequest() && emuThread->emuIsActive())
    {
        linkPausedByLoss = false;
        emuThread->emuStop(true);
        if (Link::NoteShown(Link::Note::Pause)) emuInstance->osdAddMessage(0, "%s", Link::TrS("stop.done").c_str());
    }

    // save backups from the 3DS menu
    {
        std::string rom;
        int index;
        uint32_t token;
        if (Link::Get().TakeBackupListRequest(rom)) Link::Session::Get().SendBackupList(rom);
        if (Link::Get().TakeRestoreRequest(rom, index, token))
        {
            // loading a backup of the running game ends it first (its save is written on the way out)
            std::string name = rom.substr(rom.find_last_of('/') + 1);
            if (!linkRunningGame().empty() && linkRunningGame() == name.substr(0, name.rfind('.')))
            {
                linkPausedByLoss = false;
                emuThread->emuStop(true);
            }
            Link::Session::Get().RestoreFrom3ds(rom, index, token);
        }
    }

    // the console: battery warnings, status now and then, lid closed
    {
        auto cs = Link::Get().GetConsoleState();
        static int warnedAt = 101;      // lowest warning given (101 = none)
        static int64_t lastStatusShown = 0;
        static uint32_t statusConn = 0;
        if (cs.battery >= 0)
        {
            if (cs.charging || cs.battery > warnedAt + 5) warnedAt = 101;
            for (int th : {30, 20, 10})
            {
                if (!cs.charging && cs.battery <= th && warnedAt > th)
                {
                    // the lowest threshold reached (no three messages at once)
                    int level = cs.battery <= 10 ? 10 : cs.battery <= 20 ? 20 : 30;
                    warnedAt = level;
                    const char* key = level <= 10 ? "battery.10" : level <= 20 ? "battery.20" : "battery.30";
                    const char* key3 = level <= 10 ? "battery.10.3ds" : "battery.low.3ds";
                    if (Link::NoteShown(Link::Note::Battery))
                    {
                        emuInstance->osdAddMessage(0xFFA040, "%s", Link::Tr(key).arg(cs.battery).toStdString().c_str());
                        std::string t3 = QString::fromStdString(Link::Get().Tr3DS(key3)).arg(cs.battery).toStdString();
                        std::thread([t3] { Link::Get().SetStatusText(t3); }).detach();
                    }
                    break;
                }
            }
        }
        // battery/WiFi in the top right corner: always (fixed or slowly moving,
        // kind to OLEDs) or for a few seconds every 5..60 minutes
        int64_t nowS = QDateTime::currentSecsSinceEpoch();
        uint32_t conn = Link::Get().ConnectionCount();
        int shownMode = 0;
        if (cs.known && st == Link::State::Connected)
        {
            if (linkStatusMode == 1 || linkStatusMode == 2) shownMode = linkStatusMode;
            else if (linkStatusMode == 8) shownMode = 3;
            else if (linkStatusMode >= 3)
            {
                static const int mins[] = {5, 10, 20, 30, 60};
                if (conn != statusConn || nowS - lastStatusShown >= mins[linkStatusMode - 3] * 60)
                {
                    statusConn = conn;
                    lastStatusShown = nowS;
                }
                if (nowS - lastStatusShown < 6) shownMode = 2;
            }
        }
        std::string lines = shownMode ? linkStatusLines() : std::string();
        panel->osdSetStatus(lines.c_str(), lines.empty() ? 0 : shownMode);

        // closed: pause (setting) - the 3DS reports it before it goes to sleep
        bool pauseOnLid = !actLinkPauseOnLid || actLinkPauseOnLid->isChecked();
        if (pauseOnLid && cs.lidClosed && !linkRunningGame().empty() && emuThread->emuIsRunning())
        {
            emuThread->emuPause();
            linkPausedByLid = true;
            if (Link::NoteShown(Link::Note::Pause)) emuInstance->osdAddMessage(0, "%s", Link::TrS("pause.lid").c_str());
        }
        if (linkPausedByLid && !cs.lidClosed && st == Link::State::Connected)
        {
            linkPausedByLid = false;
            linkPausedByLoss = false;
            if (emuThread->emuIsActive() && !emuThread->emuIsRunning())
            {
                emuThread->emuUnpause();
                if (Link::NoteShown(Link::Note::Pause)) emuInstance->osdAddMessage(0, "%s", Link::TrS("pause.resumed").c_str());
            }
        }
        // closed and paused: everything black if wanted (nothing lit on an OLED)
        panel->setBlackout(linkPausedByLid && !emuThread->emuIsRunning() &&
                           actLinkBlackout && actLinkBlackout->isChecked());

        // the 3DS needs to know: sleep when closed, or stay awake (game runs on)
        static uint32_t optsConn = 0;
        static int optsSent = -1;
        uint32_t opts = pauseOnLid ? 0 : LINK_OPT_KEEP_AWAKE;
        if (st == Link::State::Connected && (conn != optsConn || (int)opts != optsSent))
        {
            optsConn = conn;
            optsSent = (int)opts;
            std::thread([opts] { Link::Get().SetOptions(opts); }).detach();
        }
    }

    // the 3DS always knows which game runs here (after reconnects, too)
    {
        static std::string sentGame = "\x01";
        static uint32_t sentConn = 0;
        uint32_t conn = Link::Get().ConnectionCount();
        std::string running = linkRunningGame();
        if (st == Link::State::Connected && (running != sentGame || conn != sentConn))
        {
            sentGame = running;
            sentConn = conn;
            Link::Session::Get().SendSession(running);
        }
    }

    // after every (re)connect: tell the 3DS which games are on the PC
    static uint32_t syncedConnection = 0;
    uint32_t conn = Link::Get().ConnectionCount();
    if (conn != syncedConnection && Link::Get().GetState() == Link::State::Connected)
    {
        syncedConnection = conn;
        Link::Session::Get().RequestOnPcSync();
    }

    // a screenshot asked for on the 3DS: the emulator thread takes it while a
    // game runs; without one there is nothing to take
    if (!emuThread->emuIsActive() && Link::Get().TakeScreenshotRequest())
    {
        std::thread([] { Link::Get().SetStatusText(Link::Get().Tr3DS("shot.nogame")); }).detach();
    }

    std::string rom = Link::Session::Get().Tick(this, localCfg.GetString("SaveFilePath"));
    if (rom.empty()) return;

    QStringList file{QString::fromStdString(rom)};
    QString errorstr;
    if (!emuThread->bootROM(file, errorstr))
    {
        Link::Session::Get().SendSession(linkRunningGame(), true);
        QMessageBox::critical(this, "melonDS Link", errorstr);
        return;
    }
    Link::Session::Get().SetActiveSave(emuInstance->ndsSave ? emuInstance->ndsSave->GetPath() : "");

    QString filename = file.join('|');
    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();
    updateCartInserted(false);
}

void MainWindow::onLinkDisconnect()
{
    Link::Get().Disconnect();
    if (Link::NoteShown(Link::Note::Connection)) emuInstance->osdAddMessage(0, "%s", Link::TrS("connect.disconnected").c_str());
}

void MainWindow::onNPStartHost()
{
    //Netplay::StartHost();
    //NetplayStartHostDialog::openDlg(this);
}

void MainWindow::onNPStartClient()
{
    //Netplay::StartClient();
    //NetplayStartClientDialog::openDlg(this);
}

void MainWindow::onNPTest()
{
    // HAX
    //Netplay::StartGame();
}

void MainWindow::updateMPInterface(MPInterfaceType type)
{
    if (!hasMenu) return;

    // MP interface was changed, reflect it in the UI

    bool enable = (type == MPInterface_Local);
    actMPNewInstance->setEnabled(enable);
    actLANStartHost->setEnabled(enable);
    actLANStartClient->setEnabled(enable);
    /*actNPStartHost->setEnabled(enable);
    actNPStartClient->setEnabled(enable);
    actNPTest->setEnabled(enable);*/
}

bool MainWindow::lanWarning(bool host)
{
    if (numEmuInstances() < 2)
        return true;

    QString verb = host ? "host" : "join";
    QString msg = "Multiple emulator instances are currently open.\n"
            "If you "+verb+" a LAN game now, all secondary instances will be closed.\n\n"
            "Do you wish to continue?";

    auto res = QMessageBox::warning(this, "melonDS", msg, QMessageBox::Yes|QMessageBox::No, QMessageBox::No);
    if (res == QMessageBox::No)
        return false;

    deleteAllEmuInstances(1);
    return true;
}

void MainWindow::onOpenEmuSettings()
{
    emuThread->emuPause();

    EmuSettingsDialog* dlg = EmuSettingsDialog::openDlg(this);
    connect(dlg, &EmuSettingsDialog::finished, this, &MainWindow::onEmuSettingsDialogFinished);
}

void MainWindow::onEmuSettingsDialogFinished(int res)
{
    if (globalCfg.GetInt("Emu.ConsoleType") == 1)
    {
        actInsertGBACart->setEnabled(false);
        for (auto act : actInsertGBAAddon)
            act->setEnabled(false);
        actEjectGBACart->setEnabled(false);
    }
    else
    {
        actInsertGBACart->setEnabled(true);
        for (auto act : actInsertGBAAddon)
            act->setEnabled(true);
        actEjectGBACart->setEnabled(emuInstance->gbaCartInserted());
    }

    if (EmuSettingsDialog::needsReset)
        onReset();

    actCurrentGBACart->setText("GBA slot: " + emuInstance->gbaCartLabel());

    if (!emuThread->emuIsActive())
        actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());

    emuThread->emuUnpause();
}

void MainWindow::onOpenInputConfig()
{
    emuThread->emuPause();

    InputConfigDialog* dlg = InputConfigDialog::openDlg(this);
    connect(dlg, &InputConfigDialog::finished, this, &MainWindow::onInputConfigFinished);
}

void MainWindow::onInputConfigFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onOpenVideoSettings()
{
    VideoSettingsDialog* dlg = VideoSettingsDialog::openDlg(this);
    connect(dlg, &VideoSettingsDialog::updateVideoSettings, this, &MainWindow::onUpdateVideoSettings);
}

void MainWindow::onOpenCameraSettings()
{
    emuThread->emuPause();

    camStarted[0] = camManager[0]->isStarted();
    camStarted[1] = camManager[1]->isStarted();
    if (camStarted[0]) camManager[0]->stop();
    if (camStarted[1]) camManager[1]->stop();

    CameraSettingsDialog* dlg = CameraSettingsDialog::openDlg(this);
    connect(dlg, &CameraSettingsDialog::finished, this, &MainWindow::onCameraSettingsFinished);
}

void MainWindow::onCameraSettingsFinished(int res)
{
    if (camStarted[0]) camManager[0]->start();
    if (camStarted[1]) camManager[1]->start();

    emuThread->emuUnpause();
}

void MainWindow::onOpenAudioSettings()
{
    AudioSettingsDialog* dlg = AudioSettingsDialog::openDlg(this);
    connect(emuThread, &EmuThread::syncVolumeLevel, dlg, &AudioSettingsDialog::onSyncVolumeLevel);
    connect(emuThread, &EmuThread::windowEmuStart, dlg, &AudioSettingsDialog::onConsoleReset);
    connect(dlg, &AudioSettingsDialog::updateAudioVolume, this, &MainWindow::onUpdateAudioVolume);
    connect(dlg, &AudioSettingsDialog::updateAudioSettings, this, &MainWindow::onUpdateAudioSettings);
    connect(dlg, &AudioSettingsDialog::finished, this, &MainWindow::onAudioSettingsFinished);
}

void MainWindow::onOpenFirmwareSettings()
{
    emuThread->emuPause();

    FirmwareSettingsDialog* dlg = FirmwareSettingsDialog::openDlg(this);
    connect(dlg, &FirmwareSettingsDialog::finished, this, &MainWindow::onFirmwareSettingsFinished);
}

void MainWindow::onFirmwareSettingsFinished(int res)
{
    if (FirmwareSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onOpenPathSettings()
{
    emuThread->emuPause();

    PathSettingsDialog* dlg = PathSettingsDialog::openDlg(this);
    connect(dlg, &PathSettingsDialog::finished, this, &MainWindow::onPathSettingsFinished);
}

void MainWindow::onPathSettingsFinished(int res)
{
    if (PathSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onUpdateAudioVolume(int vol, int dsisync)
{
    emuInstance->audioVolume = vol;
    emuInstance->audioDSiVolumeSync = dsisync;
}

void MainWindow::onUpdateAudioSettings()
{
    if (!emuThread->emuIsActive()) return;
    assert(emuInstance->nds != nullptr);

    int interp = globalCfg.GetInt("Audio.Interpolation");
    emuInstance->nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(interp));

    int bitdepth = globalCfg.GetInt("Audio.BitDepth");
    if (bitdepth == 0)
        emuInstance->nds->SPU.SetDegrade10Bit(emuInstance->nds->ConsoleType == 0);
    else
        emuInstance->nds->SPU.SetDegrade10Bit(bitdepth == 1);
}

void MainWindow::onAudioSettingsFinished(int res)
{
    emuInstance->audioUpdateSettings();
}

void MainWindow::onOpenMPSettings()
{
    emuThread->emuPause();

    MPSettingsDialog* dlg = MPSettingsDialog::openDlg(this);
    connect(dlg, &MPSettingsDialog::finished, this, &MainWindow::onMPSettingsFinished);
}

void MainWindow::onMPSettingsFinished(int res)
{
    emuInstance->mpAudioMode = globalCfg.GetInt("MP.AudioMode");
    emuInstance->updateAudioMuteByWindowFocus();
    MPInterface::Get().SetRecvTimeout(globalCfg.GetInt("MP.RecvTimeout"));

    emuThread->emuUnpause();
}

void MainWindow::onOpenWifiSettings()
{
    emuThread->emuPause();

    WifiSettingsDialog* dlg = WifiSettingsDialog::openDlg(this);
    connect(dlg, &WifiSettingsDialog::finished, this, &MainWindow::onWifiSettingsFinished);
}

void MainWindow::onWifiSettingsFinished(int res)
{
    if (WifiSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onOpenInterfaceSettings()
{
    emuThread->emuPause();
    InterfaceSettingsDialog* dlg = InterfaceSettingsDialog::openDlg(this);
    connect(dlg, &InterfaceSettingsDialog::finished, this, &MainWindow::onInterfaceSettingsFinished);
    connect(dlg, &InterfaceSettingsDialog::updateInterfaceSettings, this, &MainWindow::onUpdateInterfaceSettings);
}

void MainWindow::onUpdateInterfaceSettings()
{
    pauseOnLostFocus = globalCfg.GetBool("PauseLostFocus");
    emuInstance->targetFPS = globalCfg.GetDouble("TargetFPS");
    emuInstance->fastForwardFPS = globalCfg.GetDouble("FastForwardFPS");
    emuInstance->slowmoFPS = globalCfg.GetDouble("SlowmoFPS");
    applyMouseHide();
}

void MainWindow::onInterfaceSettingsFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onChangeScreenSize()
{
    int factor = ((QAction*)sender())->data().toInt();
    QSize diff = size() - panel->size();
    resize(panel->screenGetMinSize(factor) + diff);
}

void MainWindow::onChangeScreenRotation(QAction* act)
{
    int rot = act->data().toInt();
    windowCfg.SetInt("ScreenRotation", rot);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenGap(QAction* act)
{
    int gap = act->data().toInt();
    windowCfg.SetInt("ScreenGap", gap);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenLayout(QAction* act)
{
    int layout = act->data().toInt();
    windowCfg.SetInt("ScreenLayout", layout);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSwap(bool checked)
{
    windowCfg.SetBool("ScreenSwap", checked);

    // Swap between top and bottom screen when displaying one screen.
    int sizing = windowCfg.GetInt("ScreenSizing");
    if (sizing == screenSizing_TopOnly)
    {
        // Bottom Screen.
        sizing = screenSizing_BotOnly;
        actScreenSizing[screenSizing_TopOnly]->setChecked(false);
        actScreenSizing[sizing]->setChecked(true);
    }
    else if (sizing == screenSizing_BotOnly)
    {
        // Top Screen.
        sizing = screenSizing_TopOnly;
        actScreenSizing[screenSizing_BotOnly]->setChecked(false);
        actScreenSizing[sizing]->setChecked(true);
    }
    windowCfg.SetInt("ScreenSizing", sizing);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSizing(QAction* act)
{
    int sizing = act->data().toInt();
    windowCfg.SetInt("ScreenSizing", sizing);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenAspect(QAction* act)
{
    int aspect = act->data().toInt();
    QActionGroup* group = act->actionGroup();

    if (group == grpScreenAspectTop)
    {
        windowCfg.SetInt("ScreenAspectTop", aspect);
    }
    else
    {
        windowCfg.SetInt("ScreenAspectBot", aspect);
    }

    emit screenLayoutChange();
}

void MainWindow::onChangeIntegerScaling(bool checked)
{
    windowCfg.SetBool("IntegerScaling", checked);

    emit screenLayoutChange();
}

void MainWindow::onOpenNewWindow()
{
    emuInstance->createWindow();
}

void MainWindow::onChangeScreenFiltering(bool checked)
{
    windowCfg.SetBool("ScreenFilter", checked);

    //emit screenLayoutChange();
    panel->setFilter(checked);
}

void MainWindow::onChangeShowOSD(bool checked)
{
    showOSD = checked;
    panel->osdSetEnabled(showOSD);
    windowCfg.SetBool("ShowOSD", showOSD);
}

void MainWindow::onChangeLimitFramerate(bool checked)
{
    emuInstance->doLimitFPS = checked;
    globalCfg.SetBool("LimitFPS", emuInstance->doLimitFPS);
}

void MainWindow::onChangeAudioSync(bool checked)
{
    emuInstance->doAudioSync = checked;
    globalCfg.SetBool("AudioSync", emuInstance->doAudioSync);
}


void MainWindow::onTitleUpdate(QString title)
{
    if (!emuInstance) return;

    int numinst = numEmuInstances();
    int numwin = emuInstance->getNumWindows();
    if ((numinst > 1) && (numwin > 1))
    {
        // add player/window prefix
        QString prefix = QString("[p%1:w%2] ").arg(emuInstance->instanceID+1).arg(windowID+1);
        title = prefix + title;
    }
    else if (numinst > 1)
    {
        // add player prefix
        QString prefix = QString("[p%1] ").arg(emuInstance->instanceID+1);
        title = prefix + title;
    }
    else if (numwin > 1)
    {
        // add window prefix
        QString prefix = QString("[w%1] ").arg(windowID+1);
        title = prefix + title;
    }

    setWindowTitle(title);
}

void MainWindow::toggleFullscreen()
{
    if (!isFullScreen())
    {
        maximizedBeforeFullscreen = isMaximized();
        showFullScreen();
        if (hasMenu)
            menuBar()->setFixedHeight(0); // Don't use hide() as menubar actions stop working
    }
    else
    {
        // back to how the window was before (maximized or normal)
        if (maximizedBeforeFullscreen) showMaximized();
        else showNormal();
        if (hasMenu)
        {
            int menuBarHeight = menuBar()->sizeHint().height();
            menuBar()->setFixedHeight(menuBarHeight);
        }
    }
    if (actLeaveFullscreen) actLeaveFullscreen->setEnabled(isFullScreen());
    applyMouseHide();
}

void MainWindow::applyMouseHide()
{
    if (!panel) return;
    // in fullscreen the pointer disappears after 1.5 s without moving
    // (nothing static on the screen, e.g. for OLED monitors)
    if (isFullScreen())
        panel->setCursorAutoHide(true, 1500);
    else
        panel->setCursorAutoHide(globalCfg.GetBool("Mouse.Hide"), globalCfg.GetInt("Mouse.HideSeconds") * 1000);
}

void MainWindow::onFullscreenToggled()
{
    toggleFullscreen();
}

void MainWindow::onScreenEmphasisToggled()
{
    int currentSizing = windowCfg.GetInt("ScreenSizing");
    if (currentSizing == screenSizing_EmphTop)
    {
        currentSizing = screenSizing_EmphBot;
    }
    else if (currentSizing == screenSizing_EmphBot)
    {
        currentSizing = screenSizing_EmphTop;
    }
    else
    {
        // For any other sizing mode, switch to EmphTop as a sensible default
        currentSizing = screenSizing_EmphTop;
    }
    windowCfg.SetInt("ScreenSizing", currentSizing);
    actScreenSizing[currentSizing]->setChecked(true);

    emit screenLayoutChange();
}

void MainWindow::onEmuStart()
{
    if (!hasMenu) return;

    for (int i = 1; i < 9; i++)
    {
        actSaveState[i]->setEnabled(true);
        actLoadState[i]->setEnabled(emuInstance->savestateExists(i));
    }
    actSaveState[0]->setEnabled(true);
    actLoadState[0]->setEnabled(true);
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(true);
    actPause->setChecked(false);
    actReset->setEnabled(true);
    actStop->setEnabled(true);
    actFrameStep->setEnabled(true);

    //actDateTime->setEnabled(false);
    actPowerManagement->setEnabled(true);

    actTitleManager->setEnabled(false);
}

void MainWindow::onEmuStop()
{
    if (!hasMenu) return;

    for (int i = 0; i < 9; i++)
    {
        actSaveState[i]->setEnabled(false);
        actLoadState[i]->setEnabled(false);
    }
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(false);
    actReset->setEnabled(false);
    actStop->setEnabled(false);
    actFrameStep->setEnabled(false);

    //actDateTime->setEnabled(true);
    actPowerManagement->setEnabled(false);

    actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());
}

void MainWindow::onEmuPause(bool pause)
{
    if (!hasMenu) return;

    actPause->setChecked(pause);
}

void MainWindow::onEmuReset()
{
    if (!hasMenu) return;

    actUndoStateLoad->setEnabled(false);
}

void MainWindow::onUpdateVideoSettings(bool glchange)
{
    if (!emuInstance) return;

    // if we have a parent window: pass the message over to the parent
    // the topmost parent takes care of updating all the windows
    MainWindow* parentwin = (MainWindow*)parentWidget();
    if (parentwin)
        return parentwin->onUpdateVideoSettings(glchange);

    auto childwins = findChildren<MainWindow *>(nullptr);

    bool hadOGL = hasOGL;
    if (glchange)
    {
        emuThread->emuPause();
        if (hadOGL)
        {
            emuThread->deinitContext(windowID);
            for (auto child: childwins)
            {
                auto thread = child->getEmuInstance()->getEmuThread();
                thread->deinitContext(child->windowID);
            }
        }

        createScreenPanel();
        for (auto child: childwins)
        {
            child->createScreenPanel();
        }
    }

    emuThread->updateVideoSettings();
    for (auto child: childwins)
    {
        // child windows may belong to a different instance
        // in that case we need to signal their thread appropriately
        auto thread = child->getEmuInstance()->getEmuThread();
        if (child->getWindowID() == 0)
            thread->updateVideoSettings();
    }

    if (glchange)
    {
        if (hasOGL) 
        {
            emuThread->initContext(windowID);
            for (auto child: childwins)
            {
                auto thread = child->getEmuInstance()->getEmuThread();
                thread->initContext(child->windowID);
            }
        }
    }

    if (glchange)
    {
        emuThread->emuUnpause();
    }
}
