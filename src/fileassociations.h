#pragma once

#include <QString>
#include <QStringList>

class QSettings;

namespace FileAssociations {
QStringList extensions();
QString executablePath();
// Register this installation as an available handler. An empty result means success.
QString registerFileTypes();
// Separate serialization from installation so tests can use an isolated registry/file.
QString writeWindowsRegistration(QSettings &registry, const QString &executable);
QByteArray desktopEntry(const QString &executable);
}
