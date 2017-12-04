// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QWidget>
#include <QWinThumbnailToolBar>
#include <QWinThumbnailToolButton>

class WindowsExtras : public QObject {
    Q_OBJECT

public:
    void Show();
    void UpdatePlay();
    void UpdatePause();
    void UpdateStop();

    WindowsExtras(QWidget* parent);
    ~WindowsExtras();

private:
    void InitializeThumbnailToolbar();

signals:
    void ClickPlayPause();
    void ClickStop();
    void ClickRestart();

private:
    QWinThumbnailToolBar* thumbbar = nullptr;
    QWinThumbnailToolButton* play_pause = nullptr;
    QWinThumbnailToolButton* stop = nullptr;
    QWinThumbnailToolButton* restart = nullptr;

    QWidget* parent = nullptr;
};
