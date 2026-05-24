/*
 * php-vio - iOS / iPadOS Backend implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_IOS

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "vio_ios.h"
#include "../metal/vio_metal.h"
#include "../../vio_input.h"

/* Framebuffer size in physical pixels, cached so the render thread can read
 * it without touching UIKit. UIView.bounds is main-thread-only; the game's
 * render loop (and vio_begin) runs on a background thread, so reading bounds
 * there returns garbage/0. layoutSubviews (main thread) publishes the size
 * here; vio_ios_get_framebuffer_size just reads it. Plain volatile ints - a
 * torn read at worst costs one mis-sized frame, corrected the next. */
static volatile int g_ios_fb_w = 0;
static volatile int g_ios_fb_h = 0;

/* ── VioRenderView ─────────────────────────────────────────────────
 *
 * UIView subclass that:
 *   - Backs its CALayer with CAMetalLayer (the override of +layerClass)
 *   - Receives multi-touch events and forwards them to vio_input_touch_*
 *   - Notifies the Metal backend of size changes on rotation / multitasking
 */

@interface VioRenderView : UIView
@property (nonatomic, assign) vio_input_state *inputState;
@end

@implementation VioRenderView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame inputState:(vio_input_state *)state {
    self = [super initWithFrame:frame];
    if (self) {
        _inputState = state;
        self.multipleTouchEnabled = YES;
        /* nativeScale matches Apple's recommendation for crisp Metal-rendered
         * content on Retina displays (matches contentScale of the layer). */
        self.contentScaleFactor = [UIScreen mainScreen].nativeScale;
    }
    return self;
}

/* Dispatch a touch set to the vio input ring. UITouch* is used as the
 * stable id - it's a heap pointer that stays the same for the lifetime
 * of one finger-down-to-up sequence (UIKit guarantees this). Casting it
 * to uintptr_t and then unsigned long long gives us a 64-bit stable id
 * that can never collide with another live finger.
 *
 * Coordinates are in framebuffer pixels (logical points * contentScale)
 * to match the GLFW cursor callback's behaviour. The PHP-facing
 * vio_touch_get() handles the scale-back to logical points the same way
 * vio_mouse_position does. */
- (void)dispatchTouches:(NSSet<UITouch *> *)touches phase:(vio_touch_phase)phase {
    if (!self.inputState) return;
    CGFloat scale = self.contentScaleFactor;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        unsigned long long tid = (unsigned long long)(uintptr_t)t;
        double x = (double)(p.x * scale);
        double y = (double)(p.y * scale);
        switch (phase) {
            case VIO_TOUCH_BEGAN:
                vio_input_touch_began(self.inputState, tid, x, y);
                break;
            case VIO_TOUCH_MOVED:
                vio_input_touch_moved(self.inputState, tid, x, y);
                break;
            case VIO_TOUCH_ENDED:
                vio_input_touch_ended(self.inputState, tid);
                break;
            case VIO_TOUCH_CANCELLED:
                vio_input_touch_cancelled(self.inputState, tid);
                break;
            default: break;
        }
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchTouches:touches phase:VIO_TOUCH_BEGAN];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchTouches:touches phase:VIO_TOUCH_MOVED];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchTouches:touches phase:VIO_TOUCH_ENDED];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self dispatchTouches:touches phase:VIO_TOUCH_CANCELLED];
}

/* layoutSubviews fires on rotation, on entering / leaving split view,
 * on iPad Stage Manager resize. We forward the new framebuffer size to
 * the Metal backend, which recreates depth + offscreen textures. */
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    CGFloat scale = self.contentScaleFactor;
    CGSize size = self.bounds.size;
    int w = (int)(size.width  * scale);
    int h = (int)(size.height * scale);
    layer.drawableSize = CGSizeMake(w, h);
    g_ios_fb_w = w;
    g_ios_fb_h = h;
    vio_metal_handle_resize(w, h);
}

@end

/* ── Module-level state ────────────────────────────────────────────
 *
 * Only one iOS context can exist at a time (matches the desktop model -
 * one GLFW window per process is the supported configuration). The
 * VioRenderView holds the strong reference to the layer and the input
 * state; this static just lets us find them again for shutdown / size. */

static VioRenderView *vio_ios_view = nil;

/* Find a usable UIWindow to host the render layer. iOS 13+ uses UIScene.
 *
 * Window discovery happens in two passes because vio_create() is often
 * called from viewDidAppear, when the scene is still transitioning and
 * has not reached UISceneActivationStateForegroundActive yet:
 *   pass 1 - prefer a ForegroundActive scene's key window (steady state)
 *   pass 2 - accept ANY window scene that already owns a window, regardless
 *            of activation state (covers the launch / foreground-inactive
 *            window that exists but is not yet "active")
 * Returns nil only when no window scene owns a window at all - the caller
 * should then retry on a later run-loop tick. */
static UIWindow *vio_ios_find_window_in_state(UISceneActivationState wantState, BOOL anyState)
{
    UIApplication *app = [UIApplication sharedApplication];
    for (UIScene *scene in app.connectedScenes) {
        if (!anyState && scene.activationState != wantState) continue;
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) {
            if (w.isKeyWindow) return w;
        }
        if (ws.windows.count > 0) return ws.windows.firstObject;
    }
    return nil;
}

static UIWindow *vio_ios_find_key_window(void)
{
    if (@available(iOS 13.0, *)) {
        /* Pass 1: a fully-active scene's window. */
        UIWindow *w = vio_ios_find_window_in_state(UISceneActivationStateForegroundActive, NO);
        if (w) return w;
        /* Pass 2: any scene that already has a window (launch transition). */
        w = vio_ios_find_window_in_state(UISceneActivationStateForegroundActive, YES);
        if (w) return w;
    }

    /* iOS 12 fallback - never compiled under our 14.0 baseline but kept
     * for safety in case the deployment target drops later. */
#if !defined(__IPHONE_13_0) || __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_13_0
    if ([UIApplication sharedApplication].keyWindow) {
        return [UIApplication sharedApplication].keyWindow;
    }
#endif
    return nil;
}

int vio_ios_setup_context(int width, int height, vio_config *cfg,
                          void *input_state)
{
    (void)width; (void)height; /* iOS sizes itself from screen + scene */

    /* All UIKit work (window lookup, view creation, addSubview) and the
     * CAMetalLayer setup MUST run on the main thread - UIKit is not
     * thread-safe. The game's PHP loop typically runs on a background
     * thread so the main run loop stays free to composite and deliver
     * touches, so dispatch the setup to main and block until it finishes.
     * PHP error reporting is deferred to the caller's thread (php_error_docref
     * touches PHP TLS that belongs to the calling thread, not main). */
    __block int result = -1;
    __block int no_window = 0;

    void (^setup)(void) = ^{
        @autoreleasepool {
            UIWindow *keyWindow = vio_ios_find_key_window();
            if (!keyWindow) {
                no_window = 1;
                result = -1;
                return;
            }

            CGRect bounds = keyWindow.bounds;
            vio_ios_view = [[VioRenderView alloc]
                initWithFrame:bounds
                inputState:(vio_input_state *)input_state];
            vio_ios_view.autoresizingMask =
                UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            [keyWindow addSubview:vio_ios_view];
            /* Move the render view to the back so any UIKit chrome from the
             * wrapper (status bar overlays, debug HUD) stays visible on top. */
            [keyWindow sendSubviewToBack:vio_ios_view];

            CGFloat scale = vio_ios_view.contentScaleFactor;
            int fb_w = (int)(bounds.size.width  * scale);
            int fb_h = (int)(bounds.size.height * scale);

            CAMetalLayer *layer = (CAMetalLayer *)vio_ios_view.layer;
            layer.contentsScale = scale;

            /* Publish the initial size now; layoutSubviews refreshes it on
             * rotation / resize. Without this the render thread would read
             * 0x0 until the first layout pass. */
            g_ios_fb_w = fb_w;
            g_ios_fb_h = fb_h;

            if (vio_metal_setup_context_native((__bridge void *)layer, fb_w, fb_h, cfg) != 0) {
                [vio_ios_view removeFromSuperview];
                vio_ios_view = nil;
                result = -1;
                return;
            }

            result = 0;
        }
    };

    if ([NSThread isMainThread]) {
        setup();
    } else {
        dispatch_sync(dispatch_get_main_queue(), setup);
    }

    if (no_window) {
        php_error_docref(NULL, E_WARNING,
            "iOS: no key UIWindow available - call vio_create() after UIScene "
            "activation (e.g. from viewDidAppear or a CADisplayLink callback)");
    }

    return result;
}

void vio_ios_shutdown_context(void)
{
    /* removeFromSuperview is UIKit - main thread only. vio_destroy (which
     * calls us) typically runs on the game's background thread, so hop to
     * main. Without this iOS throws NSInternalInconsistencyException
     * ("UI changes off the main thread") during teardown. */
    void (^teardown)(void) = ^{
        @autoreleasepool {
            if (vio_ios_view) {
                [vio_ios_view removeFromSuperview];
                vio_ios_view = nil;
            }
        }
    };
    if ([NSThread isMainThread]) {
        teardown();
    } else {
        dispatch_sync(dispatch_get_main_queue(), teardown);
    }
}

void vio_ios_get_framebuffer_size(int *out_w, int *out_h)
{
    /* Read the cached size published by layoutSubviews / setup on the main
     * thread. Safe to call from the render thread (no UIKit access). */
    if (out_w) *out_w = g_ios_fb_w;
    if (out_h) *out_h = g_ios_fb_h;
}

#endif /* HAVE_IOS */
