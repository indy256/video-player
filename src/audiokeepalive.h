#pragma once
#include <QObject>

class QMediaPlayer;

// Keep a Windows output endpoint running while Qt resets its media sink on seeks.
void installAudioKeepAlive(QMediaPlayer *player);
