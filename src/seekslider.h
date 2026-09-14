#pragma once
#include <QMouseEvent>
#include <QSlider>
#include <QStyleOptionSlider>

// Absolute seeking on click; dragging previews a position and commits on release.
class SeekSlider : public QSlider {
    Q_OBJECT
public:
    explicit SeekSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) {}
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) { QSlider::mousePressEvent(event); return; }
        setFocus();
        setSliderDown(true);
        updatePosition(event->position().x());
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (isSliderDown()) { updatePosition(event->position().x()); event->accept(); }
        else QSlider::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && isSliderDown()) {
            updatePosition(event->position().x());
            setSliderDown(false);
            event->accept();
        } else QSlider::mouseReleaseEvent(event);
    }
private:
    void updatePosition(double x) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const auto groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const int span = qMax(1, groove.width() - handle.width());
        setValue(QStyle::sliderValueFromPosition(minimum(), maximum(),
            qBound(0, qRound(x) - groove.x() - handle.width() / 2, span), span, option.upsideDown));
    }
};
