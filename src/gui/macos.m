/*
 * macos.m: AppKit for the window's title bar and the application menu.
 * Manual reference counting, like Raven's own Cocoa code.
 */
#import <Cocoa/Cocoa.h>

#include "macos.h"

#include <string.h>

/* ---- title bar ----------------------------------------------------------- */

void mac_window_setup(Rid window)
{
    RNativeWindow native;
    memset(&native, 0, sizeof native);
    if (!Rwindow_get_native_handle(window, &native) || native.system != R_WINDOW_SYSTEM_COCOA ||
        !native.display)
        return;
    NSWindow *w = (NSWindow *)native.display;
    /* The content covers the whole window; the title bar stays, see-through
     * and without its title, so the traffic lights are the system's. */
    w.styleMask |= NSWindowStyleMaskFullSizeContentView;
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility = NSWindowTitleHidden;
    w.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    /* An empty unified toolbar makes the band MAC_TITLE_H tall and centres
     * the traffic lights in it. */
    NSToolbar *bar = [[NSToolbar alloc] initWithIdentifier:@"Smalltify"];
    w.toolbar = bar;
    [bar release];
    w.toolbarStyle = NSWindowToolbarStyleUnified;

    /* The loop sleeps: coming to the front or leaving it wakes it, so the
     * frame sees mac_app_active change (window.c parks the text focus). */
    static int observing;
    if (!observing) {
        observing = 1;
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        void (^wake)(NSNotification *) = ^(NSNotification *note) {
            (void)note;
            Rwake_up();
        };
        [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:wake];
        [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:wake];
    }
}

int mac_app_active(void)
{
    return [NSApp isActive] ? 1 : 0;
}

/* ---- application menu ------------------------------------------------------ */

static const char *g_settings_action;

/* Menu items act inside AppKit's event handling; the action fires at the
 * next frame, as Raven's own menu items do. */
static void fire_settings(void *user)
{
    (void)user;
    if (g_settings_action)
        Raction_fire(g_settings_action, R_INVALID_ID);
}

@interface SmpMenuTarget : NSObject
- (void)showSettings:(id)sender;
@end

@implementation SmpMenuTarget
- (void)showSettings:(id)sender
{
    (void)sender;
    Rtask_post(fire_settings, NULL);
}
@end

static SmpMenuTarget *g_target; /* lives as long as the process: items do not retain targets */

/* Raven names the application after the process (the executable); the
 * bundle says how it is called. */
static void rename_app_items(NSMenu *app)
{
    NSString *process = [[NSProcessInfo processInfo] processName];
    NSString *name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
    if (!process || ![name isKindOfClass:[NSString class]] || [name isEqualToString:process])
        return;
    for (NSMenuItem *item in [app itemArray]) {
        NSString *title = [item title];
        if ([title rangeOfString:process].location != NSNotFound)
            [item setTitle:[title stringByReplacingOccurrencesOfString:process withString:name]];
    }
}

void mac_menu_sync(const char *settings_action)
{
    g_settings_action = settings_action;
    NSMenu *bar = [NSApp mainMenu];
    if (!bar || [bar numberOfItems] == 0)
        return;
    NSMenu *app = [[bar itemAtIndex:0] submenu];
    if (!app)
        return;
    for (NSMenuItem *item in [app itemArray])
        if ([item action] == @selector(showSettings:))
            return; /* this menu has it already */
    if (!g_target)
        g_target = [[SmpMenuTarget alloc] init];
    /* After "About" and its separator, as on every Mac application. */
    NSInteger at = [app numberOfItems] >= 2 ? 2 : [app numberOfItems];
    NSMenuItem *settings = [[NSMenuItem alloc] initWithTitle:@"Settings\u2026"
                                                      action:@selector(showSettings:)
                                               keyEquivalent:@","];
    [settings setTarget:g_target];
    [settings setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [app insertItem:settings atIndex:at];
    [settings release];
    [app insertItem:[NSMenuItem separatorItem] atIndex:at + 1];
    rename_app_items(app);
}
