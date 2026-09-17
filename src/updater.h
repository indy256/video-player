#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>

class QWidget;

namespace AppUpdate {
struct Asset {
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
};
// Reject incomplete releases and downloads outside this repository.
bool releaseAsset(const QJsonObject &release, const QString &name, Asset &asset);
// Returns true once the installer is ready; the caller must save state and exit.
bool installLatest(QWidget *parent, const QString &videoPath);
}
