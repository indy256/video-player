#include "folderpanel.h"
#include "fileassociations.h"
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPainter>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QRegion>
#include <QSignalBlocker>
#include <QThread>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <memory>
#include <atomic>

namespace {
constexpr int thumbnailReady = Qt::UserRole + 1;
const QSize thumbnailSize(128, 72);

class SelectionCheckBox : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    bool hitButton(const QPoint &position) const override {
        return rect().contains(position);
    }
    void paintEvent(QPaintEvent *) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.rect = QRect(width() - 24, 6, 18, 18);
        QStylePainter painter(this);
        painter.drawPrimitive(QStyle::PE_IndicatorCheckBox, option);
    }
};

class VideoDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &) const override {
        auto *checkbox = new SelectionCheckBox(parent);
        checkbox->setFocusPolicy(Qt::NoFocus);
        checkbox->setAccessibleName("Select video for saving");
        checkbox->setToolTip("Select video for saving; click the thumbnail or filename to play");
        checkbox->setStyleSheet("QCheckBox { background: transparent; spacing: 0px; }"
                               "QCheckBox::indicator { width: 18px; height: 18px; }");
        connect(checkbox, &QCheckBox::toggled, this, [this, checkbox] {
            emit const_cast<VideoDelegate *>(this)->commitData(checkbox);
        });
        return checkbox;
    }
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override {
        QRect selectionArea = option.rect;
        selectionArea.setLeft(selectionArea.left() + selectionArea.width() / 2);
        editor->setGeometry(selectionArea);
        QStyleOptionViewItem content(option);
        initStyleOption(&content, index);
        const QRect textArea = option.widget->style()->subElementRect(
            QStyle::SE_ItemViewItemText, &content, option.widget);
        // Let filename clicks reach the list's normal playback handler.
        const QRegion filename(textArea.translated(-selectionArea.topLeft()));
        editor->setMask(QRegion(editor->rect()).subtracted(filename));
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        auto *checkbox = static_cast<QCheckBox *>(editor);
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(index.data(Qt::CheckStateRole).toInt() == Qt::Checked);
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override {
        model->setData(index, static_cast<QCheckBox *>(editor)->isChecked()
                       ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
    }
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->features &= ~QStyleOptionViewItem::HasCheckIndicator;
        option->rect.adjust(0, 0, -28, 0);
    }
};

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
    files->setItemDelegate(new VideoDelegate(files));
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
    files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(files, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *clicked = files->itemAt(position);
        QStringList paths = selectedFiles();
        if (paths.isEmpty() && clicked) paths.append(clicked->data(Qt::UserRole).toString());
        emit contextMenuRequested(files->viewport()->mapToGlobal(position), paths);
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
    const QStringList checked = selectedFiles();
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
        item->setCheckState(checked.contains(file.absoluteFilePath()) ? Qt::Checked : Qt::Unchecked);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        item->setToolTip(file.fileName());
        item->setSizeHint(QSize(280, 84));
        files->openPersistentEditor(item);
    }
    if (!files->count()) title->setText("No videos in this folder");
    selectCurrent();
    thumbnailTimer->start();
}

QStringList FolderPanel::selectedFiles() const {
    QStringList paths;
    for (int row = 0; row < files->count(); ++row) {
        auto *item = files->item(row);
        if (item->checkState() == Qt::Checked) paths.append(item->data(Qt::UserRole).toString());
    }
    return paths;
}

void FolderPanel::saveVideos(const QStringList &paths) {
    if (paths.isEmpty()) return;
    const QString destination = QFileDialog::getExistingDirectory(this, "Save videos to...");
    if (destination.isEmpty()) return;
    auto *progress = new QProgressDialog("Copying videos... Cancel stops after the current file.",
                                         "Cancel", 0, 0, this);
    progress->setWindowTitle("Save videos");
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    auto errors = std::make_shared<QStringList>();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, progress, [canceled] { *canceled = true; });
    // QFile::copy refuses existing destinations, including the original file.
    // Run disk I/O off the UI thread so playback stays responsive.
    auto *job = QThread::create([paths, destination, errors, canceled] {
        const QDir target(destination);
        for (const auto &path : paths) {
            if (*canceled) break;
            QFile source(path);
            const QString name = QFileInfo(path).fileName();
            if (!source.copy(target.filePath(name))) errors->append(name + ": " + source.errorString());
        }
    });
    connect(job, &QThread::finished, progress, [progress, errors] {
        progress->hide();
        if (!errors->isEmpty())
            QMessageBox::warning(progress->parentWidget(), "Some videos could not be saved", errors->join('\n'));
        progress->deleteLater();
    });
    connect(job, &QThread::finished, job, &QObject::deleteLater);
    job->start();
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
