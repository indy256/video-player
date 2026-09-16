#include "singleinstance.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent), name(name), lock(QDir::temp().filePath(name + ".lock")) {
    // A live player may run indefinitely; only reclaim locks from dead processes.
    lock.setStaleLockTime(0);
    server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = server.nextPendingConnection()) {
            socket->write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
            auto *timeout = new QTimer(socket);
            timeout->setSingleShot(true);
            connect(timeout, &QTimer::timeout, socket, &QLocalSocket::abort);
            timeout->start(5000);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, timeout] {
                if (socket->property("handled").toBool()) return;
                if (socket->bytesAvailable() > 1024 * 1024) { socket->abort(); return; }
                if (!socket->canReadLine()) return;
                socket->setProperty("handled", true);
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(socket->readLine(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()
                    || !document.object().value("file").isString()) { socket->abort(); return; }
                timeout->stop();
                emit openRequested(document.object().value("file").toString());
                socket->write("OK\n");
                socket->disconnectFromServer();
            });
        }
    });
}

SingleInstance::Result SingleInstance::start(const QString &file) {
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 5000) {
        if (lock.tryLock()) {
            QLocalServer::removeServer(name);
            if (server.listen(name)) return Result::Primary;
            error = server.errorString();
            lock.unlock();
            return Result::Failed;
        }
        if (lock.error() != QLockFile::LockFailedError) {
            error = "Cannot create the player instance lock.";
            return Result::Failed;
        }
        QLocalSocket socket;
        socket.connectToServer(name);
        if (!socket.waitForConnected(100)) {
            QThread::msleep(25);
            continue;
        }
        const auto readLine = [&]() -> QByteArray {
            while (!socket.canReadLine()) {
                const int remaining = qMax(0, 5000 - int(deadline.elapsed()));
                if (!remaining || !socket.waitForReadyRead(remaining)) return {};
            }
            return socket.readLine().trimmed();
        };
        const auto pid = readLine().toULongLong();
        if (!pid) break;
#ifdef Q_OS_WIN
        // Let the already-running process activate its window after a shell launch.
        AllowSetForegroundWindow(static_cast<DWORD>(pid));
#endif
        socket.write(QJsonDocument(QJsonObject{{"file", file}}).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
        if (readLine() == "OK") return Result::Forwarded;
        break;
    }
    error = "The running video player did not respond. Please try again.";
    return Result::Failed;
}
