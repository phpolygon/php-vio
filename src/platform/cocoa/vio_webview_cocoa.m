/*
 * php-vio - Cocoa webview (WEBVIEW-PLAN W1): WKWebView as a subview of the
 * vio view, above the Metal layer.
 *
 * #include'd by vio_platform_cocoa.m (one translation unit, so it sees the
 * window struct). WebKit is loaded at runtime and every class is looked up
 * by name - the module links no WebKit (as UserNotifications). Custom schemes
 * go through a WKURLSchemeHandler: each task gets an id, is queued for PHP and
 * stays open until vio_webview_reply_end (or WebKit stops it). Script
 * messages arrive on the "vio" handler. Every engine callback posts an empty
 * application event so a waiting vio_wait_events returns and PHP gets them.
 */

#import <WebKit/WebKit.h>   /* declarations only: classes through NSClassFromString */

typedef struct _vio_cocoa_webview vio_cocoa_webview;

@interface VioWebHandler : NSObject <WKURLSchemeHandler, WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, assign) vio_cocoa_webview *owner;
@end

struct _vio_cocoa_webview {
    void             *view;      /* WKWebView, retained */
    void             *handler;   /* VioWebHandler, retained */
    void             *tasks;     /* NSMutableDictionary<NSNumber *, id<WKURLSchemeTask>>, retained */
    vio_cocoa_window *win;
    void             *user;
    uint64_t          next_id;
    char             *title;     /* last reported title (malloc) */
};

static int cocoa_webkit_loaded(void)
{
    static int loaded = -1;
    if (loaded < 0) {
        loaded = dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY) != NULL &&
                 NSClassFromString(@"WKWebView") != nil;
    }
    return loaded;
}

/* End a nextEventMatchingMask wait without setting vio_wake's sticky flag. */
static void cocoa_webview_kick(void)
{
    NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSMakePoint(0, 0)
                                modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:ev atStart:NO];
}

static NSString *cocoa_webview_bridge =
    @"(function () {"
     "  if (window.vio) return;"
     "  var h = window.webkit.messageHandlers.vio;"
     "  window.vio = Object.freeze({ postMessage: function (v) { h.postMessage(JSON.stringify(v === undefined ? null : v)); } });"
     "})();";

@implementation VioWebHandler

- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    vio_cocoa_webview *wv = self.owner;
    if (!wv) return;
    uint64_t rid = ++wv->next_id;
    ((__bridge NSMutableDictionary *)wv->tasks)[@(rid)] = task;

    NSURLRequest *req = task.request;
    NSData *body = req.HTTPBody;
    if (!body && req.HTTPBodyStream) {   /* fetch() bodies arrive as a stream */
        NSInputStream *in = req.HTTPBodyStream;
        NSMutableData *buf = [NSMutableData data];
        uint8_t chunk[65536];
        [in open];
        NSInteger n;
        while ((n = [in read:chunk maxLength:sizeof(chunk)]) > 0) [buf appendBytes:chunk length:(NSUInteger)n];
        [in close];
        body = buf;
    }
    NSDictionary<NSString *, NSString *> *fields = req.allHTTPHeaderFields;
    int count = (int)fields.count, i = 0;
    vio_webview_header *h = count ? (vio_webview_header *)calloc((size_t)count, sizeof(vio_webview_header)) : NULL;
    for (NSString *name in fields) {
        h[i].name = name.UTF8String;
        h[i].value = fields[name].UTF8String;
        i++;
    }
    vio_webview_request_event(wv->user, rid, req.HTTPMethod.UTF8String, req.URL.absoluteString.UTF8String,
                              h, i, body.bytes, body.length);
    free(h);
    cocoa_webview_kick();
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    vio_cocoa_webview *wv = self.owner;
    if (!wv) return;
    NSMutableDictionary *tasks = (__bridge NSMutableDictionary *)wv->tasks;
    for (NSNumber *key in [tasks allKeysForObject:task]) {
        [tasks removeObjectForKey:key];
        vio_webview_cancel_event(wv->user, key.unsignedLongLongValue);
    }
    cocoa_webview_kick();
}

- (void)userContentController:(WKUserContentController *)ucc didReceiveScriptMessage:(WKScriptMessage *)message
{
    vio_cocoa_webview *wv = self.owner;
    if (!wv || ![message.body isKindOfClass:[NSString class]]) return;
    NSData *utf8 = [(NSString *)message.body dataUsingEncoding:NSUTF8StringEncoding];
    vio_webview_message_event(wv->user, (const char *)utf8.bytes, utf8.length);
    cocoa_webview_kick();
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation
{
    vio_cocoa_webview *wv = self.owner;
    if (!wv) return;
    vio_webview_nav_event(wv->user, VIO_WEBVIEW_EV_NAVIGATED, webView.URL.absoluteString.UTF8String);
    cocoa_webview_kick();
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    vio_cocoa_webview *wv = self.owner;
    if (!wv || ![keyPath isEqualToString:@"title"]) return;
    NSString *t = ((WKWebView *)object).title;
    const char *s = t.length ? t.UTF8String : NULL;
    if (!s || (wv->title && strcmp(wv->title, s) == 0)) return;
    free(wv->title);
    wv->title = strdup(s);
    vio_webview_nav_event(wv->user, VIO_WEBVIEW_EV_TITLE, s);
    cocoa_webview_kick();
}

/* window.open / target=_blank: reported, never opened by WebKit */
- (WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
   forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features
{
    vio_cocoa_webview *wv = self.owner;
    if (wv) {
        vio_webview_nav_event(wv->user, VIO_WEBVIEW_EV_NEW_WINDOW, action.request.URL.absoluteString.UTF8String);
        cocoa_webview_kick();
    }
    return nil;
}

@end

#define COCOA_WK(wv) ((__bridge WKWebView *)(wv)->view)

static void cocoa_webview_place(vio_cocoa_webview *wv, int has_bounds, int x, int y, int width, int height)
{
    WKWebView *view = COCOA_WK(wv);
    NSView *parent = COCOA_VIEW(wv->win);
    if (has_bounds) {
        view.autoresizingMask = NSViewNotSizable;   /* the vio view is flipped: the frame stays at the top left */
        view.frame = NSMakeRect(x, y, width, height);
    } else {
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view.frame = parent.bounds;
    }
}

static int cocoa_webview_load(vio_cocoa_webview *wv, const char *url)
{
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url ? url : "about:blank"]];
    if (!u) return 0;
    [COCOA_WK(wv) loadRequest:[NSURLRequest requestWithURL:u]];
    return 1;
}

static void *cocoa_webview_create(vio_window_handle h, const vio_webview_desc *d)
{
    vio_cocoa_window *w = (vio_cocoa_window *)h;
    if (!w || !cocoa_webkit_loaded()) return NULL;
    vio_cocoa_webview *wv = (vio_cocoa_webview *)calloc(1, sizeof(vio_cocoa_webview));
    if (!wv) return NULL;
    wv->win = w;
    wv->user = d->user;
    @autoreleasepool {
        VioWebHandler *handler = [VioWebHandler new];
        handler.owner = wv;
        WKWebViewConfiguration *config = [NSClassFromString(@"WKWebViewConfiguration") new];
        for (int i = 0; i < d->scheme_count; i++) {
            @try {
                [config setURLSchemeHandler:handler forURLScheme:[NSString stringWithUTF8String:d->schemes[i]]];
            } @catch (NSException *e) {   /* a scheme WebKit handles itself */
                php_error_docref(NULL, E_WARNING, "vio_webview: WebKit refuses the scheme \"%s\"", d->schemes[i]);
            }
        }
        WKUserScript *bridge = [[NSClassFromString(@"WKUserScript") alloc] initWithSource:cocoa_webview_bridge
                                                                           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                                        forMainFrameOnly:YES];
        [config.userContentController addUserScript:bridge];
        [config.userContentController addScriptMessageHandler:handler name:@"vio"];
        if (d->devtools) [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

        WKWebView *view = [[NSClassFromString(@"WKWebView") alloc] initWithFrame:NSZeroRect configuration:config];
        if (!view) { free(wv); return NULL; }
        view.navigationDelegate = handler;
        view.UIDelegate = handler;
        if (d->devtools && [view respondsToSelector:@selector(setInspectable:)]) [view setValue:@YES forKey:@"inspectable"];
        if (d->transparent) [view setValue:@NO forKey:@"drawsBackground"];
        [view addObserver:handler forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:NULL];

        wv->view = (void *)CFBridgingRetain(view);
        wv->handler = (void *)CFBridgingRetain(handler);
        wv->tasks = (void *)CFBridgingRetain([NSMutableDictionary dictionary]);
        [COCOA_VIEW(w) addSubview:view];
        cocoa_webview_place(wv, d->has_bounds, d->x, d->y, d->width, d->height);
        cocoa_webview_load(wv, d->url);
    }
    return wv;
}

static void cocoa_webview_destroy(void *p)
{
    vio_cocoa_webview *wv = (vio_cocoa_webview *)p;
    if (!wv) return;
    @autoreleasepool {
        WKWebView *view = COCOA_WK(wv);
        VioWebHandler *handler = (__bridge VioWebHandler *)wv->handler;
        handler.owner = NULL;
        [view removeObserver:handler forKeyPath:@"title"];
        [view stopLoading];
        view.navigationDelegate = nil;
        view.UIDelegate = nil;
        [view.configuration.userContentController removeScriptMessageHandlerForName:@"vio"];
        /* open tasks: WebKit stops them when the view goes; answering them now would throw */
        [(__bridge NSMutableDictionary *)wv->tasks removeAllObjects];
        [view removeFromSuperview];
        CFRelease(wv->tasks);
        CFRelease(wv->handler);
        CFRelease(wv->view);
    }
    free(wv->title);
    free(wv);
}

static int cocoa_webview_navigate(void *p, const char *url)
{
    @autoreleasepool { return cocoa_webview_load((vio_cocoa_webview *)p, url); }
}

static int cocoa_webview_eval(void *p, const char *js)
{
    @autoreleasepool {
        NSString *s = [NSString stringWithUTF8String:js];
        if (!s) return 0;
        [COCOA_WK((vio_cocoa_webview *)p) evaluateJavaScript:s completionHandler:nil];
    }
    return 1;
}

static int cocoa_webview_set_bounds(void *p, int has_bounds, int x, int y, int width, int height)
{
    @autoreleasepool { cocoa_webview_place((vio_cocoa_webview *)p, has_bounds, x, y, width, height); }
    return 1;
}

static id<WKURLSchemeTask> cocoa_webview_task(vio_cocoa_webview *wv, uint64_t rid)
{
    return ((__bridge NSMutableDictionary *)wv->tasks)[@(rid)];
}

static int cocoa_webview_reply_head(void *p, uint64_t rid, int status, const vio_webview_header *h, int count)
{
    vio_cocoa_webview *wv = (vio_cocoa_webview *)p;
    @autoreleasepool {
        id<WKURLSchemeTask> task = cocoa_webview_task(wv, rid);
        if (!task) return 0;
        NSMutableDictionary<NSString *, NSString *> *fields = [NSMutableDictionary dictionary];
        for (int i = 0; i < count; i++) {
            NSString *name = [NSString stringWithUTF8String:h[i].name];
            NSString *value = [NSString stringWithUTF8String:h[i].value];
            if (!name || !value) continue;
            NSString *had = fields[name];   /* repeated headers fold into one line, as NSHTTPURLResponse keeps them */
            fields[name] = had ? [NSString stringWithFormat:@"%@, %@", had, value] : value;
        }
        NSHTTPURLResponse *res = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL statusCode:status
                                                            HTTPVersion:@"HTTP/1.1" headerFields:fields];
        @try { [task didReceiveResponse:res]; } @catch (NSException *e) { return 0; }
    }
    return 1;
}

static int cocoa_webview_reply_data(void *p, uint64_t rid, const void *data, size_t len)
{
    @autoreleasepool {
        id<WKURLSchemeTask> task = cocoa_webview_task((vio_cocoa_webview *)p, rid);
        if (!task) return 0;
        @try { [task didReceiveData:[NSData dataWithBytes:data length:len]]; } @catch (NSException *e) { return 0; }
    }
    return 1;
}

static int cocoa_webview_reply_end(void *p, uint64_t rid)
{
    vio_cocoa_webview *wv = (vio_cocoa_webview *)p;
    @autoreleasepool {
        id<WKURLSchemeTask> task = cocoa_webview_task(wv, rid);
        if (!task) return 0;
        [(__bridge NSMutableDictionary *)wv->tasks removeObjectForKey:@(rid)];
        @try { [task didFinish]; } @catch (NSException *e) { return 0; }
    }
    return 1;
}
