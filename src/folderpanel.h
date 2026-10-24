#pragma once

#include <QCache>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

class QListWidget;
class QLabel;
class QMediaPlayer;
class QTimer;
class QListWidgetItem;

class FolderPanel : public QWidget {
    Q_OBJECT
public:
    explicit FolderPanel(QWidget *parent = nullptr);
    void setFile(const QString &path);
    void saveVideos(const QStringList &paths);
    QStringList selectedFiles() const;
signals:
    void fileSelected(const QString &path);
    void contextMenuRequested(const QPoint &position, const QStringList &paths);
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    void refresh();
    void selectCurrent();
    void loadNextThumbnail();
    void startThumbnail(QListWidgetItem *item, const QString &key);
    void cancelThumbnail();
    QListWidget *files;
    QLabel *title;
    QString currentFile;
    QString directory;
    QMediaPlayer *thumbnailJob = nullptr;
    QTimer *thumbnailTimer;
    QCache<QString, QPixmap> thumbnails{128};
};
