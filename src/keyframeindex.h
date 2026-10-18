#pragma once
#include <QObject>
#include <QList>
#include <QUrl>

// Packet indexing runs independently of the playback decoder, only when requested.
class KeyframeIndex : public QObject {
    Q_OBJECT
public:
    explicit KeyframeIndex(QObject *parent = nullptr) : QObject(parent) {}
    ~KeyframeIndex() override;
    void setSource(const QUrl &source);
    void load();
    bool isReady() const { return !positions.isEmpty(); }
    qint64 seekPosition(qint64 requested, bool forward = false) const;
signals:
    void finished(bool available);
private:
    QUrl source;
    QList<qint64> positions;
    quint64 generation = 0;
    bool started = false;
};
