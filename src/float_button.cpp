#include "float_button.h"
#include "app.h"
#include "window_listener.h"
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>

namespace {
// 全屏模式下只画一个紧贴文字的透明小方框，不遮挡游戏画面
constexpr int kFullscreenFrameHorizontalPadding = 12;
constexpr int kFullscreenFrameVerticalPadding = 8;
constexpr int kFullscreenRightPadding = 20;
constexpr int kFullscreenTopPadding = 20;
constexpr int kWindowedRightPadding = 20;
constexpr int kWindowedTopPadding = 40;
} // namespace

FloatButton::FloatButton() : QPushButton("一键拔线"), _logger(spdlog::get("skipper")) {
    windowListener = new HearthStoneWindowListener(this);
    // 首次运行时若缺少辅助功能权限，主动触发系统授权提示，避免按钮静默不显示
    requestAccessibilityPermission();

    QFont font;
    font.setFamily("LiShu");
    font.setPixelSize(42);
    setFont(font);

    setStyleSheet("background: transparent; color: white;");
    setCursor(Qt::PointingHandCursor);

    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowStayOnTop(this);
    connect(windowListener, &HearthStoneWindowListener::onAppLaunch, this, [this](QRect rect) {
        moveToWindow(rect);
        setWindowStayOnTop(this);
        setVisible(true);
        qDebug() << "app launch rect=" << rect;
    });
    connect(windowListener, &HearthStoneWindowListener::onAppGetFocus, this, [this](QRect rect) {
        moveToWindow(rect);
        setWindowStayOnTop(this);
        setVisible(true);
    });
    connect(windowListener, &HearthStoneWindowListener::onAppLoseFocus, this, [this]() {
        // 延迟一小段时间再隐藏，避免点击悬浮按钮时因焦点切换导致按钮消失
        QTimer::singleShot(100, this, [this]() {
            if (!underMouse()) {
                setVisible(false);
            }
        });
    });
    connect(windowListener, &HearthStoneWindowListener::onAppMove, this, [this](QRect rect) {
        moveToWindow(rect);
        setWindowStayOnTop(this);
        setVisible(true);
    });
    connect(windowListener, &HearthStoneWindowListener::onAppTerminate, this, [this]() { setVisible(false); });
}

void FloatButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (_fullscreen) {
        // 全屏模式：透明背景 + 紧贴文字的细边框长方形
        const QRectF frame = QRectF(rect()).adjusted(2, 2, -2, -2);
        QPainterPath framePath;
        framePath.addRoundedRect(frame, 8, 8);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(framePath);

        QPainterPath textPath;
        textPath.addText(0, 0, font(), text());
        const QRectF textBounds = textPath.boundingRect();
        textPath.translate(frame.center().x() - textBounds.center().x(),
                           frame.center().y() - textBounds.center().y());

        // 先画黑色描边，再画白色填充，保证游戏画面上的可读性
        painter.setPen(QPen(Qt::black, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(textPath);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(textPath);
        return;
    }

    QRect rect = this->rect().adjusted(2, 2, -2, -2);
    QPainterPath path;
    path.addText(rect.bottomLeft(), font(), text());

    // 先画黑色描边
    painter.setPen(QPen(Qt::black, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);

    // 再画白色填充
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
}

void FloatButton::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        event->accept();
        onBtnClicked();
    }
}

void FloatButton::moveToWindow(const QRect &windowRect) {
    const bool fullscreen = isFullscreen(windowRect);
    if (fullscreen != _fullscreen) {
        SPDLOG_LOGGER_INFO(_logger, "float_button_mode_changed fullscreen={}", fullscreen);
        _fullscreen = fullscreen;
    }

    ensurePolished();
    const QSize hint = sizeHint();
    const int buttonWidth = _fullscreen ? hint.width() + kFullscreenFrameHorizontalPadding : hint.width();
    const int buttonHeight = _fullscreen ? hint.height() + kFullscreenFrameVerticalPadding : hint.height();
    const int rightPadding = _fullscreen ? kFullscreenRightPadding : kWindowedRightPadding;
    const int topPadding = _fullscreen ? kFullscreenTopPadding : kWindowedTopPadding;

    resize(buttonWidth, buttonHeight);
    move(windowRect.right() - buttonWidth - rightPadding, windowRect.y() + topPadding);
    update();
}

bool FloatButton::isFullscreen(const QRect &windowRect) const {
    const QScreen *screen = QGuiApplication::screenAt(windowRect.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return false;
    }

    // macOS 全屏窗口的 AX frame 与所在屏幕的 geometry 一致；
    // 无边框“伪全屏”窗口则与 availableGeometry 一致，两种都按全屏处理。
    const auto matches = [&windowRect](const QRect &reference) {
        constexpr int kTolerance = 2;
        return qAbs(windowRect.x() - reference.x()) <= kTolerance &&
               qAbs(windowRect.y() - reference.y()) <= kTolerance &&
               qAbs(windowRect.width() - reference.width()) <= kTolerance &&
               qAbs(windowRect.height() - reference.height()) <= kTolerance;
    };
    return matches(screen->geometry()) || matches(screen->availableGeometry());
}

void FloatButton::onBtnClicked() {
    SPDLOG_LOGGER_INFO(_logger, "float_button_clicked");
    connect(
        App::skipper, &Skipper::skipFinished, this,
        [this](bool success) {
            setText(success ? "拔线成功" : "拔线失败");
            QTimer::singleShot(1000, this, [this]() { setText("一键拔线"); });
        },
        Qt::SingleShotConnection);
    App::skipper->skip();
}
