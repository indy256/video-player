#pragma once
#include <QObject>

class QMediaPlayer;

// Prepare a Windows output endpoint during initial decoding and keep it open on seeks.
void installAudioKeepAlive(QMediaPlayer *player);
