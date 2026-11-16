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

#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QtGlobal>

#include "types.h"
#include "Platform.h"
#include "Config.h"
#include "GPU.h"
#include "main.h"

#include "VideoSettingsDialog.h"
#include "ui_VideoSettingsDialog.h"


inline bool VideoSettingsDialog::UsesGL()
{
    auto& cfg = emuInstance->getGlobalConfig();
    return cfg.GetBool("Screen.UseGL") || (cfg.GetInt("3D.Renderer") != renderer3D_Software);
}

VideoSettingsDialog* VideoSettingsDialog::currentDlg = nullptr;

void VideoSettingsDialog::setEnabled()
{
    auto& cfg = emuInstance->getGlobalConfig();
    int renderer = cfg.GetInt("3D.Renderer");

    bool softwareRenderer = renderer == renderer3D_Software;
    ui->cbGLDisplay->setEnabled(softwareRenderer);
    ui->cbSoftwareThreaded->setEnabled(softwareRenderer);
    ui->cbxGLResolution->setEnabled(!softwareRenderer);
    ui->cbBetterPolygons->setEnabled(renderer == renderer3D_OpenGL);
    ui->cbxComputeHiResCoords->setEnabled(renderer == renderer3D_OpenGLCompute);
    updateTexFilterEnabled();
}

void VideoSettingsDialog::updateTexFilterEnabled()
{
    if (!cbxTexFilter) return;
    auto& cfg = emuInstance->getGlobalConfig();
    bool gl = cfg.GetInt("3D.Renderer") != renderer3D_Software;
    bool on = gl && cbxTexFilter->currentIndex() != 0;
    cbxTexFilter->setEnabled(gl);
    cbxTexFilterScale->setEnabled(on);
    cbTexFilterObjects->setEnabled(on);
}

VideoSettingsDialog::VideoSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::VideoSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto& cfg = emuInstance->getGlobalConfig();
    oldRenderer = cfg.GetInt("3D.Renderer");
    oldGLDisplay = cfg.GetBool("Screen.UseGL");
    oldVSync = cfg.GetBool("Screen.VSync");
    oldVSyncInterval = cfg.GetInt("Screen.VSyncInterval");
    oldSoftThreaded = cfg.GetBool("3D.Soft.Threaded");
    oldGLScale = cfg.GetInt("3D.GL.ScaleFactor");
    oldGLBetterPolygons = cfg.GetBool("3D.GL.BetterPolygons");
    oldHiresCoordinates = cfg.GetBool("3D.GL.HiresCoordinates");

    grp3DRenderer = new QButtonGroup(this);
    grp3DRenderer->addButton(ui->rb3DSoftware, renderer3D_Software);
    grp3DRenderer->addButton(ui->rb3DOpenGL,   renderer3D_OpenGL);
    grp3DRenderer->addButton(ui->rb3DCompute,  renderer3D_OpenGLCompute);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    connect(grp3DRenderer, SIGNAL(buttonClicked(int)), this, SLOT(onChange3DRenderer(int)));
#else
    connect(grp3DRenderer, SIGNAL(idClicked(int)), this, SLOT(onChange3DRenderer(int)));
#endif
    grp3DRenderer->button(oldRenderer)->setChecked(true);

#ifndef OGLRENDERER_ENABLED
    ui->rb3DOpenGL->setEnabled(false);
#endif

#ifdef __APPLE__
    ui->rb3DCompute->setEnabled(false);
#endif

    ui->cbGLDisplay->setChecked(oldGLDisplay != 0);

    ui->cbVSync->setChecked(oldVSync != 0);
    ui->sbVSyncInterval->setValue(oldVSyncInterval);

    ui->cbSoftwareThreaded->setChecked(oldSoftThreaded);

    for (int i = 1; i <= 16; i++)
        ui->cbxGLResolution->addItem(QString("%1x native (%2x%3)").arg(i).arg(256*i).arg(192*i));
    ui->cbxGLResolution->setCurrentIndex(oldGLScale-1);

    ui->cbBetterPolygons->setChecked(oldGLBetterPolygons != 0);
    ui->cbxComputeHiResCoords->setChecked(oldHiresCoordinates != 0);

    // texture filter (melonDS Link): magnifies the textures of the 3D graphics
    {
        oldTexFilter = cfg.GetInt("3D.GL.TextureFilter");
        oldTexFilterScale = cfg.GetInt("3D.GL.TextureFilterScale");
        if (oldTexFilterScale <= 0) oldTexFilterScale = 4;
        oldTexFilterObjects = cfg.GetBool("3D.GL.TextureFilterObjectsOnly");

        auto* label = new QLabel("Texture filter:", this);
        cbxTexFilter = new QComboBox(this);
        cbxTexFilter->addItems({"Off", "MMPX (keeps the pixel-art look)", "xBRZ (smooth)"});
        cbxTexFilter->setCurrentIndex(oldTexFilter >= 0 && oldTexFilter <= 2 ? oldTexFilter : 0);
        cbxTexFilter->setWhatsThis("Magnifies the textures of the 3D graphics (characters, Pokemon, trees...) with a "
                                   "pixel-art filter. Works with both OpenGL renderers.");
        cbxTexFilterScale = new QComboBox(this);
        cbxTexFilterScale->addItems({"2x", "4x"});
        cbxTexFilterScale->setCurrentIndex(oldTexFilterScale >= 4 ? 1 : 0);
        cbTexFilterObjects = new QCheckBox("Only characters and objects (textures with transparency)", this);
        cbTexFilterObjects->setChecked(oldTexFilterObjects);

        int row = ui->gridLayout_4->rowCount();
        ui->gridLayout_4->addWidget(label, row, 0);
        ui->gridLayout_4->addWidget(cbxTexFilter, row + 1, 0);
        ui->gridLayout_4->addWidget(cbxTexFilterScale, row + 2, 0);
        ui->gridLayout_4->addWidget(cbTexFilterObjects, row + 3, 0);

        connect(cbxTexFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx)
        {
            emuInstance->getGlobalConfig().SetInt("3D.GL.TextureFilter", idx);
            updateTexFilterEnabled();
            emit updateVideoSettings(false);
        });
        connect(cbxTexFilterScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx)
        {
            emuInstance->getGlobalConfig().SetInt("3D.GL.TextureFilterScale", idx == 1 ? 4 : 2);
            emit updateVideoSettings(false);
        });
        connect(cbTexFilterObjects, &QCheckBox::toggled, this, [this](bool on)
        {
            emuInstance->getGlobalConfig().SetBool("3D.GL.TextureFilterObjectsOnly", on);
            emit updateVideoSettings(false);
        });
    }

    if (!oldVSync)
        ui->sbVSyncInterval->setEnabled(false);
    setVsyncControlEnable(UsesGL());

    setEnabled();
}

VideoSettingsDialog::~VideoSettingsDialog()
{
    delete ui;
}

void VideoSettingsDialog::on_VideoSettingsDialog_accepted()
{
    Config::Save();

    closeDlg();
}

void VideoSettingsDialog::on_VideoSettingsDialog_rejected()
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        closeDlg();
        return;
    }

    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.Renderer", oldRenderer);
    cfg.SetBool("Screen.UseGL", oldGLDisplay);
    cfg.SetBool("Screen.VSync", oldVSync);
    cfg.SetInt("Screen.VSyncInterval", oldVSyncInterval);
    cfg.SetBool("3D.Soft.Threaded", oldSoftThreaded);
    cfg.SetInt("3D.GL.ScaleFactor", oldGLScale);
    cfg.SetBool("3D.GL.BetterPolygons", oldGLBetterPolygons);
    cfg.SetBool("3D.GL.HiresCoordinates", oldHiresCoordinates);
    cfg.SetInt("3D.GL.TextureFilter", oldTexFilter);
    cfg.SetInt("3D.GL.TextureFilterScale", oldTexFilterScale);
    cfg.SetBool("3D.GL.TextureFilterObjectsOnly", oldTexFilterObjects);

    emit updateVideoSettings(old_gl != UsesGL());

    closeDlg();
}

void VideoSettingsDialog::setVsyncControlEnable(bool hasOGL)
{
    ui->cbVSync->setEnabled(hasOGL);
    ui->sbVSyncInterval->setEnabled(hasOGL);
}

void VideoSettingsDialog::onChange3DRenderer(int renderer)
{
    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.Renderer", renderer);

    setEnabled();

    emit updateVideoSettings(old_gl != UsesGL());
}

void VideoSettingsDialog::on_cbGLDisplay_stateChanged(int state)
{
    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("Screen.UseGL", (state != 0));

    setVsyncControlEnable(UsesGL());

    emit updateVideoSettings(old_gl != UsesGL());
}

void VideoSettingsDialog::on_cbVSync_stateChanged(int state)
{
    bool vsync = (state != 0);
    ui->sbVSyncInterval->setEnabled(vsync);

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("Screen.VSync", vsync);

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_sbVSyncInterval_valueChanged(int val)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("Screen.VSyncInterval", val);

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbSoftwareThreaded_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.Soft.Threaded", (state != 0));

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbxGLResolution_currentIndexChanged(int idx)
{
    // prevent a spurious change
    if (ui->cbxGLResolution->count() < 16) return;

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.GL.ScaleFactor", idx+1);

    setVsyncControlEnable(UsesGL());

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbBetterPolygons_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.GL.BetterPolygons", (state != 0));

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbxComputeHiResCoords_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.GL.HiresCoordinates", (state != 0));

    emit updateVideoSettings(false);
}
