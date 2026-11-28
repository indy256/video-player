#include "videosurface.h"
#include "gammafilter.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QVBoxLayout>
#include <QVariant>
#include <QVideoSink>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QtMultimedia/private/qmultimediautils_p.h>
#include <QtMultimedia/private/qvideoframeconverter_p.h>
#include <QtMultimedia/private/qvideotexturehelper_p.h>
#endif
#include <memory>
#ifdef Q_OS_WIN
#include "windowsbackground.h"
#endif

namespace {
constexpr char softwareDecodingProperty[] = "videoPlayerSoftwareDecoding";

class CpuVideoWidget : public QWidget {
public:
    explicit CpuVideoWidget(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        connect(&sink, &QVideoSink::videoFrameChanged, this, [this] { update(); });
        connect(&sink, &QVideoSink::subtitleTextChanged, this, [this] { update(); });
    }

    QVideoSink sink;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QVideoFrame frame = sink.videoFrame();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (!frame.isValid()) return;
        // QVideoFrame::paint can use a GPU shader to convert video frames to RGB.
        const QImage image = qImageFromVideoFrame(frame, qNormalizedFrameTransformation(frame), true);
        const QSize size = image.size().scaled(this->size(), Qt::KeepAspectRatio);
        QRect picture(QPoint(), size);
        picture.moveCenter(rect().center());
        painter.drawImage(picture, image);
        const QString subtitle = frame.subtitleText();
        if (!subtitle.isEmpty()) {
            QVideoTextureHelper::SubtitleLayout layout;
            layout.update(size, subtitle);
            layout.draw(&painter, picture.topLeft());
        }
#else
        frame.paint(&painter, rect(), {});
#endif
    }
};

class VideoView : public QQuickView {
public:
    VideoView() { setColor(Qt::black); }

#ifdef Q_OS_WIN
protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override {
        return eraseBlackBackground(message, result)
            || QQuickView::nativeEvent(eventType, message, result);
    }
#endif
};
}

VideoSurface::VideoSurface(QWidget *parent) : QWidget(parent) {
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    const QString platform = QApplication::platformName();
    const bool gpuAvailable = platform != "offscreen" && platform != "minimal"
        && !qEnvironmentVariableIntValue("VIDEO_PLAYER_CPU_GAMMA")
        && qgetenv("QT_QUICK_BACKEND") != "software"
        && QQuickWindow::sceneGraphBackend() != "software";
    if (!gpuAvailable || !useGpuRenderer()) useCpuRenderer();
}

bool VideoSurface::useGpuRenderer() {
    auto view = std::make_unique<VideoView>();
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setSource(QUrl("qrc:/player/video/Video.qml"));
    if (view->status() != QQuickView::Ready) return false;
    auto *output = view->rootObject()->property("videoSink").value<QVideoSink *>();
    if (!output) return false;

    sink = output;
    quick = view.release();
    container = QWidget::createWindowContainer(quick, this);
    quick->installEventFilter(this);
    connect(quick, &QQuickWindow::sceneGraphInitialized, this, [this] {
        if (quick && quick->rendererInterface()->graphicsApi() == QSGRendererInterface::Software)
            useCpuRenderer();
    }, Qt::QueuedConnection);
    connect(quick, &QQuickWindow::sceneGraphError, this, [this](auto, const QString &message) {
        if (!quick) return;
        qWarning().noquote() << "GPU video rendering unavailable:" << message;
        useCpuRenderer();
    }, Qt::QueuedConnection);
    connect(quick->rootObject(), SIGNAL(shaderError()), this, SLOT(useCpuRenderer()), Qt::QueuedConnection);
    watchSurface(container);
    return true;
}

void VideoSurface::useCpuRenderer() {
    if (filter) return;
    const bool softwareDecoding = usesSoftwareDecoding();
    const QVideoFrame frame = sink ? sink->videoFrame() : QVideoFrame{};
    const QString subtitle = sink ? sink->subtitleText() : QString{};
    if (quick) {
        disconnect(sink, &QVideoSink::videoFrameChanged, this, &VideoSurface::videoFrameChanged);
        quick->removeEventFilter(this);
        quick = nullptr;
        container->removeEventFilter(this);
        layout()->removeWidget(container);
        container->hide();
        container->deleteLater();
        container = nullptr;
    }
    auto *cpu = new CpuVideoWidget(this);
    sink = &cpu->sink;
    filter = new GammaFilter(sink, cpu);
    setSoftwareDecoding(softwareDecoding);
    if (player) player->setVideoSink(sink);
    sink->setSubtitleText(subtitle);
    if (frame.isValid()) sink->setVideoFrame(frame);
    setGamma(gamma);
    watchSurface(cpu);
    cpu->show();
    if (frame.isValid()) emit videoFrameChanged(sink->videoFrame());
}

void VideoSurface::watchSurface(QWidget *surface) {
    surface->setFocusPolicy(Qt::NoFocus);
    surface->installEventFilter(this);
    layout()->addWidget(surface);
    connect(sink, &QVideoSink::videoFrameChanged, this, &VideoSurface::videoFrameChanged);
}

bool VideoSurface::usesSoftwareDecoding() const {
    return sink && sink->property(softwareDecodingProperty).toBool();
}

void VideoSurface::setSoftwareDecoding(bool enabled) {
    sink->setProperty(softwareDecodingProperty, enabled);
    if (filter) filter->input()->setProperty(softwareDecodingProperty, enabled);
}

void VideoSurface::setPlayer(QMediaPlayer *value) {
    player = value;
    player->setVideoSink(sink);
    setGamma(gamma);
}

void VideoSurface::setGamma(int tenths) {
    gamma = qBound(1, tenths, 40);
    if (quick) quick->rootObject()->setProperty("gamma", gamma / 10.0);
    else if (player) filter->apply(player, gamma);
}

QRect VideoSurface::contentRect() const {
    if (quick) return quick->rootObject()->property("contentRect").toRectF().toAlignedRect();
    QSize size = sink->videoSize();
    if (size.isEmpty()) size = this->size();
    size.scale(this->size(), Qt::KeepAspectRatio);
    QRect rect(QPoint(), size);
    rect.moveCenter(this->rect().center());
    return rect;
}

QImage VideoSurface::grabFrame() {
    if (quick) return quick->grabWindow();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    return qImageFromVideoFrame(sink->videoFrame(), true);
#else
    return sink->videoFrame().toImage();
#endif
}

bool VideoSurface::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (watched == quick && mouse->button() == Qt::RightButton) {
            // Native Quick windows do not synthesize QWidget context menus.
            QContextMenuEvent context(QContextMenuEvent::Mouse, mouse->position().toPoint(),
                mouse->globalPosition().toPoint(), mouse->modifiers());
            QApplication::sendEvent(this, &context);
            return true;
        }
        break;
    }
    case QEvent::ContextMenu:
        // Some platforms also deliver a native context event after the release.
        if (watched == quick
            && static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Mouse)
            return true;
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::ShortcutOverride:
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
        break;
    default:
        return false;
    }
    // Both child renderers fill this widget, so local coordinates match.
    QApplication::sendEvent(this, event);
    return true;
}
