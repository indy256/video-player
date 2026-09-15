#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <memory>

class SingleInstance : public QObject {
    Q_OBJECT
public:
    enum class Result { Primary, Forwarded, Failed };
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);
    Result start(const QString &file);
    QString errorString() const { return error; }
signals:
    void openRequested(const QString &file);
private:
    QString name;
    QString error;
    std::unique_ptr<QLockFile> lock;
    QLocalServer server;
};
