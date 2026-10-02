#include "folderpanel.h"
#include "fileassociations.h"
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>

namespace {
constexpr int thumbnailReady = Qt::UserRole + 1;
const QSize thumbnailSize(128, 72);

qint64 previewPosition(const QMediaPlayer *player) {
    return player->duration() / 2;
}

QString thumbnailKey(const QFileInfo &file) {
    return file.absoluteFilePath() + '\n' + QString::number(file.size()) + '\n'
        + QString::number(file.lastModified().toMSecsSinceEpoch());
}

QPixmap placeholder() {
    QPixmap image(thumbnailSize);
    image.fill(QColor("#242b3b"));
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#939eb4"));
    painter.drawPolygon(QPolygon({QPoint(57, 25), QPoint(57, 47), QPoint(75, 36)}));
    return image;
}
}

FolderPanel::FolderPanel(QWidget *parent) : QWidget(parent) {
    setObjectName("folderPanel");
    setFixedWidth(320);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    title = new QLabel;
    title->setTextFormat(Qt::PlainText);
    files = new QListWidget;
    files->setObjectName("folderFiles");
    files->setAccessibleName("Videos in current folder");
    files->setFocusPolicy(Qt::NoFocus);
    files->setIconSize(thumbnailSize);
    files->setSpacing(3);
    files->setWordWrap(true);
    files->setTextElideMode(Qt::ElideRight);
    files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    files->setStyleSheet("QListWidget { background: #151923; border: none; }"
                         "QListWidget::item { padding: 4px; color: #e4eaf8; }"
                         "QListWidget::item:selected { background: #33415b; }"
                         "QListWidget::item:hover { background: #242b3b; }");
    layout->addWidget(title);
    layout->addWidget(files);
    connect(files, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        emit fileSelected(item->data(Qt::UserRole).toString());
    });
    thumbnailTimer = new QTimer(this);
    thumbnailTimer->setSingleShot(true);
    connect(thumbnailTimer, &QTimer::timeout, this, &FolderPanel::loadNextThumbnail);
    connect(files->verticalScrollBar(), &QScrollBar::valueChanged, thumbnailTimer, qOverload<>(&QTimer::start));
}

void FolderPanel::setFile(const QString &path) {
    currentFile = QFileInfo(path).absoluteFilePath();
    if (!isVisible()) return;
    if (QFileInfo(currentFile).absolutePath() != directory) refresh();
    else selectCurrent();
}

void FolderPanel::selectCurrent() {
    for (int row = 0; row < files->count(); ++row) {
        auto *item = files->item(row);
        if (item->data(Qt::UserRole).toString() == currentFile) {
            files->setCurrentItem(item);
            files->scrollToItem(item);
            break;
        }
    }
}

void FolderPanel::refresh() {
    cancelThumbnail();
    files->clear();
    directory = currentFile.isEmpty() ? QDir::currentPath() : QFileInfo(currentFile).absolutePath();
    title->setText(QDir(directory).dirName());
    title->setToolTip(directory);
    const QIcon fallback(placeholder());
    const auto extensions = FileAssociations::extensions();
    for (const auto &file : QDir(directory).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase)) {
        if (!extensions.contains(file.suffix().toLower())) continue;
        auto *item = new QListWidgetItem(fallback, file.fileName(), files);
        item->setData(Qt::UserRole, file.absoluteFilePath());
        item->setToolTip(file.fileName());
        item->setSizeHint(QSize(280, 84));
    }
    if (!files->count()) title->setText("No videos in this folder");
    selectCurrent();
    thumbnailTimer->start();
}

void FolderPanel::cancelThumbnail() {
    // Disconnect immediately: already queued frames must not touch rebuilt list items.
    thumbnailTimer->stop();
    delete thumbnailJob;
    thumbnailJob = nullptr;
}

void FolderPanel::loadNextThumbnail() {
    if (!isVisible() || thumbnailJob) return;
    for (int row = 0; row < files->count(); ++row) {
        auto *item = files->item(row);
        if (item->data(thumbnailReady).toBool() || !files->visualItemRect(item).intersects(files->viewport()->rect())) continue;
        const QFileInfo file(item->data(Qt::UserRole).toString());
        const QString key = thumbnailKey(file);
        if (const auto *cached = thumbnails.object(key)) {
            item->setIcon(*cached);
            item->setData(thumbnailReady, true);
            continue;
        }
        startThumbnail(item, key);
        return;
    }
}

void FolderPanel::startThumbnail(QListWidgetItem *item, const QString &key) {
    // The decoder owns all per-file work, so cancellation is a single deletion.
    auto *decoder = new QMediaPlayer(this);
    thumbnailJob = decoder;
    auto *sink = new QVideoSink(decoder);
    decoder->setVideoSink(sink); // No audio output: browsing never produces sound.
    const auto finish = [this, decoder, item, key](const QImage &image) {
        if (thumbnailJob != decoder) return;
        thumbnailJob = nullptr;
        item->setData(thumbnailReady, true);
        if (!image.isNull()) {
            QPixmap preview(thumbnailSize);
            preview.fill(Qt::black);
            QPainter painter(&preview);
            const QImage scaled = image.scaled(thumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawImage((preview.width() - scaled.width()) / 2, (preview.height() - scaled.height()) / 2, scaled);
            painter.end();
            thumbnails.insert(key, new QPixmap(preview));
            item->setIcon(preview);
        }
        decoder->stop();
        decoder->deleteLater();
        thumbnailTimer->start();
    };
    connect(decoder, &QMediaPlayer::mediaStatusChanged, decoder, [decoder, started = false](QMediaPlayer::MediaStatus status) mutable {
        if (started || status != QMediaPlayer::LoadedMedia) return;
        started = true;
        decoder->setActiveAudioTrack(-1);
        decoder->setActiveSubtitleTrack(-1);
        decoder->setPosition(previewPosition(decoder));
        decoder->play();
    });
    connect(sink, &QVideoSink::videoFrameChanged, decoder, [finish, decoder](const QVideoFrame &frame) {
        if (frame.isValid() && frame.startTime() / 1000 >= previewPosition(decoder) - 100) finish(frame.toImage());
    });
    connect(decoder, &QMediaPlayer::errorOccurred, decoder, [finish] { finish({}); });
    QTimer::singleShot(5000, decoder, [finish] { finish({}); });
    decoder->setSource(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
}

void FolderPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    refresh();
}

void FolderPanel::hideEvent(QHideEvent *event) {
    cancelThumbnail();
    QWidget::hideEvent(event);
}

void FolderPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    thumbnailTimer->start();
}

void FolderPanel::wheelEvent(QWheelEvent *event) {
    // Consume wheel events the list cannot scroll, including at its boundaries.
    event->accept();
}
