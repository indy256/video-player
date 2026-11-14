#pragma once
#include <QObject>
#include <QList>
#include <QUrl>

// Read container seek tables independently of playback, with bounded I/O.
class KeyframeIndex : public QObject {
    Q_OBJECT
public:
    explicit KeyframeIndex(QObject *parent = nullptr) : QObject(parent) {}
    ~KeyframeIndex() override;
    void setSource(const QUrl &source);
    void load();
    bool isReady() const { return !positions.isEmpty(); }
    qint64 bytesRead() const { return readBytes; }
    qint64 seekPosition(qint64 requested, bool forward = false) const;
signals:
    void finished(bool available);
private:
    QUrl source;
    QList<qint64> positions;
    qint64 readBytes = 0;
    quint64 generation = 0;
    bool started = false;
};
