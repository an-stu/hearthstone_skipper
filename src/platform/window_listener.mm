#include <Cocoa/Cocoa.h>

#include "spdlog/spdlog.h"
#include "window_listener.h"
#include <QRect>
#include <QTimer>
#include <QWidget>

@interface AppLaunchObserver : NSObject
@property HearthStoneWindowListener *listener;
@property AXUIElementRef hearthstoneWindowRef;
@property(nonatomic, assign) AXObserverRef windowObserver;
@property pid_t pid;
@property std::shared_ptr<spdlog::logger> logger;
@property int retryCount;
@end

static NSString *bundleIdentifier = @"unity.Blizzard Entertainment.Hearthstone";

@implementation AppLaunchObserver

- (instancetype)initWithListener:(HearthStoneWindowListener *)listener {
    self = [super init];
    self.logger = spdlog::get("skipper");
    self.windowObserver = nullptr;
    self.retryCount = 30;
    if (self) {
        self.listener = listener;
        NSNotificationCenter *center = [[NSWorkspace sharedWorkspace] notificationCenter];
        [center addObserver:self
                   selector:@selector(appGetFocused:)
                       name:NSWorkspaceDidActivateApplicationNotification
                     object:nil];
        [center addObserver:self
                   selector:@selector(appGetFocused:)
                       name:NSWorkspaceActiveSpaceDidChangeNotification
                     object:nil];
        [center addObserver:self
                   selector:@selector(appLoseFocused:)
                       name:NSWorkspaceDidDeactivateApplicationNotification
                     object:nil];
        [center addObserver:self
                   selector:@selector(appLaunched:)
                       name:NSWorkspaceDidLaunchApplicationNotification
                     object:nil];
        [center addObserver:self
                   selector:@selector(appTerminated:)
                       name:NSWorkspaceDidTerminateApplicationNotification
                     object:nil];

        // 处理 Skipper 启动时炉石已经在前台（例如已经全屏）的情况：
        // 此时不会再有激活通知，需要主动触发一次检查。
        NSRunningApplication *frontmost = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if ([frontmost.bundleIdentifier isEqual:bundleIdentifier]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self appGetFocused:nil];
            });
        }
    }
    return self;
}

- (void)dealloc {
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
    [self closeListener];
    if (self.hearthstoneWindowRef) {
        CFRelease(self.hearthstoneWindowRef);
    }
}

static void windowPositionUpdateCallback(AXObserverRef, AXUIElementRef, CFStringRef, void *refcon) {
    auto *self = (__bridge AppLaunchObserver *)refcon;
    QRect rect = [self getWindowLocation];
    if (rect.isNull()) {
        return;
    }
    emit self.listener->onAppMove(rect);
}

- (void)appGetFocused:(NSNotification *)notification {
    NSRunningApplication *app = notification.userInfo[NSWorkspaceApplicationKey];
    if (app == nil) {
        app = [[NSWorkspace sharedWorkspace] frontmostApplication];
    }
    if (![app.bundleIdentifier isEqual:bundleIdentifier]) {
        return;
    }
    requestAccessibilityPermission();
    if (self.windowObserver == nullptr) {
        SPDLOG_LOGGER_INFO(_logger, "Hearthstone get focus, try setup window listener");
        [self setupListener:app];
    }
    QRect rect = [self getWindowLocation];
    if (rect.isNull() && !accessibilityPermissionGranted()) {
        // 没有辅助功能权限时拿不到炉石窗口 frame，退回当前活动屏幕的 frame，
        // 保证全屏时按钮仍然出现在右上角。
        rect = [self fallbackWindowLocation];
        SPDLOG_LOGGER_WARN(_logger, "AX permission missing, fallback to active screen frame={},{},{},{}", rect.x(),
                           rect.y(), rect.width(), rect.height());
    }
    if (!rect.isNull()) {
        emit self.listener->onAppGetFocus(rect);
    }
}

- (void)appLoseFocused:(NSNotification *)notification {
    NSRunningApplication *app = notification.userInfo[NSWorkspaceApplicationKey];
    if ([app.bundleIdentifier isEqual:bundleIdentifier]) {
        emit self.listener->onAppLoseFocus();
    }
}

- (void)appLaunched:(NSNotification *)notification {
    NSRunningApplication *app = notification.userInfo[NSWorkspaceApplicationKey];
    if (![app.bundleIdentifier isEqual:bundleIdentifier]) {
        return;
    }
    [self startRetrySetupListener:app.processIdentifier];
}

- (void)appTerminated:(NSNotification *)notification {
    NSRunningApplication *app = notification.userInfo[NSWorkspaceApplicationKey];
    if (![app.bundleIdentifier isEqual:bundleIdentifier]) {
        return;
    }
    [self closeListener];
    if (self.hearthstoneWindowRef) {
        CFRelease(self.hearthstoneWindowRef);
        self.hearthstoneWindowRef = nullptr;
    }
    self.pid = 0;
    emit self.listener->onAppTerminate();
}

- (QRect)fallbackWindowLocation {
    NSArray<NSScreen *> *screens = [NSScreen screens];
    if (screens.count == 0) {
        return QRect();
    }
    NSScreen *primary = screens.firstObject;
    NSScreen *target = [NSScreen mainScreen];
    if (target == nil) {
        target = primary;
    }
    // Cocoa 屏幕坐标原点在左下角，转换为 Qt 的左上角全局坐标
    const NSRect primaryFrame = primary.frame;
    const NSRect targetFrame = target.frame;
    const int x = qRound(targetFrame.origin.x - primaryFrame.origin.x);
    const int y = qRound(primaryFrame.origin.y + primaryFrame.size.height -
                         (targetFrame.origin.y + targetFrame.size.height));
    return QRect(x, y, qRound(targetFrame.size.width), qRound(targetFrame.size.height));
}

- (QRect)getWindowLocation {
    if (self.hearthstoneWindowRef == nullptr) {
        return QRect();
    }

    CFTypeRef positionValue = nullptr;
    CFTypeRef sizeValue = nullptr;

    const AXError positionError =
        AXUIElementCopyAttributeValue(self.hearthstoneWindowRef, kAXPositionAttribute, &positionValue);
    if (positionError != kAXErrorSuccess) {
        SPDLOG_LOGGER_WARN(_logger, "AX position request failed error={}", (long)positionError);
        return QRect();
    }
    const AXError sizeError = AXUIElementCopyAttributeValue(self.hearthstoneWindowRef, kAXSizeAttribute, &sizeValue);
    if (sizeError != kAXErrorSuccess) {
        SPDLOG_LOGGER_WARN(_logger, "AX size request failed error={}", (long)sizeError);
        CFRelease(positionValue);
        return QRect();
    }

    QRect result;
    CGPoint position;
    CGSize size;
    if (AXValueGetType((AXValueRef)positionValue) == kAXValueTypeCGPoint &&
        AXValueGetType((AXValueRef)sizeValue) == kAXValueTypeCGSize) {
        AXValueGetValue((AXValueRef)positionValue, kAXValueTypeCGPoint, &position);
        AXValueGetValue((AXValueRef)sizeValue, kAXValueTypeCGSize, &size);
        result = QRect((int)position.x, (int)position.y, (int)size.width, (int)size.height);
    }

    CFRelease(positionValue);
    CFRelease(sizeValue);
    return result;
}

- (void)startRetrySetupListener:(pid_t)pid {
    self.pid = pid;
    self.retryCount = 0;
    [self retrySetupListener];
}

- (void)retrySetupListener {
    NSRunningApplication *currentApp = [NSRunningApplication runningApplicationWithProcessIdentifier:self.pid];
    if (currentApp == nil) {
        SPDLOG_LOGGER_WARN(_logger, "Hearthstone process is gone, stop retry");
        self.retryCount = 30;
        return;
    }
    [self setupListener:currentApp];
    QRect rect = [self getWindowLocation];
    if (!rect.isNull()) {
        SPDLOG_LOGGER_INFO(_logger, "found Hearthstone window at retry#{}", self.retryCount);
        self.retryCount = 30;
        emit self.listener->onAppLaunch(rect);
        return;
    }
    if (!accessibilityPermissionGranted()) {
        // 缺少辅助功能权限时重试也拿不到窗口，直接使用活动屏幕 frame 兜底。
        rect = [self fallbackWindowLocation];
        SPDLOG_LOGGER_WARN(_logger, "AX permission missing, stop retry and fallback to screen frame={},{},{},{}",
                           rect.x(), rect.y(), rect.width(), rect.height());
        self.retryCount = 30;
        if (!rect.isNull()) {
            emit self.listener->onAppLaunch(rect);
        }
        return;
    }

    if (self.retryCount < 30) {
        self.retryCount++;
        SPDLOG_LOGGER_WARN(_logger, "Hearthstone window not found, retry for #{}", self.retryCount);
        QTimer::singleShot(500, [self]() { [self retrySetupListener]; });
        return;
    }
    SPDLOG_LOGGER_WARN(_logger, "Hearthstone window not found, stop retry#{}", self.retryCount);
    self.retryCount = 30;
}

- (void)setupListener:(NSRunningApplication *)app {
    if (self.hearthstoneWindowRef != nullptr) {
        return;
    }
    self.pid = app.processIdentifier;
    AXUIElementRef appRef = AXUIElementCreateApplication(self.pid);
    CFArrayRef windows = nullptr;
    const AXError windowsError = AXUIElementCopyAttributeValue(appRef, kAXWindowsAttribute, (CFTypeRef *)&windows);
    CFRelease(appRef);
    if (windowsError != kAXErrorSuccess) {
        SPDLOG_LOGGER_WARN(_logger, "AX windows request failed error={} trusted={}", (long)windowsError,
                           accessibilityPermissionGranted());
        return;
    }
    if (windows == nullptr) {
        SPDLOG_LOGGER_WARN(_logger, "Hearthstone has no window, will retry");
        return;
    }
    if (CFArrayGetCount(windows) == 0) {
        CFRelease(windows);
        SPDLOG_LOGGER_WARN(_logger, "Hearthstone has zero window, will retry");
        return;
    }
    self.hearthstoneWindowRef = (AXUIElementRef)CFRetain(CFArrayGetValueAtIndex(windows, 0));
    CFRelease(windows);

    AXObserverCreate(self.pid, windowPositionUpdateCallback, &self->_windowObserver);
    if (self.windowObserver == nullptr) {
        SPDLOG_LOGGER_WARN(_logger, "Failed to create AXObserver for pid {}", self.pid);
        CFRelease(self.hearthstoneWindowRef);
        self.hearthstoneWindowRef = nullptr;
        return;
    }
    AXObserverAddNotification(self.windowObserver, self.hearthstoneWindowRef, kAXMovedNotification,
                              (__bridge void *)self);
    AXObserverAddNotification(self.windowObserver, self.hearthstoneWindowRef, kAXResizedNotification,
                              (__bridge void *)self);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), AXObserverGetRunLoopSource(self.windowObserver), kCFRunLoopDefaultMode);
    SPDLOG_LOGGER_INFO(_logger, "setup Hearthstone window listener");
}

- (void)closeListener {
    if (self.windowObserver) {
        AXObserverRemoveNotification(self.windowObserver, self.hearthstoneWindowRef, kAXMovedNotification);
        AXObserverRemoveNotification(self.windowObserver, self.hearthstoneWindowRef, kAXResizedNotification);
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), AXObserverGetRunLoopSource(self.windowObserver),
                              kCFRunLoopDefaultMode);
        CFRelease(self.windowObserver);
        self.windowObserver = nullptr;
    }
}

@end

bool accessibilityPermissionGranted() {
    return AXIsProcessTrusted();
}

void requestAccessibilityPermission() {
    const auto logger = spdlog::get("skipper");
    const bool trusted = AXIsProcessTrusted();
    SPDLOG_LOGGER_INFO(logger, "accessibility_permission trusted={}", trusted);
    if (trusted) {
        return;
    }
    SPDLOG_LOGGER_WARN(logger, "accessibility_permission missing, showing system prompt");
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
    const bool trustedAfterPrompt = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    SPDLOG_LOGGER_INFO(logger, "accessibility_permission after_prompt={}", trustedAfterPrompt);
}

void setWindowStayOnTop(QWidget *widget) {
    auto *view = (__bridge NSView *)(void *)widget->winId();
    NSWindow *window = view.window;
    window.level = NSFloatingWindowLevel;
    window.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
}

HearthStoneWindowListener::HearthStoneWindowListener(QObject *parent) : QObject(parent) {
    AppLaunchObserver *observer = [[AppLaunchObserver alloc] initWithListener:this];
    _observer = (__bridge_retained void *)observer;
}

HearthStoneWindowListener::~HearthStoneWindowListener() {
    AppLaunchObserver *observer = (__bridge_transfer AppLaunchObserver *)_observer;
    (void)observer; // ARC releases it, dealloc runs
}