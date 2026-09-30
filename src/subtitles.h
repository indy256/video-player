#pragma once
#include <QString>
#include <QVector>

class SubtitleTrack {
public:
    bool load(const QString &path, QString &error);
    QString textAt(qint64 position) const;
    const QString &filePath() const { return path; }
    void clear() { cues.clear(); path.clear(); }
private:
    struct Cue { qint64 start, end; QString text; };
    QVector<Cue> cues;
    QString path;
};
