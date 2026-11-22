#pragma once
#include <QObject>
#include <QList>
#include <QUrl>
#include <optional>

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
    std::optional<qint64> indexedPosition(qint64 requested, bool forward = false) const;
    qint64 seekPosition(qint64 requested, bool forward = false) const {
        return indexedPosition(requested, forward).value_or(requested);
    }
signals:
    void finished(bool available);
private:
    QUrl source;
    QList<qint64> positions;
    qint64 readBytes = 0;
    quint64 generation = 0;
    bool started = false;
};
