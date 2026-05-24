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

/* Try to find a foreground key UIWindow. iOS 13+ uses UIScene; if the
 * wrapper still uses the older AppDelegate-only model, we fall back to
 * the deprecated UIApplication.keyWindow. Returns nil if no window is
 * ready - caller must retry later (typically from viewDidAppear). */
static UIWindow *vio_ios_find_key_window(void)
{
    UIApplication *app = [UIApplication sharedApplication];

    if (@available(iOS 13.0, *)) {
        for (UIScene *scene in app.connectedScenes) {
            if (scene.activationState != UISceneActivationStateForegroundActive) continue;
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene *ws = (UIWindowScene *)scene;
            for (UIWindow *w in ws.windows) {
                if (w.isKeyWindow) return w;
            }
            if (ws.windows.count > 0) return ws.windows.firstObject;
        }
    }

    /* iOS 12 fallback - never compiled under our 14.0 baseline but kept
     * for safety in case the deployment target drops later. */
#if !defined(__IPHONE_13_0) || __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_13_0
    if (app.keyWindow) return app.keyWindow;
#endif
    return nil;
}

int vio_ios_setup_context(int width, int height, vio_config *cfg,
                          void *input_state)
{
    (void)width; (void)height; /* iOS sizes itself from screen + scene */

    @autoreleasepool {
        UIWindow *keyWindow = vio_ios_find_key_window();
        if (!keyWindow) {
            php_error_docref(NULL, E_WARNING,
                "iOS: no key UIWindow available - call vio_create() after UIScene "
                "activation (e.g. from viewDidAppear or a CADisplayLink callback)");
            return -1;
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

        if (vio_metal_setup_context_native((__bridge void *)layer, fb_w, fb_h, cfg) != 0) {
            [vio_ios_view removeFromSuperview];
            vio_ios_view = nil;
            return -1;
        }

        return 0;
    }
}

void vio_ios_shutdown_context(void)
{
    @autoreleasepool {
        if (vio_ios_view) {
            [vio_ios_view removeFromSuperview];
            vio_ios_view = nil;
        }
    }
}

void vio_ios_get_framebuffer_size(int *out_w, int *out_h)
{
    if (!vio_ios_view) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }
    CGFloat scale = vio_ios_view.contentScaleFactor;
    CGSize size = vio_ios_view.bounds.size;
    if (out_w) *out_w = (int)(size.width  * scale);
    if (out_h) *out_h = (int)(size.height * scale);
}

#endif /* HAVE_IOS */
