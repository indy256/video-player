#include "videosurface.h"
#include <QApplication>
#include <QAudioOutput>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoSink>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
double processSeconds() {
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](FILETIME time) {
        return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 10000000.0;
#else
    return double(std::clock()) / CLOCKS_PER_SEC;
#endif
}

class Prototype : public QWidget {
public:
    Prototype(const QString &mode, int gamma, const QString &report)
        : mode(mode), report(report), player(this), audio(this) {
        setWindowTitle("GPU gamma prototype — " + mode);
        resize(1280, 800);
        audio.setMuted(true);
        player.setAudioOutput(&audio);
        auto *layout = new QVBoxLayout(this);
        // Select the same fallback used by the application, without duplicating either renderer.
        qputenv("VIDEO_PLAYER_CPU_GAMMA", mode == "cpu" ? "1" : "0");
        surface = new VideoSurface;
        surface->setPlayer(&player);
        layout->addWidget(surface, 1);
        auto *controls = new QHBoxLayout;
        auto *play = new QPushButton("Play / pause (Space)");
        controls->addWidget(play);
        timeline = new QSlider(Qt::Horizontal);
        timeline->setRange(0, 10000);
        controls->addWidget(timeline, 1);
        auto *gammaLabel = new QLabel;
        controls->addWidget(gammaLabel);
        gammaSlider = new QSlider(Qt::Horizontal);
        gammaSlider->setRange(1, 40);
        gammaSlider->setFixedWidth(180);
        controls->addWidget(gammaSlider);
        layout->addLayout(controls);
        const auto toggle = [this] { player.isPlaying() ? player.pause() : player.play(); };
        connect(play, &QPushButton::clicked, this, toggle);
        auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
        connect(space, &QShortcut::activated, this, toggle);
        auto *fullscreen = new QShortcut(QKeySequence(Qt::Key_F), this);
        connect(fullscreen, &QShortcut::activated, this, [this] {
            isFullScreen() ? showNormal() : showFullScreen();
        });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(escape, &QShortcut::activated, this, &QWidget::close);
        connect(timeline, &QSlider::valueChanged, this, [this](int value) {
            const bool resume = player.mediaStatus() == QMediaPlayer::EndOfMedia;
            player.setPosition(qRound64(player.duration() * value / 10000.0));
            if (resume && player.position() < player.duration()) player.play();
        });
        connect(&player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
            if (timeline->isSliderDown()) return;
            const QSignalBlocker blocked(timeline);
            timeline->setValue(player.duration() ? qRound(position * 10000.0 / player.duration()) : 0);
        });
        connect(gammaSlider, &QSlider::valueChanged, this, [this, gammaLabel](int value) {
            gammaLabel->setText(QString("Gamma %1").arg(value / 10.0, 0, 'f', 1));
            surface->setGamma(value);
        });
        gammaSlider->setValue(gamma);
        connect(&player, &QMediaPlayer::errorOccurred, this,
                [this](QMediaPlayer::Error, const QString &error) { fail(error); });
        connect(surface, &VideoSurface::videoFrameChanged, this, [this](const QVideoFrame &frame) {
            if (!frame.isValid()) return;
            if (!firstFrame) {
                firstFrame = true;
                if (benchmarkSeconds > 0) QTimer::singleShot(2000, this, [this] { startMeasurement(); });
            }
            if (measuring) {
                const double now = elapsed.nsecsElapsed() / 1000000.0;
                if (frames) intervals.append(now - lastArrival);
                lastArrival = now;
                ++frames;
                lastTimestamp = frame.startTime();
            }
        });
    }

    void open(const QString &path, double seconds) {
        benchmarkSeconds = seconds;
        player.setSource(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
        player.play();
        QTimer::singleShot(15000, this, [this] { if (!firstFrame) fail("No decoded frame within 15 seconds"); });
    }

private:
    void fail(const QString &message) {
        qCritical().noquote() << message;
        QTimer::singleShot(0, qApp, [] { qApp->exit(1); });
    }

    void finish(QJsonObject result) {
        if (mode == "gpu" && !surface->usesGpuGamma()) {
            fail("GPU rendering became unavailable during measurement");
            return;
        }
        result.insert("mode", mode);
        result.insert("gamma", gammaSlider->value() / 10.0);
        result.insert("device_pixel_ratio", surface->devicePixelRatioF());
        auto *quick = surface->findChild<QQuickWidget *>();
        result.insert("graphics_api", surface->usesGpuGamma() && quick
            ? quick->quickWindow()->rendererInterface()->graphicsApi() == QSGRendererInterface::Direct3D11
                ? "d3d11" : "other"
            : "native-widget");
        const QByteArray json = QJsonDocument(result).toJson();
        if (!report.isEmpty()) {
            QFile file(report);
            if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) {
                fail("Cannot write the report");
                return;
            }
        }
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        qApp->exit(0);
    }

    void startMeasurement() {
        if (mode == "gpu" && !surface->usesGpuGamma()) {
            fail("GPU benchmark requires a GPU scene graph");
            return;
        }
        if (!player.isPlaying() || !isVisible() || !windowHandle()->isExposed()) {
            fail("Benchmark requires visible playback");
            return;
        }
        frames = 0;
        intervals.clear();
        cpuStart = processSeconds();
        firstTimestamp = surface->videoSink()->videoFrame().startTime();
        elapsed.start();
        measuring = true;
        QTimer::singleShot(qRound(benchmarkSeconds * 1000), this, [this] {
            const double wall = elapsed.nsecsElapsed() / 1e9;
            const double cpu = processSeconds() - cpuStart;
            measuring = false;
            std::sort(intervals.begin(), intervals.end());
            const double p95 = intervals.isEmpty() ? 0 : intervals[qMin(intervals.size() - 1,
                qsizetype(intervals.size() * 0.95))];
            if (!player.isPlaying() || frames < 2 || lastTimestamp <= firstTimestamp) {
                fail("Video ended or stopped during measurement; use a longer fixture");
                return;
            }
            finish({{"wall_seconds", wall}, {"cpu_seconds", cpu}, {"cpu_percent_one_core", cpu / wall * 100},
                {"frames", frames}, {"delivered_fps", frames / wall}, {"arrival_interval_p95_ms", p95},
                {"media_advance_seconds", (lastTimestamp - firstTimestamp) / 1e6},
                {"source_width", surface->videoSink()->videoSize().width()}, {"source_height", surface->videoSink()->videoSize().height()},
                {"surface_width", surface->width()}, {"surface_height", surface->height()}});
        });
    }

    QString mode, report;
    QMediaPlayer player;
    QAudioOutput audio;
    VideoSurface *surface = nullptr;
    QSlider *timeline = nullptr;
    QSlider *gammaSlider = nullptr;
    QElapsedTimer elapsed;
    QList<double> intervals;
    double benchmarkSeconds = 0, cpuStart = 0, lastArrival = 0;
    qint64 firstTimestamp = 0, lastTimestamp = 0;
    int frames = 0;
    bool firstFrame = false, measuring = false;
};
}

int main(int argc, char **argv) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QApplication app(argc, argv);
    app.setApplicationName("GPU gamma prototype");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("video", "Local video (opens a file picker when omitted)", "[video]");
    parser.addOption({"mode", "Rendering path: gpu or cpu", "mode", "gpu"});
    parser.addOption({"gamma", "Initial gamma (0.1–4.0)", "gamma", "1.6"});
    parser.addOption({"seconds", "Benchmark duration after two seconds of warmup; 0 is interactive", "seconds", "0"});
    parser.addOption({"report", "Write JSON results to this file", "path"});
    parser.process(app);
    bool validGamma, validSeconds;
    const double gamma = parser.value("gamma").toDouble(&validGamma);
    const double seconds = parser.value("seconds").toDouble(&validSeconds);
    const QString mode = parser.value("mode");
    if (parser.positionalArguments().size() > 1 || !validGamma || !std::isfinite(gamma) || gamma < 0.1 || gamma > 4
        || !validSeconds || !std::isfinite(seconds) || seconds < 0 || seconds > 3600 || (mode != "gpu" && mode != "cpu")
        || (parser.positionalArguments().isEmpty() && seconds > 0)) parser.showHelp(2);
    const QString path = parser.positionalArguments().isEmpty()
        ? QFileDialog::getOpenFileName(nullptr, "Choose a video for the gamma prototype")
        : parser.positionalArguments().first();
    if (path.isEmpty()) return 0;
    Prototype window(mode, qRound(gamma * 10), parser.value("report"));
    window.show();
    window.open(path, seconds);
    return app.exec();
}
