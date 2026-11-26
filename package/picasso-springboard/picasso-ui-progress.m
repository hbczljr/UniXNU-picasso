// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// UI progress breadcrumbs for SpringBoard, written to /dev/console so they land
// in the kernel console ring -- and therefore in the durable console record,
// which is the only channel that survives past the ~91 s USB console death.
//
// Why: the boot reaches an Apple logo drawn by backboardd's own render overlay
// and then stops there for many minutes, while SpringBoard is running (injected,
// acknowledged, metrics installed). Nothing on the console says how far
// SpringBoard's own UI gets, so this reports the process's lifecycle events and
// a periodic window census: did it finish launching, did it become active, and
// does it own a visible/key window with a plausible size.
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static dispatch_source_t picasso_ui_tick;
static NSMutableArray *picasso_ui_observers;

static void picasso_ui_emit(const char *text)
{
    int fd = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char line[320];
    int n = snprintf(line, sizeof(line), "PICASSO_SBUI %s\n", text);
    if (n > 0) {
        size_t length = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        ssize_t written = write(fd, line, length);
        (void)written;
    }
    close(fd);
}

// Main queue only: UIApplication and the scene graph are main-thread objects.
static void picasso_ui_census(const char *tag)
{
    UIApplication *app = [UIApplication sharedApplication];
    unsigned scenes = 0, total = 0, shown = 0, key = 0, active = 0;
    unsigned long long width = 0, height = 0, level = 0;
    NSMutableArray<UIWindow *> *windows = [NSMutableArray array];
    if (@available(iOS 13.0, *)) {
        for (UIScene *scene in app.connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            ++scenes;
            [windows addObjectsFromArray:((UIWindowScene *)scene).windows];
        }
    }
    total = (unsigned)windows.count;
    for (UIWindow *window in windows) {
        if (window.isHidden) continue;
        ++shown;
        if (!window.isKeyWindow) continue;
        ++key;
        CGRect bounds = window.bounds;
        width = (unsigned long long)bounds.size.width;
        height = (unsigned long long)bounds.size.height;
        level = (unsigned long long)window.windowLevel;
    }
    if (app.applicationState == UIApplicationStateActive) active = 1;
    char text[224];
    int n = snprintf(text, sizeof(text),
        "%s scenes=%u windows=%u shown=%u key=%u active=%u level=%llu size=%llux%llu",
        tag, scenes, total, shown, key, active, level, width, height);
    if (n > 0) picasso_ui_emit(text);
}

static void picasso_ui_observe(NSString *name, const char *tag)
{
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    if (!picasso_ui_observers) picasso_ui_observers = [NSMutableArray array];
    id token = [center addObserverForName:name object:nil queue:[NSOperationQueue mainQueue]
                               usingBlock:^(NSNotification *note) {
        (void)note;
        picasso_ui_census(tag);
    }];
    if (token) [picasso_ui_observers addObject:token];
}

__attribute__((constructor)) static void picasso_ui_progress_install(void)
{
    picasso_ui_emit("loaded");
    dispatch_async(dispatch_get_main_queue(), ^{
        picasso_ui_emit("main");
        picasso_ui_census("boot");
        picasso_ui_observe(UIApplicationDidFinishLaunchingNotification, "launched");
        picasso_ui_observe(UIApplicationDidBecomeActiveNotification, "active");
        picasso_ui_observe(UIApplicationWillResignActiveNotification, "resign");
        picasso_ui_observe(UIWindowDidBecomeVisibleNotification, "window-visible");
        picasso_ui_observe(UIWindowDidBecomeKeyNotification, "window-key");
        picasso_ui_observe(UIApplicationWillEnterForegroundNotification, "foreground");
        // The record holds the tail, so a periodic census is what shows whether a
        // process that stopped printing is still alive and still owns a window.
        picasso_ui_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                 dispatch_get_main_queue());
        if (picasso_ui_tick) {
            dispatch_source_set_timer(picasso_ui_tick,
                dispatch_time(DISPATCH_TIME_NOW, 20ull * NSEC_PER_SEC),
                20ull * NSEC_PER_SEC, 2ull * NSEC_PER_SEC);
            dispatch_source_set_event_handler(picasso_ui_tick, ^{
                picasso_ui_census("tick");
            });
            dispatch_resume(picasso_ui_tick);
        }
    });
}
