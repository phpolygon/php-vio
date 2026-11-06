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
#include <objc/message.h>
#include <dlfcn.h>
#import <UserNotifications/UserNotifications.h>   /* used through NSClassFromString: the module does not link it */
#include <float.h>
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
    void            *cursor;     /* NSCursor for the view, retained; NULL = arrow */
    void            *menu_target;   /* VioCocoaMenuTarget of the menu bar, retained */
    int              ime_off;       /* vio_ime_enable(false): characters straight from the key event */
    int              ime_x, ime_y, ime_w, ime_h;   /* vio_ime_rect, view points */
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

@interface VioCocoaView : NSView <NSTextInputClient>
@property (nonatomic, assign) vio_cocoa_window *owner;
@property (nonatomic, strong) NSString *marked;   /* the IME composition, nil = none */
@end

/* UTF-16 index -> codepoint index in s */
static int cocoa_cp_index(NSString *s, NSUInteger utf16)
{
    int n = 0;
    for (NSUInteger i = 0; i < utf16 && i < s.length; i++, n++) {
        unichar c = [s characterAtIndex:i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) i++;
    }
    return n;
}

/* text -> chars, without control and function keys (as the key path) */
static void cocoa_emit_text(vio_input_state *input, NSString *chars)
{
    NSUInteger n = chars.length;
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [chars characterAtIndex:i];
        unsigned int cp = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            unichar lo = [chars characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (cp < 32 || cp == 127 || (cp >= 0xF700 && cp <= 0xF8FF)) continue;
        vio_input_emit_char(input, cp);
    }
}

@implementation VioCocoaView

- (BOOL)isFlipped { return YES; }   /* y from the top, like GLFW's cursor positions */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

/* File drops (NSDraggingDestination) */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    return [sender.draggingPasteboard canReadObjectForClasses:@[[NSURL class]]
                                                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }]
        ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    vio_cocoa_window *w = self.owner;
    NSArray<NSURL *> *urls = [sender.draggingPasteboard readObjectsForClasses:@[[NSURL class]]
                                                                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if (!w || urls.count == 0) return NO;
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];   /* flipped: y from the top */
    const char **paths = (const char **)calloc(urls.count, sizeof(char *));
    if (!paths) return NO;
    int n = 0;
    for (NSURL *u in urls) if (u.fileSystemRepresentation) paths[n++] = u.fileSystemRepresentation;
    vio_input_drop_event(w->input, n, paths, p.x, p.y);
    free(paths);
    return YES;
}

- (void)resetCursorRects
{
    vio_cocoa_window *w = self.owner;
    NSCursor *c = (w && w->cursor) ? (__bridge NSCursor *)w->cursor : [NSCursor arrowCursor];
    [self addCursorRect:self.bounds cursor:c];
}

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
    /* the input method composes (IMEs, dead keys) and answers with insertText: / setMarkedText: */
    if (!w->ime_off) { [self interpretKeyEvents:@[event]]; return; }
    cocoa_emit_text(cocoa_input(w), event.characters);
}

/* ── NSTextInputClient (IME composition, DESKTOP-APP-PLAN Phase 3) ── */
- (void)insertText:(id)string replacementRange:(NSRange)range
{
    (void)range;
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    if (self.marked) { self.marked = nil; vio_input_preedit_event(w->input, "", 0, 0); }
    cocoa_emit_text(cocoa_input(w), s);
}
- (void)doCommandBySelector:(SEL)selector { (void)selector; }   /* keys arrive as key events; no beep */
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range
{
    (void)range;
    vio_cocoa_window *w = self.owner;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    self.marked = s.length ? s : nil;
    if (!w) return;
    if (!s.length) { vio_input_preedit_event(w->input, "", 0, 0); return; }
    vio_input_preedit_event(w->input, s.UTF8String, cocoa_cp_index(s, sel.location), cocoa_cp_index(s, NSMaxRange(sel)));
}
- (void)unmarkText
{
    if (!self.marked) return;
    self.marked = nil;
    if (self.owner) vio_input_preedit_event(self.owner->input, "", 0, 0);
}
- (BOOL)hasMarkedText { return self.marked != nil; }
- (NSRange)markedRange { return self.marked ? NSMakeRange(0, self.marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(self.marked ? self.marked.length : 0, 0); }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual { (void)range; (void)actual; return nil; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    (void)range; (void)actual;
    vio_cocoa_window *w = self.owner;
    NSRect r = w ? NSMakeRect(w->ime_x, w->ime_y, w->ime_w, w->ime_h) : NSZeroRect;   /* flipped view: y from the top */
    return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
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

/* Menu actions: an item's tag goes to the window's input state; Quit asks
 * the window to close. Popups record the tag instead. */
@interface VioCocoaMenuTarget : NSObject
@property (nonatomic, assign) vio_cocoa_window *owner;
@property (nonatomic, assign) NSInteger chosen;
@property (nonatomic, assign) BOOL popup;
@property (nonatomic, assign) BOOL tray;
@end

@implementation VioCocoaMenuTarget
- (void)vioMenu:(NSMenuItem *)item
{
    if (self.popup) { self.chosen = item.tag; return; }
    if (self.tray) { vio_tray_menu_event((int)item.tag); return; }
    if (self.owner) vio_input_menu_event(self.owner->input, (int)item.tag);
}
- (void)vioTrayClick:(id)sender
{
    (void)sender;
    vio_tray_click_event();
}
- (void)vioQuit:(id)sender
{
    (void)sender;
    vio_cocoa_window *w = self.owner;
    if (w && vio_input_close_request(w->input)) w->should_close = 1;
}
@end

@interface VioCocoaDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) vio_cocoa_window *owner;
@end

@implementation VioCocoaDelegate
- (BOOL)windowShouldClose:(id)sender
{
    (void)sender;
    if (self.owner && vio_input_close_request(self.owner->input)) self.owner->should_close = 1;
    return NO;
}
- (void)windowDidResize:(NSNotification *)n
{
    (void)n;
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    cocoa_report_size(w);
    /* a live resize runs its own event loop: let the app draw from inside it */
    if (COCOA_VIEW(w).inLiveResize) vio_input_refresh_event(w->input);
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    (void)n;
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    cocoa_report_size(w);
    if (!w->headless) {   /* headless contexts report scale 1 */
        float s = (float)COCOA_WIN(w).backingScaleFactor;
        vio_input_scale_event(w->input, s, s);
    }
}
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; if (self.owner) vio_input_focus_event(self.owner->input, 1); }
- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    vio_cocoa_window *w = self.owner;
    if (!w) return;
    vio_input_focus_event(w->input, 0);
    if (w->monitor >= 0 && w->auto_iconify) [COCOA_WIN(w) miniaturize:nil];
}
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; if (self.owner) vio_input_minimize_event(self.owner->input, 1); }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; if (self.owner) vio_input_minimize_event(self.owner->input, 0); }
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
        /* Keys go through the input method (NSTextInputClient) since the IME
         * work; with press-and-hold on, holding A, E, S, ... opened the accent
         * picker over the game and stopped the character repeating. Off for this
         * process only, as games and terminals do; dead keys and IMEs stay. */
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"ApplePressAndHoldEnabled": @NO }];
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

static NSOpenGLContext *cocoa_gl_create(int samples, NSOpenGLContext *share)
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
            NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:share];
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
        [view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
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
            vio_cocoa_window *share_w = (vio_cocoa_window *)cfg->gl_share;
            NSOpenGLContext *ctx = cocoa_gl_create(cfg->samples, share_w && share_w->glctx ? (__bridge NSOpenGLContext *)share_w->glctx : nil);
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
    w->input = NULL;   /* no callbacks from the teardown (focus loss on destroy) */
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
        if (w->cursor) { CFRelease(w->cursor); w->cursor = NULL; }
        if (w->menu_target) {
            ((__bridge VioCocoaMenuTarget *)w->menu_target).owner = NULL;   /* the main menu may outlive the window */
            CFRelease(w->menu_target);
            w->menu_target = NULL;
        }
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

static void cocoa_wait_events(double timeout)
{
    if (!cocoa_initialized) { vio_platform_wait_wake_only(timeout); return; }
    if (vio_platform_wake_take()) { cocoa_poll_events(); return; }
    @autoreleasepool {
        NSDate *until = timeout < 0.0 ? [NSDate distantFuture] : [NSDate dateWithTimeIntervalSinceNow:timeout];
        NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until
                                            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
    }
    cocoa_poll_events();
    vio_platform_wake_take();
}

/* An empty application-defined event ends a nextEventMatchingMask wait. The
 * sticky flag is separate: a poll dequeues this event but leaves the flag. */
static void cocoa_wake(void)
{
    vio_platform_wake_signal();
    if (!cocoa_initialized) return;
    @autoreleasepool {
        NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSMakePoint(0, 0)
                                    modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
        [NSApp postEvent:ev atStart:YES];
    }
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

/* ── Window details (DESKTOP-APP-PLAN Phase 1) ────────────────────── */

static NSCursor *cocoa_private_cursor(const char *sel)
{
    /* GLFW's source for diagonal resize. Private: some releases answer
     * respondsToSelector and still throw, so the call is guarded. */
    SEL s = sel_registerName(sel);
    if (![NSCursor respondsToSelector:s]) return nil;
    @try {
        id c = ((id (*)(id, SEL))objc_msgSend)([NSCursor class], s);
        return [c isKindOfClass:[NSCursor class]] ? c : nil;
    } @catch (NSException *e) {
        (void)e;
        return nil;
    }
}

static void cocoa_apply_cursor(vio_cocoa_window *w, NSCursor *c)
{
    if (w->cursor) CFRelease(w->cursor);
    w->cursor = c ? (void *)CFBridgingRetain(c) : NULL;
    [COCOA_WIN(w) invalidateCursorRectsForView:COCOA_VIEW(w)];
    /* the cursor rects only switch on the next mouse move: set it now when inside */
    NSPoint p = [COCOA_WIN(w) mouseLocationOutsideOfEventStream];
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_NORMAL && NSPointInRect(p, [COCOA_VIEW(w) frame])) [(c ? c : [NSCursor arrowCursor]) set];
}

static int cocoa_set_cursor_shape(vio_window_handle h, int shape)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    @autoreleasepool {
        NSCursor *c = nil;
        switch (shape) {
            case VIO_PLATFORM_SHAPE_IBEAM:       c = [NSCursor IBeamCursor]; break;
            case VIO_PLATFORM_SHAPE_CROSSHAIR:   c = [NSCursor crosshairCursor]; break;
            case VIO_PLATFORM_SHAPE_HAND:        c = [NSCursor pointingHandCursor]; break;
            case VIO_PLATFORM_SHAPE_RESIZE_EW:   c = [NSCursor resizeLeftRightCursor]; break;
            case VIO_PLATFORM_SHAPE_RESIZE_NS:   c = [NSCursor resizeUpDownCursor]; break;
            case VIO_PLATFORM_SHAPE_RESIZE_NWSE: c = cocoa_private_cursor("_windowResizeNorthWestSouthEastCursor"); break;
            case VIO_PLATFORM_SHAPE_RESIZE_NESW: c = cocoa_private_cursor("_windowResizeNorthEastSouthWestCursor"); break;
            case VIO_PLATFORM_SHAPE_RESIZE_ALL:  c = [NSCursor closedHandCursor]; break;
            case VIO_PLATFORM_SHAPE_NOT_ALLOWED: c = [NSCursor operationNotAllowedCursor]; break;
            default:                             c = [NSCursor arrowCursor]; break;
        }
        cocoa_apply_cursor(w, c ? c : [NSCursor arrowCursor]);
    }
    return 1;
}

static NSImage *cocoa_image(const unsigned char *rgba, int width, int height)
{
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width * 4 bitsPerPixel:32];
    if (!rep) return nil;
    memcpy(rep.bitmapData, rgba, (size_t)width * (size_t)height * 4);
    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [img addRepresentation:rep];
    return img;
}

static int cocoa_set_cursor_image(vio_window_handle h, const unsigned char *rgba, int width, int height, int hot_x, int hot_y)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    @autoreleasepool {
        NSImage *img = cocoa_image(rgba, width, height);
        if (!img) return 0;
        cocoa_apply_cursor(w, [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(hot_x, hot_y)]);
    }
    return 1;
}

/* Windows have no icons on macOS: the image is the application's (Dock) icon. */
static int cocoa_set_window_icon(vio_window_handle h, int count, const vio_platform_image *images)
{
    (void)h;
    @autoreleasepool {
        if (count <= 0) { [NSApp setApplicationIconImage:nil]; return 1; }
        int best = 0;
        for (int i = 1; i < count; i++) if (images[i].width > images[best].width) best = i;
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(images[best].width, images[best].height)];
        for (int i = 0; i < count; i++) {
            NSImage *one = cocoa_image(images[i].rgba, images[i].width, images[i].height);
            if (one) for (NSImageRep *r in one.representations) [img addRepresentation:r];
        }
        [NSApp setApplicationIconImage:img];
    }
    return 1;
}

static int cocoa_set_size_limits(vio_window_handle h, int min_w, int min_h, int max_w, int max_h)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    NSWindow *win = COCOA_WIN(w);
    win.contentMinSize = NSMakeSize(min_w > 0 ? min_w : 0, min_h > 0 ? min_h : 0);
    win.contentMaxSize = NSMakeSize(max_w > 0 ? max_w : FLT_MAX, max_h > 0 ? max_h : FLT_MAX);
    return 1;
}

static int cocoa_set_aspect_ratio(vio_window_handle h, int num, int den)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    if (num > 0 && den > 0) COCOA_WIN(w).contentAspectRatio = NSMakeSize(num, den);
    else COCOA_WIN(w).contentResizeIncrements = NSMakeSize(1.0, 1.0);   /* clears the ratio (as GLFW does) */
    return 1;
}

static int cocoa_set_floating(vio_window_handle h, int floating)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    COCOA_WIN(w).level = floating ? NSFloatingWindowLevel : NSNormalWindowLevel;
    return 1;
}

static int cocoa_set_opacity(vio_window_handle h, float opacity)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    COCOA_WIN(w).alphaValue = opacity;
    COCOA_WIN(w).opaque = opacity >= 1.0f;
    return 1;
}

/* VIO_OPEN_COMMAND replaces NSWorkspace with a program (`open` semantics: -R reveals). */
static int cocoa_open_url(const char *url)
{
    if (getenv("VIO_OPEN_COMMAND")) { const char *a[1] = { url }; return vio_platform_spawn_opener("open", a, 1); }
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!u) u = [NSURL URLWithString:[[NSString stringWithUTF8String:url]
                         stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLFragmentAllowedCharacterSet]]];
        return u && [[NSWorkspace sharedWorkspace] openURL:u] ? 1 : 0;
    }
}

static int cocoa_reveal_file(const char *path)
{
    if (getenv("VIO_OPEN_COMMAND")) { const char *a[2] = { "-R", path }; return vio_platform_spawn_opener("open", a, 2); }
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[u]];
    }
    return 1;
}

/* ── Dialogs (DESKTOP-APP-PLAN Phase 2) ───────────────────────────── */

/* VIO_TEST_DIALOG: dismiss the modal window after a delay (tests only). The
 * timer runs in the common modes, which include the modal panel mode. */
static void cocoa_test_dismiss(void (^dismiss)(int action))
{
    int ms = 0, action = 0;
    if (!vio_platform_test_dialog(&ms, &action)) return;
    NSTimer *t = [NSTimer timerWithTimeInterval:ms / 1000.0 repeats:NO block:^(NSTimer *timer) { (void)timer; dismiss(action); }];
    [[NSRunLoop currentRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
}

static NSString *cocoa_str(const char *utf8)
{
    return utf8 ? [NSString stringWithUTF8String:utf8] : nil;
}

static int cocoa_file_dialog(vio_window_handle h, const vio_file_dialog_desc *d, char ***out_paths, int *out_count)
{
    (void)h;
    *out_paths = NULL;
    *out_count = 0;
    if (!cocoa_initialized) return 0;
    @autoreleasepool {
        NSSavePanel *panel;
        NSOpenPanel *open = nil;
        if (d->mode == VIO_PLATFORM_DIALOG_SAVE) {
            panel = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;
        } else {
            open = [NSOpenPanel openPanel];
            open.canChooseFiles = d->mode == VIO_PLATFORM_DIALOG_OPEN;
            open.canChooseDirectories = d->mode == VIO_PLATFORM_DIALOG_FOLDER;
            open.allowsMultipleSelection = d->multiple ? YES : NO;
            open.canCreateDirectories = d->mode == VIO_PLATFORM_DIALOG_FOLDER;
            panel = open;
        }
        if (d->title) { panel.title = cocoa_str(d->title); panel.message = cocoa_str(d->title); }
        if (d->filter_count > 0 && d->mode != VIO_PLATFORM_DIALOG_FOLDER) {
            NSMutableArray<NSString *> *types = [NSMutableArray array];
            for (int i = 0; i < d->filter_count; i++)
                for (int k = 0; k < d->filters[i].count; k++) [types addObject:cocoa_str(d->filters[i].extensions[k])];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            panel.allowedFileTypes = types;   /* UTType would need a framework the module does not link */
#pragma clang diagnostic pop
        }
        if (d->default_path) {
            NSString *p = cocoa_str(d->default_path);
            BOOL dir = NO;
            if ([[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&dir] && dir) {
                panel.directoryURL = [NSURL fileURLWithPath:p isDirectory:YES];
            } else {
                panel.directoryURL = [NSURL fileURLWithPath:[p stringByDeletingLastPathComponent] isDirectory:YES];
                if (d->mode == VIO_PLATFORM_DIALOG_SAVE) panel.nameFieldStringValue = [p lastPathComponent];
            }
        }
        [NSApp activateIgnoringOtherApps:YES];
        /* the panel runs out of process: -ok: does not reach it, ending the modal loop does */
        cocoa_test_dismiss(^(int action) { [NSApp stopModalWithCode:action == -2 ? NSModalResponseOK : NSModalResponseCancel]; });
        if ([panel runModal] != NSModalResponseOK) return 1;
        NSArray<NSURL *> *urls = open ? open.URLs : (panel.URL ? @[panel.URL] : @[]);
        if (urls.count == 0) return 1;
        char **paths = (char **)calloc(urls.count, sizeof(char *));
        if (!paths) return 0;
        int n = 0;
        for (NSURL *u in urls) {
            const char *fs = u.fileSystemRepresentation;
            if (fs && (paths[n] = strdup(fs))) n++;
        }
        *out_paths = paths;
        *out_count = n;
    }
    return 1;
}

static int cocoa_message_box(vio_window_handle h, const vio_message_box_desc *d)
{
    (void)h;
    if (!cocoa_initialized) return -1;
    @autoreleasepool {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = cocoa_str(d->title) ?: @"";
        alert.informativeText = cocoa_str(d->message) ?: @"";
        alert.alertStyle = d->kind == VIO_PLATFORM_MESSAGE_ERROR ? NSAlertStyleCritical
                         : d->kind == VIO_PLATFORM_MESSAGE_WARNING ? NSAlertStyleWarning : NSAlertStyleInformational;
        for (int i = 0; i < d->button_count; i++) [alert addButtonWithTitle:cocoa_str(d->buttons[i]) ?: @"?"];
        if (d->button_count > 1) alert.buttons.lastObject.keyEquivalent = @"\033";   /* Escape picks the last */
        [NSApp activateIgnoringOtherApps:YES];
        int last = d->button_count - 1;
        cocoa_test_dismiss(^(int action) {
            NSInteger i = action >= 0 && action <= last ? action : last;
            [alert.buttons[(NSUInteger)i] performClick:nil];
        });
        NSModalResponse r = [alert runModal];
        int idx = (int)(r - NSAlertFirstButtonReturn);
        return idx >= 0 && idx < d->button_count ? idx : last;
    }
}

/* ── Menus (DESKTOP-APP-PLAN Phase 2b) ────────────────────────────── */

static NSString *cocoa_key_equivalent(int key)
{
    unichar c = 0;
    if (key >= VIO_KEY_A && key <= VIO_KEY_Z) c = (unichar)('a' + key - VIO_KEY_A);
    else if (key >= VIO_KEY_0 && key <= VIO_KEY_9) c = (unichar)('0' + key - VIO_KEY_0);
    else if (key >= VIO_KEY_F1 && key <= VIO_KEY_F1 + 23) c = (unichar)(NSF1FunctionKey + key - VIO_KEY_F1);
    else switch (key) {
        case VIO_KEY_ENTER: c = '\r'; break;           case VIO_KEY_ESCAPE: c = 0x1B; break;
        case VIO_KEY_TAB: c = '\t'; break;             case VIO_KEY_SPACE: c = ' '; break;
        case VIO_KEY_BACKSPACE: c = 0x08; break;        case VIO_KEY_DELETE: c = NSDeleteFunctionKey; break;
        case VIO_KEY_INSERT: c = NSInsertFunctionKey; break; case VIO_KEY_HOME: c = NSHomeFunctionKey; break;
        case VIO_KEY_END: c = NSEndFunctionKey; break;  case VIO_KEY_PAGE_UP: c = NSPageUpFunctionKey; break;
        case VIO_KEY_PAGE_DOWN: c = NSPageDownFunctionKey; break; case VIO_KEY_UP: c = NSUpArrowFunctionKey; break;
        case VIO_KEY_DOWN: c = NSDownArrowFunctionKey; break; case VIO_KEY_LEFT: c = NSLeftArrowFunctionKey; break;
        case VIO_KEY_RIGHT: c = NSRightArrowFunctionKey; break; case VIO_KEY_COMMA: c = ','; break;
        case VIO_KEY_PERIOD: c = '.'; break;            case VIO_KEY_MINUS: c = '-'; break;
        case VIO_KEY_SLASH: c = '/'; break;             case VIO_KEY_EQUAL: c = '='; break;
        default: return @"";
    }
    return [NSString stringWithCharacters:&c length:1];
}

static NSMenu *cocoa_build_menu(NSString *title, const vio_menu_item *items, int n, VioCocoaMenuTarget *target)
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    menu.autoenablesItems = NO;
    for (int i = 0; i < n; i++) {
        const vio_menu_item *it = &items[i];
        if (it->separator) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
        NSString *label = cocoa_str(it->label) ?: @"?";
        NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:label action:NULL keyEquivalent:@""];
        mi.enabled = it->enabled ? YES : NO;
        if (it->children) {
            mi.submenu = cocoa_build_menu(label, it->children, it->child_count, target);
        } else {
            mi.target = target;
            mi.action = @selector(vioMenu:);
            mi.tag = it->tag;
            mi.state = it->checked ? NSControlStateValueOn : NSControlStateValueOff;
            if (it->key) {
                mi.keyEquivalent = cocoa_key_equivalent(it->key);
                NSEventModifierFlags m = 0;
                if (it->mods & VIO_MOD_SUPER)   m |= NSEventModifierFlagCommand;
                if (it->mods & VIO_MOD_CONTROL) m |= NSEventModifierFlagControl;
                if (it->mods & VIO_MOD_ALT)     m |= NSEventModifierFlagOption;
                if (it->mods & VIO_MOD_SHIFT)   m |= NSEventModifierFlagShift;
                mi.keyEquivalentModifierMask = m;
            }
        }
        [menu addItem:mi];
    }
    return menu;
}

/* About, Hide, Hide Others, Show All, Quit (asks vio_on_close) */
static NSMenuItem *cocoa_app_menu(VioCocoaMenuTarget *target)
{
    NSString *name = [[NSProcessInfo processInfo] processName];
    NSMenu *m = [[NSMenu alloc] initWithTitle:name];
    [m addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [m addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [m addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [m addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(vioQuit:) keyEquivalent:@"q"];
    quit.target = target;
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:name action:NULL keyEquivalent:@""];
    item.submenu = m;
    return item;
}

static int cocoa_set_menu_bar(vio_window_handle h, const vio_menu_item *menus, int count)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    @autoreleasepool {
        if (!w->menu_target) {
            VioCocoaMenuTarget *t = [[VioCocoaMenuTarget alloc] init];
            t.owner = w;
            w->menu_target = (void *)CFBridgingRetain(t);
        }
        VioCocoaMenuTarget *target = (__bridge VioCocoaMenuTarget *)w->menu_target;
        NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
        [bar addItem:cocoa_app_menu(target)];
        for (int i = 0; i < count; i++) {
            NSString *label = cocoa_str(menus[i].label) ?: @"?";
            NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:label action:NULL keyEquivalent:@""];
            top.submenu = cocoa_build_menu(label, menus[i].children, menus[i].child_count, target);
            [bar addItem:top];
        }
        [NSApp setMainMenu:bar];
    }
    return 1;
}

static int cocoa_popup_menu(vio_window_handle h, const vio_menu_item *items, int count, double x, double y)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!cocoa_initialized) return -2;
    @autoreleasepool {
        VioCocoaMenuTarget *target = [[VioCocoaMenuTarget alloc] init];
        target.popup = YES;
        target.chosen = -1;
        NSMenu *menu = cocoa_build_menu(@"", items, count, target);
        cocoa_test_dismiss(^(int action) { (void)action; [menu cancelTracking]; });
        if (w && !w->headless) {
            [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(x, y) inView:COCOA_VIEW(w)];   /* flipped: y from the top */
        } else {
            [menu popUpMenuPositioningItem:nil atLocation:[NSEvent mouseLocation] inView:nil];   /* no visible view: at the pointer */
        }
        return (int)target.chosen;
    }
}

/* ── Dock progress, notifications (DESKTOP-APP-PLAN Phase 2c) ─────── */

static void *cocoa_dock_bar;   /* NSProgressIndicator on the Dock tile, retained */

static int cocoa_set_progress(vio_window_handle h, double value, int state)
{
    (void)h;
    if (!cocoa_initialized) return 0;
    @autoreleasepool {
        NSDockTile *tile = NSApp.dockTile;
        if (value < 0.0 && state != VIO_PLATFORM_PROGRESS_INDETERMINATE) {
            tile.contentView = nil;
            [tile display];
            if (cocoa_dock_bar) { CFRelease(cocoa_dock_bar); cocoa_dock_bar = NULL; }
            return 1;
        }
        if (!cocoa_dock_bar) {
            NSSize size = tile.size;
            NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
            NSImageView *icon = [[NSImageView alloc] initWithFrame:root.bounds];
            icon.image = NSApp.applicationIconImage;
            [root addSubview:icon];
            NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(size.width * 0.1, size.height * 0.08, size.width * 0.8, 20)];
            bar.style = NSProgressIndicatorStyleBar;
            bar.minValue = 0;
            bar.maxValue = 1;
            [root addSubview:bar];
            tile.contentView = root;
            cocoa_dock_bar = (void *)CFBridgingRetain(bar);
        }
        NSProgressIndicator *bar = (__bridge NSProgressIndicator *)cocoa_dock_bar;
        /* no paused / error colours in AppKit: the bar shows the value */
        bar.indeterminate = state == VIO_PLATFORM_PROGRESS_INDETERMINATE;
        if (bar.indeterminate) [bar startAnimation:nil]; else { [bar stopAnimation:nil]; bar.doubleValue = value; }
        [tile display];
    }
    return 1;
}

/* UNUserNotificationCenter: only inside an app bundle (it throws without one). */
static int cocoa_available(int feature)
{
    if (feature != VIO_PLATFORM_FEATURE_NOTIFICATIONS) return 1;
    static int loaded = -1;
    if (loaded < 0) loaded = dlopen("/System/Library/Frameworks/UserNotifications.framework/UserNotifications", RTLD_LAZY) != NULL;
    return loaded && [NSBundle mainBundle].bundleIdentifier != nil && NSClassFromString(@"UNUserNotificationCenter") != nil;
}

static int cocoa_notify(const char *title, const char *body, const char *icon, const char *key)
{
    (void)icon;   /* a bundle's notifications carry its icon */
    if (!cocoa_available(VIO_PLATFORM_FEATURE_NOTIFICATIONS)) return 0;
    @autoreleasepool {
        UNUserNotificationCenter *center = [NSClassFromString(@"UNUserNotificationCenter") currentNotificationCenter];
        static int asked;
        if (!asked) {   /* the first time macOS asks the user */
            asked = 1;
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                  completionHandler:^(BOOL granted, NSError *error) { (void)granted; (void)error; }];
        }
        UNMutableNotificationContent *content = [[NSClassFromString(@"UNMutableNotificationContent") alloc] init];
        content.title = cocoa_str(title) ?: @"";
        content.body = cocoa_str(body) ?: @"";
        NSString *ident = key ? cocoa_str(key) : [[NSUUID UUID] UUIDString];   /* same id: replaces */
        UNNotificationRequest *req = [NSClassFromString(@"UNNotificationRequest") requestWithIdentifier:ident content:content trigger:nil];
        [center addNotificationRequest:req withCompletionHandler:nil];
    }
    return 1;
}

/* ── Tray (DESKTOP-APP-PLAN Phase 2c-2) ───────────────────────────── */

static void *cocoa_status_item;   /* NSStatusItem, retained */
static void *cocoa_tray_target;   /* VioCocoaMenuTarget for its menu, retained */

static int cocoa_set_tray(const vio_tray_desc *d)
{
    if (!cocoa_initialized) return 0;
    @autoreleasepool {
        if (!d) {
            if (cocoa_status_item) {
                [[NSStatusBar systemStatusBar] removeStatusItem:(__bridge NSStatusItem *)cocoa_status_item];
                CFRelease(cocoa_status_item);
                cocoa_status_item = NULL;
            }
            if (cocoa_tray_target) { CFRelease(cocoa_tray_target); cocoa_tray_target = NULL; }
            return 1;
        }
        if (!cocoa_tray_target) {
            VioCocoaMenuTarget *t = [[VioCocoaMenuTarget alloc] init];
            t.tray = YES;
            cocoa_tray_target = (void *)CFBridgingRetain(t);
        }
        VioCocoaMenuTarget *target = (__bridge VioCocoaMenuTarget *)cocoa_tray_target;
        if (!cocoa_status_item) {
            NSStatusItem *it = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
            if (!it) return 0;
            cocoa_status_item = (void *)CFBridgingRetain(it);
        }
        NSStatusItem *item = (__bridge NSStatusItem *)cocoa_status_item;
        NSStatusBarButton *button = item.button;
        if (d->image_count > 0) {
            int best = 0;   /* the menu bar is 22 pt high: the image closest to 18 px */
            for (int i = 1; i < d->image_count; i++) if (abs(d->images[i].height - 18) < abs(d->images[best].height - 18)) best = i;
            NSImage *img = cocoa_image(d->images[best].rgba, d->images[best].width, d->images[best].height);
            img.size = NSMakeSize(18.0 * d->images[best].width / d->images[best].height, 18.0);
            button.image = img;
            button.title = @"";
        } else {
            button.image = nil;
            button.title = d->tooltip ? cocoa_str(d->tooltip) : @"\u25CF";
        }
        button.toolTip = cocoa_str(d->tooltip);
        if (d->menu_count > 0) {
            item.menu = cocoa_build_menu(@"", d->menu, d->menu_count, target);   /* a click opens it */
            button.target = nil;
            button.action = NULL;
        } else {
            item.menu = nil;
            button.target = target;
            button.action = @selector(vioTrayClick:);
        }
    }
    return 1;
}

/* ── IME (DESKTOP-APP-PLAN Phase 3) ───────────────────────────────── */

static int cocoa_set_ime_rect(vio_window_handle h, int x, int y, int width, int height)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    w->ime_x = x; w->ime_y = y; w->ime_w = width; w->ime_h = height;
    [COCOA_VIEW(w).inputContext invalidateCharacterCoordinates];
    return 1;
}

static int cocoa_set_ime_enabled(vio_window_handle h, int enabled)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w) return 0;
    w->ime_off = !enabled;
    if (!enabled) {
        VioCocoaView *view = (__bridge VioCocoaView *)w->view;
        [view.inputContext discardMarkedText];
        [view unmarkText];
    }
    return 1;
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

/* ── Keyboard layout: UCKeyTranslate on the current layout ────────────
 * HIToolbox is loaded at run time (as GLFW does), so the build links no
 * Carbon. Any missing piece reads as "layout unknown" (NULL). */

typedef void *(*cocoa_TISCopy_t)(void);
typedef void *(*cocoa_TISGetProp_t)(void *source, CFStringRef key);
typedef int32_t (*cocoa_UCKeyTranslate_t)(const void *layout, uint16_t vk, uint16_t action, uint32_t mods,
                                          uint32_t kbd_type, uint32_t options, uint32_t *dead_state,
                                          unsigned long max_len, unsigned long *actual_len, uint16_t *chars);
typedef uint8_t (*cocoa_LMGetKbdType_t)(void);

static const char *cocoa_key_name(int key)
{
    static void *hitoolbox;
    static cocoa_TISCopy_t tis_copy;
    static cocoa_TISGetProp_t tis_prop;
    static cocoa_UCKeyTranslate_t uc_translate;
    static cocoa_LMGetKbdType_t kbd_type;
    static CFStringRef *layout_data_key;
    static char name[16];

    if (!hitoolbox) {
        hitoolbox = dlopen("/System/Library/Frameworks/Carbon.framework/Frameworks/HIToolbox.framework/HIToolbox", RTLD_LAZY);
        if (!hitoolbox) return NULL;
        tis_copy = (cocoa_TISCopy_t)dlsym(hitoolbox, "TISCopyCurrentKeyboardLayoutInputSource");
        tis_prop = (cocoa_TISGetProp_t)dlsym(hitoolbox, "TISGetInputSourceProperty");
        layout_data_key = (CFStringRef *)dlsym(hitoolbox, "kTISPropertyUnicodeKeyLayoutData");
        kbd_type = (cocoa_LMGetKbdType_t)dlsym(hitoolbox, "LMGetKbdType");
        uc_translate = (cocoa_UCKeyTranslate_t)dlsym(RTLD_DEFAULT, "UCKeyTranslate");
        if (!uc_translate) uc_translate = (cocoa_UCKeyTranslate_t)dlsym(hitoolbox, "UCKeyTranslate");
    }
    if (!tis_copy || !tis_prop || !layout_data_key || !kbd_type || !uc_translate) return NULL;

    int keycode = -1;
    for (int kc = 0; kc < 256; kc++) {
        if (cocoa_keycodes[kc] == key) { keycode = kc; break; }
    }
    if (keycode < 0) return NULL;

    void *source = tis_copy();
    if (!source) return NULL;
    CFDataRef data = (CFDataRef)tis_prop(source, *layout_data_key);
    const char *result = NULL;
    if (data) {
        uint32_t dead = 0;
        unsigned long len = 0;
        uint16_t chars[4];
        /* kUCKeyActionDisplay = 3, kUCKeyTranslateNoDeadKeysMask = 1 */
        if (uc_translate(CFDataGetBytePtr(data), (uint16_t)keycode, 3, 0, kbd_type(), 1,
                         &dead, 4, &len, chars) == 0 && len > 0) {
            CFStringRef s = CFStringCreateWithCharacters(kCFAllocatorDefault, chars, (CFIndex)len);
            if (s) {
                CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
                if (m) {
                    CFStringLowercase(m, NULL);
                    if (CFStringGetCString(m, name, sizeof(name), kCFStringEncodingUTF8)
                        && (unsigned char)name[0] >= 32 && name[0] != 127) result = name;
                    CFRelease(m);
                }
                CFRelease(s);
            }
        }
    }
    CFRelease((CFTypeRef)source);
    return result;
}

/* ── Clipboard: the general NSPasteboard, plain UTF-8 text ─────────── */

static int cocoa_clipboard_get(char **out_utf8)
{
    *out_utf8 = NULL;
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char *u = s ? [s UTF8String] : NULL;
        if (u) {
            size_t n = strlen(u);
            char *copy = (char *)malloc(n + 1);
            if (copy) { memcpy(copy, u, n + 1); *out_utf8 = copy; }
        }
    }
    return 1;
}

static int cocoa_clipboard_set(const char *utf8)
{
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *s = [NSString stringWithUTF8String:(utf8 ? utf8 : "")];
        if (!s) return 0;
        [pb clearContents];
        return [pb setString:s forType:NSPasteboardTypeString] ? 1 : 0;
    }
}

/* vio_window 'parent' / 'modal': a child window moves with its parent and stays
 * above it; 'modal' is vio's input block (an app-modal session would stop the
 * PHP loop). */
static int cocoa_set_parent(vio_window_handle h, vio_window_handle ph, int modal)
{
    (void)modal;
    vio_cocoa_window *w = (vio_cocoa_window *)h, *p = (vio_cocoa_window *)ph;
    if (!w || !w->window) return 0;
    @autoreleasepool {
        NSWindow *child = (__bridge NSWindow *)w->window;
        if (!p) {
            if (child.parentWindow) [child.parentWindow removeChildWindow:child];
            return 1;
        }
        if (!p->window) return 0;
        /* addChildWindow orders the child in: a headless (hidden) one stays apart */
        if (w->headless) return 1;
        [(__bridge NSWindow *)p->window addChildWindow:child ordered:NSWindowAbove];
    }
    return 1;
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
    .wake                 = cocoa_wake,
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
    .clipboard_get        = cocoa_clipboard_get,
    .clipboard_set        = cocoa_clipboard_set,
    .key_name             = cocoa_key_name,
    .set_cursor_shape     = cocoa_set_cursor_shape,
    .set_cursor_image     = cocoa_set_cursor_image,
    .set_window_icon      = cocoa_set_window_icon,
    .set_size_limits      = cocoa_set_size_limits,
    .set_aspect_ratio     = cocoa_set_aspect_ratio,
    .set_floating         = cocoa_set_floating,
    .set_opacity          = cocoa_set_opacity,
    .open_url             = cocoa_open_url,
    .reveal_file          = cocoa_reveal_file,
    .accepts_drops        = 1,
    .file_dialog          = cocoa_file_dialog,
    .message_box          = cocoa_message_box,
    .set_menu_bar         = cocoa_set_menu_bar,
    .popup_menu           = cocoa_popup_menu,
    .available            = cocoa_available,
    .notify               = cocoa_notify,
    .set_progress         = cocoa_set_progress,
    .set_tray             = cocoa_set_tray,
    .ime_preedit          = 1,
    .set_ime_rect         = cocoa_set_ime_rect,
    .set_ime_enabled      = cocoa_set_ime_enabled,
    .set_parent           = cocoa_set_parent,
};

void vio_platform_cocoa_register(void)
{
    vio_register_platform(&vio_platform_cocoa);
}

#endif /* HAVE_COCOA && !HAVE_IOS */
