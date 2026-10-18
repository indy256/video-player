#include "videosurface.h"
#include "gammafilter.h"
#include <QApplication>
#include <QEvent>
#include <QMediaPlayer>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QVBoxLayout>
#include <QVideoWidget>

VideoSurface::VideoSurface(QWidget *parent) : QWidget(parent) {
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    const QString platform = QApplication::platformName();
    if (platform == "offscreen" || platform == "minimal"
        || qEnvironmentVariableIntValue("VIDEO_PLAYER_CPU_GAMMA")
        || qgetenv("QT_QUICK_BACKEND") == "software"
        || QQuickWindow::sceneGraphBackend() == "software") {
        useCpuRenderer();
        return;
    }
    quick = new QQuickWidget(this);
    quick->setClearColor(Qt::black);
    quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    connect(quick->quickWindow(), &QQuickWindow::sceneGraphInitialized, this, [this] {
        if (quick && quick->quickWindow()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software)
            useCpuRenderer();
    }, Qt::QueuedConnection);
    connect(quick, &QQuickWidget::sceneGraphError, this, [this](auto, const QString &message) {
        qWarning().noquote() << "GPU video rendering unavailable:" << message;
        useCpuRenderer();
    }, Qt::QueuedConnection);
    quick->setSource(QUrl("qrc:/player/video/Video.qml"));
    if (quick->status() != QQuickWidget::Ready
        || !(sink = quick->rootObject()->property("videoSink").value<QVideoSink *>())) {
        useCpuRenderer();
        return;
    }
    connect(quick->rootObject(), SIGNAL(shaderError()), this, SLOT(useCpuRenderer()), Qt::QueuedConnection);
    watchSurface(quick);
}

void VideoSurface::watchSurface(QWidget *surface) {
    surface->setFocusPolicy(Qt::NoFocus);
    surface->installEventFilter(this);
    layout()->addWidget(surface);
    connect(sink, &QVideoSink::videoFrameChanged, this, &VideoSurface::videoFrameChanged);
}

void VideoSurface::useCpuRenderer() {
    if (filter) return;
    const QVideoFrame frame = sink ? sink->videoFrame() : QVideoFrame();
    if (quick) {
        qWarning("Using CPU gamma fallback");
        layout()->removeWidget(quick);
        quick->hide();
        quick->deleteLater();
        quick = nullptr;
    }
    auto *native = new QVideoWidget(this);
    sink = native->videoSink();
    filter = new GammaFilter(sink, this);
    watchSurface(native);
    if (frame.isValid()) sink->setVideoFrame(frame);
    if (player) setPlayer(player);
    native->show();
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
    return quick ? quick->grabFramebuffer() : sink->videoFrame().toImage();
}

bool VideoSurface::eventFilter(QObject *, QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
        // Both child renderers fill this widget, so local coordinates match.
        QApplication::sendEvent(this, event);
        return true;
    default:
        return false;
    }
}
