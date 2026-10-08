/*
 * php-vio - Cocoa platform (NATIVE-PLATFORM-PLAN Phase 2, OPEN-ITEMS A3)
 *
 * macOS without GLFW: NSWindow with a vio view as first responder (keys by
 * virtual key code - the physical position, GLFW's table - text from the
 * event's characters, mouse buttons, precise and line scrolling, cursor
 * deltas while VIO_CURSOR_DISABLED), monitors and video modes through
 * NSScreen / CoreGraphics with a CGDisplaySetDisplayMode switch for
 * fullscreen, gamepads through the GameController framework, OpenGL through
 * NSOpenGLContext (4.1 core, then 3.2 core), and the Vulkan surface through
 * VK_EXT_metal_surface on a CAMetalLayer. The slots keep GLFW's Cocoa
 * semantics: window sizes and cursor positions in points, framebuffer sizes
 * in pixels, content scale = backingScaleFactor, screen y from the top.
 *
 * Compiled through vio_platform_cocoa.c with -x objective-c -fobjc-arc.
 * Objective-C objects held in C structs are CFBridgingRetain'd (ARC does not
 * track them there) and CFRelease'd when the window goes away.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#if defined(HAVE_COCOA) && !defined(HAVE_IOS)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <GameController/GameController.h>
#include <dlfcn.h>

#ifdef HAVE_VULKAN
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#endif

#include "../../../include/vio_platform.h"
#include "../../../include/vio_constants.h"
#include "../../vio_input.h"

typedef struct _vio_cocoa_window vio_cocoa_window;

/* ── Keyboard: virtual key code -> VIO_KEY_* (GLFW's table) ───────── */

static short cocoa_keycodes[256];

static void cocoa_keytable_init(void)
{
    for (int i = 0; i < 256; i++) cocoa_keycodes[i] = VIO_KEY_UNKNOWN;
    static const struct { int kc, key; } t[] = {
        {0x1D, VIO_KEY_0}, {0x12, VIO_KEY_1}, {0x13, VIO_KEY_2}, {0x14, VIO_KEY_3}, {0x15, VIO_KEY_4},
        {0x17, VIO_KEY_5}, {0x16, VIO_KEY_6}, {0x1A, VIO_KEY_7}, {0x1C, VIO_KEY_8}, {0x19, VIO_KEY_9},
        {0x00, VIO_KEY_A}, {0x0B, VIO_KEY_B}, {0x08, VIO_KEY_C}, {0x02, VIO_KEY_D}, {0x0E, VIO_KEY_E},
        {0x03, VIO_KEY_F}, {0x05, VIO_KEY_G}, {0x04, VIO_KEY_H}, {0x22, VIO_KEY_I}, {0x26, VIO_KEY_J},
        {0x28, VIO_KEY_K}, {0x25, VIO_KEY_L}, {0x2E, VIO_KEY_M}, {0x2D, VIO_KEY_N}, {0x1F, VIO_KEY_O},
        {0x23, VIO_KEY_P}, {0x0C, VIO_KEY_Q}, {0x0F, VIO_KEY_R}, {0x01, VIO_KEY_S}, {0x11, VIO_KEY_T},
        {0x20, VIO_KEY_U}, {0x09, VIO_KEY_V}, {0x0D, VIO_KEY_W}, {0x07, VIO_KEY_X}, {0x10, VIO_KEY_Y},
        {0x06, VIO_KEY_Z},
        {0x27, VIO_KEY_APOSTROPHE}, {0x2A, VIO_KEY_BACKSLASH}, {0x2B, VIO_KEY_COMMA}, {0x18, VIO_KEY_EQUAL},
        {0x32, VIO_KEY_GRAVE_ACCENT}, {0x21, VIO_KEY_LEFT_BRACKET}, {0x1B, VIO_KEY_MINUS}, {0x2F, VIO_KEY_PERIOD},
        {0x1E, VIO_KEY_RIGHT_BRACKET}, {0x29, VIO_KEY_SEMICOLON}, {0x2C, VIO_KEY_SLASH}, {0x0A, 161 /* WORLD_1 */},
        {0x33, VIO_KEY_BACKSPACE}, {0x39, VIO_KEY_CAPS_LOCK}, {0x75, VIO_KEY_DELETE}, {0x7D, VIO_KEY_DOWN},
        {0x77, VIO_KEY_END}, {0x24, VIO_KEY_ENTER}, {0x35, VIO_KEY_ESCAPE},
        {0x7A, VIO_KEY_F1}, {0x78, VIO_KEY_F2}, {0x63, VIO_KEY_F3}, {0x76, VIO_KEY_F4}, {0x60, VIO_KEY_F5},
        {0x61, VIO_KEY_F6}, {0x62, VIO_KEY_F7}, {0x64, VIO_KEY_F8}, {0x65, VIO_KEY_F9}, {0x6D, VIO_KEY_F10},
        {0x67, VIO_KEY_F11}, {0x6F, VIO_KEY_F12}, {0x69, 302}, {0x6B, 303}, {0x71, 304}, {0x6A, 305},
        {0x40, 306}, {0x4F, 307}, {0x50, 308}, {0x5A, 309},   /* F13 .. F20 */
        {0x73, VIO_KEY_HOME}, {0x72, VIO_KEY_INSERT}, {0x7B, VIO_KEY_LEFT}, {0x3A, VIO_KEY_LEFT_ALT},
        {0x3B, VIO_KEY_LEFT_CONTROL}, {0x38, VIO_KEY_LEFT_SHIFT}, {0x37, VIO_KEY_LEFT_SUPER}, {0x6E, VIO_KEY_MENU},
        {0x47, VIO_KEY_NUM_LOCK}, {0x79, VIO_KEY_PAGE_DOWN}, {0x74, VIO_KEY_PAGE_UP}, {0x7C, VIO_KEY_RIGHT},
        {0x3D, VIO_KEY_RIGHT_ALT}, {0x3E, VIO_KEY_RIGHT_CONTROL}, {0x3C, VIO_KEY_RIGHT_SHIFT},
        {0x36, VIO_KEY_RIGHT_SUPER}, {0x31, VIO_KEY_SPACE}, {0x30, VIO_KEY_TAB}, {0x7E, VIO_KEY_UP},
        {0x52, VIO_KEY_KP_0}, {0x53, VIO_KEY_KP_1}, {0x54, VIO_KEY_KP_2}, {0x55, VIO_KEY_KP_3}, {0x56, VIO_KEY_KP_4},
        {0x57, VIO_KEY_KP_5}, {0x58, VIO_KEY_KP_6}, {0x59, VIO_KEY_KP_7}, {0x5B, VIO_KEY_KP_8}, {0x5C, VIO_KEY_KP_9},
        {0x45, VIO_KEY_KP_ADD}, {0x41, VIO_KEY_KP_DECIMAL}, {0x4B, VIO_KEY_KP_DIVIDE}, {0x4C, VIO_KEY_KP_ENTER},
        {0x51, VIO_KEY_KP_EQUAL}, {0x43, VIO_KEY_KP_MULTIPLY}, {0x4E, VIO_KEY_KP_SUBTRACT},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) cocoa_keycodes[t[i].kc] = (short)t[i].key;
}

static int cocoa_mods(NSEventModifierFlags f)
{
    int m = 0;
    if (f & NSEventModifierFlagShift)    m |= VIO_MOD_SHIFT;
    if (f & NSEventModifierFlagControl)  m |= VIO_MOD_CONTROL;
    if (f & NSEventModifierFlagOption)   m |= VIO_MOD_ALT;
    if (f & NSEventModifierFlagCommand)  m |= VIO_MOD_SUPER;
    if (f & NSEventModifierFlagCapsLock) m |= VIO_MOD_CAPS_LOCK;
    return m;
}

/* ── Window state ─────────────────────────────────────────────────── */

struct _vio_cocoa_window {
    void            *window;     /* NSWindow, retained */
    void            *view;       /* VioCocoaView, retained */
    void            *delegate;   /* VioCocoaDelegate, retained */
    void            *glctx;      /* NSOpenGLContext, retained */
    int              should_close;
    int              headless;
    int              decorated;
    int              auto_iconify;
    int              cursor_mode;
    double           cursor_x, cursor_y;
    int              fb_w, fb_h;
    int              monitor;
    CGDirectDisplayID mode_display;
    CGDisplayModeRef saved_mode;
    NSRect           windowed_frame;
    unsigned char    keys_down[256];
    vio_input_state *input;
};

static vio_input_state *cocoa_input(vio_cocoa_window *w)
{
    return (w && w->input && !w->input->replaying) ? w->input : NULL;
}

#define COCOA_WIN(w)  ((__bridge NSWindow *)(w)->window)
#define COCOA_VIEW(w) ((__bridge NSView *)(w)->view)

static void cocoa_report_size(vio_cocoa_window *w)
{
    NSRect px = [COCOA_VIEW(w) convertRectToBacking:[COCOA_VIEW(w) bounds]];
    int fw = (int)px.size.width, fh = (int)px.size.height;
    if (w->glctx) [(__bridge NSOpenGLContext *)w->glctx update];
    if (fw == w->fb_w && fh == w->fb_h) return;
    w->fb_w = fw;
    w->fb_h = fh;
    vio_input_resize_event(w->input, fw, fh);
}

/* ── View: keys, text, mouse ──────────────────────────────────────── */

@interface VioCocoaView : NSView
@property (nonatomic, assign) vio_cocoa_window *owner;
@end

@implementation VioCocoaView

- (BOOL)isFlipped { return YES; }   /* y from the top, like GLFW's cursor positions */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (void)keyDown:(NSEvent *)event
{
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    unsigned short kc = event.keyCode;
    int key = kc < 256 ? cocoa_keycodes[kc] : VIO_KEY_UNKNOWN;
    int action = (kc < 256 && w->keys_down[kc]) || event.isARepeat ? VIO_REPEAT : VIO_PRESS;
    if (kc < 256) w->keys_down[kc] = 1;
    vio_input_key_event(cocoa_input(w), key, action, cocoa_mods(event.modifierFlags));
    if (event.modifierFlags & NSEventModifierFlagCommand) return;   /* shortcuts carry no text */
    NSString *chars = event.characters;
    NSUInteger n = chars.length;
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [chars characterAtIndex:i];
        unsigned int cp = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            unichar lo = [chars characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (cp < 32 || cp == 127 || (cp >= 0xF700 && cp <= 0xF8FF)) continue;   /* control and function keys */
        vio_input_emit_char(cocoa_input(w), cp);
    }
}

- (void)keyUp:(NSEvent *)event
{
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    unsigned short kc = event.keyCode;
    if (kc < 256) w->keys_down[kc] = 0;
    vio_input_key_event(cocoa_input(w), kc < 256 ? cocoa_keycodes[kc] : VIO_KEY_UNKNOWN, VIO_RELEASE,
                        cocoa_mods(event.modifierFlags));
}

/* Modifier keys only report a changed flag set: a press when the key's
 * state flips to down, a release when it flips back. */
- (void)flagsChanged:(NSEvent *)event
{
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    unsigned short kc = event.keyCode;
    if (kc >= 256) return;
    int down = !w->keys_down[kc];
    w->keys_down[kc] = (unsigned char)down;
    vio_input_key_event(cocoa_input(w), cocoa_keycodes[kc], down ? VIO_PRESS : VIO_RELEASE, cocoa_mods(event.modifierFlags));
}

- (void)cursor:(NSEvent *)event
{
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        w->cursor_x += event.deltaX;
        w->cursor_y += event.deltaY;
    } else {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        w->cursor_x = p.x;
        w->cursor_y = p.y;
    }
    vio_input_cursor_event(cocoa_input(w), w->cursor_x, w->cursor_y);
}

- (void)mouseMoved:(NSEvent *)event        { [self cursor:event]; }
- (void)mouseDragged:(NSEvent *)event      { [self cursor:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self cursor:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self cursor:event]; }

- (void)button:(int)b action:(int)action
{
    if (b >= 0 && b <= VIO_MOUSE_LAST) vio_input_button_event(cocoa_input(self.owner), b, action);
}

- (void)mouseDown:(NSEvent *)event       { (void)event; [self button:0 action:VIO_PRESS]; }
- (void)mouseUp:(NSEvent *)event         { (void)event; [self button:0 action:VIO_RELEASE]; }
- (void)rightMouseDown:(NSEvent *)event  { (void)event; [self button:1 action:VIO_PRESS]; }
- (void)rightMouseUp:(NSEvent *)event    { (void)event; [self button:1 action:VIO_RELEASE]; }
- (void)otherMouseDown:(NSEvent *)event  { [self button:(int)event.buttonNumber action:VIO_PRESS]; }
- (void)otherMouseUp:(NSEvent *)event    { [self button:(int)event.buttonNumber action:VIO_RELEASE]; }

- (void)scrollWheel:(NSEvent *)event
{
    double dx = event.scrollingDeltaX, dy = event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) { dx *= 0.1; dy *= 0.1; }   /* trackpads report pixels (GLFW's factor) */
    if (dx != 0.0 || dy != 0.0) vio_input_scroll_event(cocoa_input(self.owner), dx, dy);
}

- (void)updateTrackingAreas
{
    for (NSTrackingArea *a in [self.trackingAreas copy]) [self removeTrackingArea:a];
    NSTrackingArea *a = [[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:a];
    [super updateTrackingAreas];
}

@end

@interface VioCocoaDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) vio_cocoa_window *owner;
@end

@implementation VioCocoaDelegate
- (BOOL)windowShouldClose:(id)sender { (void)sender; if (self.owner) self.owner->should_close = 1; return NO; }
- (void)windowDidResize:(NSNotification *)n { (void)n; if (self.owner) cocoa_report_size(self.owner); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; if (self.owner) cocoa_report_size(self.owner); }
- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    vio_cocoa_window *w = self.owner;
    if (w && w->monitor >= 0 && w->auto_iconify) [COCOA_WIN(w) miniaturize:nil];
}
@end

/* ── Process lifecycle ────────────────────────────────────────────── */

static int cocoa_initialized;

static int cocoa_init(void)
{
    if (cocoa_initialized) return 1;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   /* Regular once a window is shown */
        [NSApp finishLaunching];
        cocoa_keytable_init();
    }
    cocoa_initialized = 1;
    return 1;
}

static void cocoa_shutdown(void)
{
    cocoa_initialized = 0;
}

/* ── GL ───────────────────────────────────────────────────────────── */

static CFBundleRef cocoa_gl_bundle;

static void *cocoa_gl_get_proc_address(const char *name)
{
    if (!cocoa_gl_bundle) cocoa_gl_bundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.opengl"));
    if (!cocoa_gl_bundle) return NULL;
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingASCII);
    void *p = CFBundleGetFunctionPointerForName(cocoa_gl_bundle, s);
    CFRelease(s);
    return p;
}

static NSOpenGLContext *cocoa_gl_create(int samples)
{
    static const NSOpenGLPixelFormatAttribute profiles[] = { NSOpenGLProfileVersion4_1Core, NSOpenGLProfileVersion3_2Core };
    for (int p = 0; p < 2; p++) {
        for (int ms = samples > 1 ? 1 : 0; ms >= 0; ms--) {
            NSOpenGLPixelFormatAttribute attrs[32];
            int n = 0;
            attrs[n++] = NSOpenGLPFAAccelerated;
            attrs[n++] = NSOpenGLPFADoubleBuffer;
            attrs[n++] = NSOpenGLPFAOpenGLProfile; attrs[n++] = profiles[p];
            attrs[n++] = NSOpenGLPFAColorSize;     attrs[n++] = 24;
            attrs[n++] = NSOpenGLPFAAlphaSize;     attrs[n++] = 8;
            attrs[n++] = NSOpenGLPFADepthSize;     attrs[n++] = 24;
            attrs[n++] = NSOpenGLPFAStencilSize;   attrs[n++] = 8;
            if (ms) {
                attrs[n++] = NSOpenGLPFAMultisample;
                attrs[n++] = NSOpenGLPFASampleBuffers; attrs[n++] = 1;
                attrs[n++] = NSOpenGLPFASamples;       attrs[n++] = (NSOpenGLPixelFormatAttribute)samples;
            }
            attrs[n] = 0;
            NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
            if (!pf) continue;
            NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
            if (ctx) return ctx;
        }
    }
    return nil;
}

/* ── Monitors ─────────────────────────────────────────────────────── */

static NSArray<NSScreen *> *cocoa_screens(void)
{
    /* the main display first, as GLFW orders them */
    NSMutableArray *list = [NSMutableArray arrayWithArray:[NSScreen screens]];
    CGDirectDisplayID main = CGMainDisplayID();
    for (NSUInteger i = 0; i < list.count; i++) {
        NSScreen *s = list[i];
        if ([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == main && i > 0) {
            [list removeObjectAtIndex:i];
            [list insertObject:s atIndex:0];
            break;
        }
    }
    return list;
}

static CGDirectDisplayID cocoa_display(NSScreen *s)
{
    return [s.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

/* Cocoa's y grows upwards from the main display's bottom; GLFW's from its top. */
static double cocoa_flip_y(double y)
{
    return CGDisplayBounds(CGMainDisplayID()).size.height - y;
}

static int cocoa_mode_refresh(CGDisplayModeRef m)
{
    double r = CGDisplayModeGetRefreshRate(m);
    return r > 0.0 ? (int)lround(r) : 60;   /* built-in panels report 0 */
}

static int cocoa_monitor_count(void)
{
    @autoreleasepool { return (int)[NSScreen screens].count; }
}

static int cocoa_primary_monitor(void)
{
    return cocoa_monitor_count() > 0 ? 0 : -1;
}

static int cocoa_monitor_desc(int index, vio_monitor_desc *out)
{
    memset(out, 0, sizeof(*out));
    @autoreleasepool {
        NSArray<NSScreen *> *list = cocoa_screens();
        if (index < 0 || index >= (int)list.count) return -1;
        NSScreen *s = list[(NSUInteger)index];
        CGDirectDisplayID d = cocoa_display(s);
        NSString *name = nil;
        if (@available(macOS 10.15, *)) name = s.localizedName;
        snprintf(out->name, sizeof(out->name), "%s", name ? name.UTF8String : "Display");
        CGRect b = CGDisplayBounds(d);
        out->x = (int)b.origin.x;
        out->y = (int)b.origin.y;
        NSRect vf = s.visibleFrame;
        out->work_x = (int)vf.origin.x;
        out->work_y = (int)cocoa_flip_y(vf.origin.y + vf.size.height);
        out->work_width = (int)vf.size.width;
        out->work_height = (int)vf.size.height;
        out->scale_x = out->scale_y = (float)s.backingScaleFactor;
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        out->mode.width = m ? (int)CGDisplayModeGetWidth(m) : (int)b.size.width;
        out->mode.height = m ? (int)CGDisplayModeGetHeight(m) : (int)b.size.height;
        out->mode.refresh_hz = m ? cocoa_mode_refresh(m) : 60;
        if (m) CGDisplayModeRelease(m);
        out->mode.red_bits = out->mode.green_bits = out->mode.blue_bits = 8;
        out->primary = d == CGMainDisplayID();
    }
    return 0;
}

static int cocoa_mode_cmp(const void *a, const void *b)
{
    const vio_video_mode *x = (const vio_video_mode *)a, *y = (const vio_video_mode *)b;
    long ax = (long)x->width * x->height, ay = (long)y->width * y->height;
    if (ax != ay) return ax < ay ? -1 : 1;
    if (x->width != y->width) return x->width - y->width;
    return x->refresh_hz - y->refresh_hz;
}

static int cocoa_video_modes(int index, vio_video_mode *out, int max)
{
    int n = 0;
    @autoreleasepool {
        NSArray<NSScreen *> *list = cocoa_screens();
        if (index < 0 || index >= (int)list.count || max <= 0) return 0;
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(cocoa_display(list[(NSUInteger)index]), NULL);
        for (CFIndex i = 0; modes && i < CFArrayGetCount(modes) && n < max; i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (!CGDisplayModeIsUsableForDesktopGUI(m)) continue;
            vio_video_mode v;
            v.width = (int)CGDisplayModeGetWidth(m);
            v.height = (int)CGDisplayModeGetHeight(m);
            v.refresh_hz = cocoa_mode_refresh(m);
            v.red_bits = v.green_bits = v.blue_bits = 8;
            int dup = 0;
            for (int k = 0; k < n && !dup; k++) dup = memcmp(&out[k], &v, sizeof(v)) == 0;
            if (!dup) out[n++] = v;
        }
        if (modes) CFRelease(modes);
    }
    qsort(out, (size_t)n, sizeof(out[0]), cocoa_mode_cmp);
    return n;
}

/* ── Window lifecycle ─────────────────────────────────────────────── */

static NSWindowStyleMask cocoa_style(vio_cocoa_window *w)
{
    if (w->monitor >= 0 || w->headless || !w->decorated) return NSWindowStyleMaskBorderless;
    return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
}

static vio_window_handle cocoa_create_window(vio_config *cfg, const char *backend_name)
{
    if (!cocoa_init()) return NULL;
    vio_cocoa_window *w = (vio_cocoa_window *)calloc(1, sizeof(vio_cocoa_window));
    if (!w) return NULL;
    w->headless = cfg->headless;
    w->decorated = !cfg->headless;
    w->monitor = -1;
    w->fb_w = w->fb_h = -1;
    @autoreleasepool {
        int width  = cfg->width  > 0 ? cfg->width  : 800;
        int height = cfg->height > 0 ? cfg->height : 600;
        NSRect rect = NSMakeRect(0, 0, width, height);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:rect styleMask:cocoa_style(w)
                                                      backing:NSBackingStoreBuffered defer:NO];
        if (!win) { free(w); return NULL; }
        win.releasedWhenClosed = NO;
        win.title = [NSString stringWithUTF8String:cfg->title ? cfg->title : "php-vio"];
        win.acceptsMouseMovedEvents = YES;
        VioCocoaView *view = [[VioCocoaView alloc] initWithFrame:rect];
        view.owner = w;
        view.wantsBestResolutionOpenGLSurface = YES;
        win.contentView = view;
        [win makeFirstResponder:view];
        VioCocoaDelegate *del = [VioCocoaDelegate new];
        del.owner = w;
        win.delegate = del;
        w->window = (void *)CFBridgingRetain(win);
        w->view = (void *)CFBridgingRetain(view);
        w->delegate = (void *)CFBridgingRetain(del);

        if (backend_name && strcmp(backend_name, "opengl") == 0) {
            NSOpenGLContext *ctx = cocoa_gl_create(cfg->samples);
            if (!ctx) {
                php_error_docref(NULL, E_WARNING, "Cocoa: no OpenGL context >= 3.2 core available");
                win.delegate = nil;
                CFRelease(w->delegate); CFRelease(w->view); CFRelease(w->window);
                free(w);
                return NULL;
            }
            ctx.view = view;
            [ctx makeCurrentContext];
            GLint interval = cfg->vsync ? 1 : 0;
            [ctx setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
            w->glctx = (void *)CFBridgingRetain(ctx);
        }

        if (!cfg->headless) {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [win center];
            [win makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
        NSRect px = [view convertRectToBacking:view.bounds];
        w->fb_w = (int)px.size.width;
        w->fb_h = (int)px.size.height;
    }
    return w;
}

static void cocoa_restore_mode(vio_cocoa_window *w)
{
    if (!w->saved_mode) return;
    CGDisplaySetDisplayMode(w->mode_display, w->saved_mode, NULL);
    CGDisplayModeRelease(w->saved_mode);
    w->saved_mode = NULL;
}

static void cocoa_destroy_window(vio_window_handle h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return;
    @autoreleasepool {
        cocoa_restore_mode(w);
        if (w->cursor_mode != VIO_PLATFORM_CURSOR_NORMAL) {
            CGAssociateMouseAndMouseCursorPosition(true);
            [NSCursor unhide];
        }
        if (w->glctx) {
            NSOpenGLContext *ctx = (__bridge NSOpenGLContext *)w->glctx;
            if ([NSOpenGLContext currentContext] == ctx) [NSOpenGLContext clearCurrentContext];
            [ctx clearDrawable];
            CFRelease(w->glctx);
        }
        NSWindow *win = COCOA_WIN(w);
        ((__bridge VioCocoaView *)w->view).owner = NULL;
        ((__bridge VioCocoaDelegate *)w->delegate).owner = NULL;
        win.delegate = nil;
        [win orderOut:nil];
        [win close];
        CFRelease(w->delegate);
        CFRelease(w->view);
        CFRelease(w->window);
    }
    free(w);
}

static int  cocoa_should_close(vio_window_handle h) { return h ? ((vio_cocoa_window *)h)->should_close : 1; }
static void cocoa_set_should_close(vio_window_handle h, int v) { if (h) ((vio_cocoa_window *)h)->should_close = v; }

static void cocoa_poll_events(void)
{
    if (!cocoa_initialized) return;
    @autoreleasepool {
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
        }
    }
}

static void cocoa_wait_events(void)
{
    if (!cocoa_initialized) return;
    @autoreleasepool {
        NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantFuture]
                                            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
    }
    cocoa_poll_events();
}

static void cocoa_swap_buffers(vio_window_handle h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (w && w->glctx) [(__bridge NSOpenGLContext *)w->glctx flushBuffer];
}

static void cocoa_get_framebuffer_size(vio_window_handle h, int *fw, int *fh)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    int x = 0, y = 0;
    if (w) {
        @autoreleasepool {
            NSRect px = [COCOA_VIEW(w) convertRectToBacking:COCOA_VIEW(w).bounds];
            x = (int)px.size.width;
            y = (int)px.size.height;
        }
    }
    if (fw) *fw = x;
    if (fh) *fh = y;
}

static void cocoa_get_window_size(vio_window_handle h, int *ww, int *wh)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    NSRect r = w ? COCOA_VIEW(w).bounds : NSZeroRect;
    if (ww) *ww = (int)r.size.width;
    if (wh) *wh = (int)r.size.height;
}

static void cocoa_get_content_scale(vio_window_handle h, float *sx, float *sy)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    float s = w ? (float)COCOA_WIN(w).backingScaleFactor : 1.0f;
    if (sx) *sx = s;
    if (sy) *sy = s;
}

static void cocoa_get_cursor_pos(vio_window_handle h, double *x, double *y)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return;
    if (w->cursor_mode != VIO_PLATFORM_CURSOR_DISABLED) {
        NSPoint p = [COCOA_WIN(w) mouseLocationOutsideOfEventStream];
        p = [COCOA_VIEW(w) convertPoint:p fromView:nil];
        w->cursor_x = p.x;
        w->cursor_y = p.y;
    }
    if (x) *x = w->cursor_x;
    if (y) *y = w->cursor_y;
}

static int cocoa_gl_make_current(vio_window_handle h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w || !w->glctx) { [NSOpenGLContext clearCurrentContext]; return 0; }
    [(__bridge NSOpenGLContext *)w->glctx makeCurrentContext];
    return 0;
}

static void cocoa_gl_swap_interval(int interval)
{
    NSOpenGLContext *ctx = [NSOpenGLContext currentContext];
    GLint v = interval;
    if (ctx) [ctx setValues:&v forParameter:NSOpenGLContextParameterSwapInterval];
}

/* ── Window properties ────────────────────────────────────────────── */

static void cocoa_set_title(vio_window_handle h, const char *utf8)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (w && utf8) { @autoreleasepool { COCOA_WIN(w).title = [NSString stringWithUTF8String:utf8]; } }
}

static void cocoa_set_window_size(vio_window_handle h, int ww, int wh)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w || w->monitor >= 0 || ww <= 0 || wh <= 0) return;
    NSWindow *win = COCOA_WIN(w);
    NSRect content = [win contentRectForFrameRect:win.frame];
    content.origin.y += content.size.height - wh;   /* keep the top-left corner */
    content.size = NSMakeSize(ww, wh);
    [win setFrame:[win frameRectForContentRect:content] display:YES];
    cocoa_report_size(w);
}

static void cocoa_get_window_pos(vio_window_handle h, int *x, int *y)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    NSRect c = w ? [COCOA_WIN(w) contentRectForFrameRect:COCOA_WIN(w).frame] : NSZeroRect;
    if (x) *x = (int)c.origin.x;
    if (y) *y = (int)cocoa_flip_y(c.origin.y + c.size.height);
}

static void cocoa_set_window_pos(vio_window_handle h, int x, int y)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w || w->monitor >= 0) return;
    NSWindow *win = COCOA_WIN(w);
    NSRect c = [win contentRectForFrameRect:win.frame];
    c.origin = NSMakePoint(x, cocoa_flip_y(y + c.size.height));
    [win setFrameOrigin:[win frameRectForContentRect:c].origin];
}

static int cocoa_get_attrib(vio_window_handle h, vio_window_attrib a)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    switch (a) {
        case VIO_WINDOW_DECORATED:    return w->decorated;
        case VIO_WINDOW_MAXIMIZED:    return COCOA_WIN(w).zoomed ? 1 : 0;
        case VIO_WINDOW_AUTO_ICONIFY: return w->auto_iconify;
        case VIO_WINDOW_FOCUSED:      return COCOA_WIN(w).keyWindow ? 1 : 0;
        case VIO_WINDOW_VISIBLE:      return COCOA_WIN(w).visible ? 1 : 0;
    }
    return 0;
}

static void cocoa_apply_style(vio_cocoa_window *w)
{
    NSWindow *win = COCOA_WIN(w);
    NSRect content = [win contentRectForFrameRect:win.frame];
    win.styleMask = cocoa_style(w);
    [win setFrame:[win frameRectForContentRect:content] display:YES];
    [win makeFirstResponder:COCOA_VIEW(w)];
}

static void cocoa_set_attrib(vio_window_handle h, vio_window_attrib a, int value)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return;
    switch (a) {
        case VIO_WINDOW_DECORATED:
            w->decorated = value ? 1 : 0;
            if (w->monitor < 0 && !w->headless) cocoa_apply_style(w);
            break;
        case VIO_WINDOW_AUTO_ICONIFY:
            w->auto_iconify = value ? 1 : 0;
            break;
        case VIO_WINDOW_VISIBLE:
            if (value) [COCOA_WIN(w) orderFront:nil]; else [COCOA_WIN(w) orderOut:nil];
            break;
        default:
            break;
    }
}

static void cocoa_maximize(vio_window_handle h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (w && !COCOA_WIN(w).zoomed) [COCOA_WIN(w) zoom:nil];
}

static void cocoa_restore(vio_window_handle h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return;
    if (COCOA_WIN(w).miniaturized) [COCOA_WIN(w) deminiaturize:nil];
    else if (COCOA_WIN(w).zoomed) [COCOA_WIN(w) zoom:nil];
}

static void cocoa_set_cursor_mode(vio_window_handle h, int mode)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w || w->cursor_mode == mode) return;
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_NORMAL) [NSCursor hide];
    if (mode == VIO_PLATFORM_CURSOR_NORMAL) [NSCursor unhide];
    if (mode == VIO_PLATFORM_CURSOR_DISABLED) {
        double x = 0, y = 0;
        cocoa_get_cursor_pos(w, &x, &y);
        CGAssociateMouseAndMouseCursorPosition(false);   /* deltas only, the pointer stays */
    } else if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        CGAssociateMouseAndMouseCursorPosition(true);
    }
    w->cursor_mode = mode;
}

static int cocoa_window_monitor(vio_window_handle h)
{
    return h ? ((vio_cocoa_window *)h)->monitor : -1;
}

static void cocoa_set_window_monitor(vio_window_handle h, int monitor, int x, int y, int width, int height, int refresh_hz)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return;
    @autoreleasepool {
        NSWindow *win = COCOA_WIN(w);
        if (monitor >= 0) {
            NSArray<NSScreen *> *list = cocoa_screens();
            if (list.count == 0) return;
            if (monitor >= (int)list.count) monitor = 0;
            NSScreen *s = list[(NSUInteger)monitor];
            CGDirectDisplayID d = cocoa_display(s);
            if (w->monitor < 0) w->windowed_frame = win.frame;
            /* the closest mode, when the requested one differs from the current */
            CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
            if (cur && ((int)CGDisplayModeGetWidth(cur) != width || (int)CGDisplayModeGetHeight(cur) != height)) {
                CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, NULL);
                CGDisplayModeRef best = NULL;
                long best_score = -1;
                for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
                    CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
                    if (!CGDisplayModeIsUsableForDesktopGUI(m)) continue;
                    long score = labs((long)CGDisplayModeGetWidth(m) - width) * 1000 + labs((long)CGDisplayModeGetHeight(m) - height) * 1000
                               + (refresh_hz > 0 ? labs((long)cocoa_mode_refresh(m) - refresh_hz) : 0);
                    if (best_score < 0 || score < best_score) { best_score = score; best = m; }
                }
                if (best) {
                    if (!w->saved_mode) { w->saved_mode = CGDisplayModeRetain(cur); w->mode_display = d; }
                    CGDisplaySetDisplayMode(d, best, NULL);
                }
                if (modes) CFRelease(modes);
            }
            if (cur) CGDisplayModeRelease(cur);
            w->monitor = monitor;
            win.styleMask = NSWindowStyleMaskBorderless;
            win.level = NSMainMenuWindowLevel + 1;
            [win setFrame:s.frame display:YES];
            [win makeFirstResponder:COCOA_VIEW(w)];
            [win makeKeyAndOrderFront:nil];
            cocoa_report_size(w);
            return;
        }
        cocoa_restore_mode(w);
        w->monitor = -1;
        win.level = NSNormalWindowLevel;
        win.styleMask = cocoa_style(w);
        NSRect c = NSMakeRect(x, cocoa_flip_y(y + height), width > 0 ? width : 1, height > 0 ? height : 1);
        [win setFrame:[win frameRectForContentRect:c] display:YES];
        [win makeFirstResponder:COCOA_VIEW(w)];
        cocoa_report_size(w);
    }
}

/* ── Gamepads: GameController ─────────────────────────────────────── */

static unsigned char cocoa_pad_buttons[VIO_PLATFORM_JOYSTICK_LAST + 1][15];
static float         cocoa_pad_axes[VIO_PLATFORM_JOYSTICK_LAST + 1][6];
static char          cocoa_pad_names[VIO_PLATFORM_JOYSTICK_LAST + 1][128];

static GCExtendedGamepad *cocoa_pad(int id)
{
    if (id < 0 || id > VIO_PLATFORM_JOYSTICK_LAST) return nil;
    NSArray<GCController *> *list = [GCController controllers];
    int k = 0;
    for (GCController *c in list) {
        if (!c.extendedGamepad) continue;
        if (k++ == id) {
            snprintf(cocoa_pad_names[id], sizeof(cocoa_pad_names[id]), "%s",
                     c.vendorName ? c.vendorName.UTF8String : "Gamepad");
            return c.extendedGamepad;
        }
    }
    return nil;
}

static int cocoa_joystick_present(int id)
{
    @autoreleasepool { return cocoa_pad(id) != nil; }
}

static const char *cocoa_pad_name(int id)
{
    @autoreleasepool { return cocoa_pad(id) ? cocoa_pad_names[id] : NULL; }
}

/* GLFW's mapping: axes -1..1 with Y up = -1 (GameController reports up = +1), triggers -1..1. */
static int cocoa_gamepad_state(int id, unsigned char buttons[15], float axes[6])
{
    @autoreleasepool {
        GCExtendedGamepad *g = cocoa_pad(id);
        if (!g) return 0;
        memset(buttons, 0, 15);
        buttons[0] = g.buttonA.pressed;
        buttons[1] = g.buttonB.pressed;
        buttons[2] = g.buttonX.pressed;
        buttons[3] = g.buttonY.pressed;
        buttons[4] = g.leftShoulder.pressed;
        buttons[5] = g.rightShoulder.pressed;
        if (@available(macOS 10.15, *)) {
            buttons[6] = g.buttonOptions ? g.buttonOptions.pressed : 0;
            buttons[7] = g.buttonMenu.pressed;
        }
        if (@available(macOS 11.0, *)) buttons[8] = g.buttonHome ? g.buttonHome.pressed : 0;
        if (@available(macOS 10.14.1, *)) {
            buttons[9] = g.leftThumbstickButton ? g.leftThumbstickButton.pressed : 0;
            buttons[10] = g.rightThumbstickButton ? g.rightThumbstickButton.pressed : 0;
        }
        buttons[11] = g.dpad.up.pressed;
        buttons[12] = g.dpad.right.pressed;
        buttons[13] = g.dpad.down.pressed;
        buttons[14] = g.dpad.left.pressed;
        axes[0] = g.leftThumbstick.xAxis.value;
        axes[1] = -g.leftThumbstick.yAxis.value;
        axes[2] = g.rightThumbstick.xAxis.value;
        axes[3] = -g.rightThumbstick.yAxis.value;
        axes[4] = g.leftTrigger.value * 2.0f - 1.0f;
        axes[5] = g.rightTrigger.value * 2.0f - 1.0f;
        return 1;
    }
}

static const unsigned char *cocoa_joystick_buttons(int id, int *count)
{
    *count = 0;
    if (id < 0 || id > VIO_PLATFORM_JOYSTICK_LAST || !cocoa_gamepad_state(id, cocoa_pad_buttons[id], cocoa_pad_axes[id])) return NULL;
    *count = 15;
    return cocoa_pad_buttons[id];
}

static const float *cocoa_joystick_axes(int id, int *count)
{
    *count = 0;
    if (id < 0 || id > VIO_PLATFORM_JOYSTICK_LAST || !cocoa_gamepad_state(id, cocoa_pad_buttons[id], cocoa_pad_axes[id])) return NULL;
    *count = 6;
    return cocoa_pad_axes[id];
}

/* ── Native handles, Vulkan, input ────────────────────────────────── */

static void *cocoa_native_handle(vio_window_handle h, vio_native_kind kind)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return NULL;
    return kind == VIO_NATIVE_NSWINDOW ? w->window : NULL;
}

#ifdef HAVE_VULKAN
static const char **cocoa_vk_instance_extensions(uint32_t *count)
{
    static const char *ext[] = { "VK_KHR_surface", "VK_EXT_metal_surface" };
    *count = 2;
    return ext;
}

static int cocoa_vk_create_surface(vio_window_handle h, void *instance, void *out_surface)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return (int)VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateMetalSurfaceEXT create = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr((VkInstance)instance, "vkCreateMetalSurfaceEXT");
    if (!create) return (int)VK_ERROR_EXTENSION_NOT_PRESENT;
    NSView *view = COCOA_VIEW(w);
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.contentsScale = COCOA_WIN(w).backingScaleFactor;
    view.wantsLayer = YES;
    view.layer = layer;
    VkMetalSurfaceCreateInfoEXT ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    ci.pLayer = layer;
    return (int)create((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface);
}
#endif

static void cocoa_install_input(vio_window_handle h, vio_input_state *state)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (w) w->input = state;
}

static const vio_platform vio_platform_cocoa = {
    .name                 = "cocoa",
    .init                 = cocoa_init,
    .shutdown             = cocoa_shutdown,
    .create_window        = cocoa_create_window,
    .destroy_window       = cocoa_destroy_window,
    .should_close         = cocoa_should_close,
    .set_should_close     = cocoa_set_should_close,
    .poll_events          = cocoa_poll_events,
    .wait_events          = cocoa_wait_events,
    .swap_buffers         = cocoa_swap_buffers,
    .get_framebuffer_size = cocoa_get_framebuffer_size,
    .get_window_size      = cocoa_get_window_size,
    .get_content_scale    = cocoa_get_content_scale,
    .get_cursor_pos       = cocoa_get_cursor_pos,
    .gl_make_current      = cocoa_gl_make_current,
    .gl_swap_interval     = cocoa_gl_swap_interval,
    .gl_get_proc_address  = cocoa_gl_get_proc_address,
    .set_title            = cocoa_set_title,
    .set_window_size      = cocoa_set_window_size,
    .get_window_pos       = cocoa_get_window_pos,
    .set_window_pos       = cocoa_set_window_pos,
    .get_attrib           = cocoa_get_attrib,
    .set_attrib           = cocoa_set_attrib,
    .maximize             = cocoa_maximize,
    .restore              = cocoa_restore,
    .set_cursor_mode      = cocoa_set_cursor_mode,
    .window_monitor       = cocoa_window_monitor,
    .set_window_monitor   = cocoa_set_window_monitor,
    .monitor_count        = cocoa_monitor_count,
    .primary_monitor      = cocoa_primary_monitor,
    .monitor_desc         = cocoa_monitor_desc,
    .video_modes          = cocoa_video_modes,
    .joystick_present     = cocoa_joystick_present,
    .joystick_is_gamepad  = cocoa_joystick_present,
    .joystick_name        = cocoa_pad_name,
    .gamepad_name         = cocoa_pad_name,
    .gamepad_state        = cocoa_gamepad_state,
    .joystick_buttons     = cocoa_joystick_buttons,
    .joystick_axes        = cocoa_joystick_axes,
    .native_handle        = cocoa_native_handle,
#ifdef HAVE_VULKAN
    .vk_instance_extensions = cocoa_vk_instance_extensions,
    .vk_create_surface      = cocoa_vk_create_surface,
#endif
    .install_input        = cocoa_install_input,
};

void vio_platform_cocoa_register(void)
{
    vio_register_platform(&vio_platform_cocoa);
}

#endif /* HAVE_COCOA && !HAVE_IOS */
