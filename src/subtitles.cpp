#include "subtitles.h"
#include <QFile>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringList>
#include <QTextDocumentFragment>
#include <algorithm>

bool SubtitleTrack::load(const QString &path, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    const QByteArray bytes = file.readAll();
    QStringDecoder decoder(QStringDecoder::encodingForData(bytes).value_or(QStringDecoder::Utf8));
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) { error = "Save the subtitle file as UTF-8 or UTF-16."; return false; }
    text.replace("\r\n", "\n").replace('\r', '\n');
    const QStringList lines = text.split('\n');
    static const QRegularExpression timing(
        R"(^\s*(\d+):([0-5]\d):([0-5]\d)[,.](\d{3})\s*-->\s*(\d+):([0-5]\d):([0-5]\d)[,.](\d{3})(?:\s.*)?$)");
    static const QRegularExpression formatting(R"(</?(?:i|b|u|font)(?:\s[^>]*)?>)",
        QRegularExpression::CaseInsensitiveOption);
    QVector<Cue> parsed;
    for (int i = 0; i < lines.size(); ++i) {
        const auto match = timing.match(lines[i]);
        if (!match.hasMatch()) continue;
        const auto milliseconds = [&match](int offset) {
            return ((match.captured(offset).toLongLong() * 60 + match.captured(offset + 1).toInt()) * 60
                + match.captured(offset + 2).toInt()) * 1000 + match.captured(offset + 3).toInt();
        };
        const qint64 start = milliseconds(1), end = milliseconds(5);
        QStringList body;
        while (++i < lines.size() && !lines[i].trimmed().isEmpty()) body.append(lines[i]);
        // Preserve line breaks while removing SRT formatting tags and decoding entities.
        QString caption = body.join('\n').remove(formatting);
        caption.replace("<", "&lt;").replace(">", "&gt;").replace('\n', "<br>");
        caption = QTextDocumentFragment::fromHtml(caption).toPlainText().trimmed();
        if (end > start && !caption.isEmpty()) parsed.append({start, end, caption});
    }
    if (parsed.isEmpty()) { error = "No valid SRT subtitle cues were found."; return false; }
    std::stable_sort(parsed.begin(), parsed.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
    cues = std::move(parsed);
    this->path = path;
    error.clear();
    return true;
}

QString SubtitleTrack::textAt(qint64 position) const {
    QStringList active;
    for (const auto &cue : cues) {
        if (cue.start > position) break;
        if (position < cue.end) active.append(cue.text);
    }
    return active.join('\n');
}
