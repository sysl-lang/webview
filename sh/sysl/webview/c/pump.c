#include "pump.h"

#include <stddef.h>

// ## Why every symbol here is reached with `dlsym` rather than linked
//
// The pump needs the platform's own toolkit -- the Objective-C runtime on macOS, glib on GTK -- and a
// `@link` directive in `c.sysl` goes on EVERY consumer's link line on every platform. `-lobjc` is not
// a library a Linux box has and `-lglib-2.0` is not one a Mac has, so linking either would break the
// other. There is no per-platform link directive.
//
// `dlsym(RTLD_DEFAULT, ...)` asks the process for what is ALREADY loaded, and by the time anything
// here is called `libwebview` has been loaded and has brought its own toolkit with it -- libobjc on
// macOS, glib and gtk on Linux. So the symbols are there to be found, and the answer where they are
// not is an honest -1 rather than a link that failed on a machine nobody tested.
//
// `dlsym` itself needs no link flag: it is in `libSystem` on macOS and in `libc` from glibc 2.34,
// which is the floor this org already builds against.

#if defined(__APPLE__)

#include <dlfcn.h>

// objc_msgSend is called through a correctly typed function pointer for each selector, never through
// one variadic declaration. On arm64 a variadic call puts its arguments somewhere else entirely, so a
// single `id (*)(id, SEL, ...)` would pass garbage -- this is the one rule about calling the
// Objective-C runtime from C that cannot be got wrong quietly.
typedef void *(*msg_cls_t)(void *, void *);
typedef void *(*msg_str_t)(void *, void *, const char *);
typedef void *(*msg_next_t)(void *, void *, unsigned long long, void *, void *, signed char);
typedef void (*msg_arg_t)(void *, void *, void *);
typedef void (*msg_void_t)(void *, void *);

static int ready;     // 1 usable, -1 unusable, 0 not tried
static void *app;     // NSApplication.sharedApplication
static void *mode;    // NSDefaultRunLoopMode, which is the string "kCFRunLoopDefaultMode"
static void *past;    // NSDate.distantPast -- an expiry already gone, so the wait never blocks
static void *sel_next;
static void *sel_send;
static void *sel_update;

static void *(*msg_send)(void *, void *);
static void *(*pool_push)(void);
static void (*pool_pop)(void *);

static int start(void) {
    void *(*get_class)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*get_sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");

    msg_send = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    pool_push = (void *(*)(void))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush");
    pool_pop = (void (*)(void *))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop");

    if (!get_class || !get_sel || !msg_send || !pool_push || !pool_pop) return -1;

    void *ns_application = get_class("NSApplication");
    void *ns_string = get_class("NSString");
    void *ns_date = get_class("NSDate");
    if (!ns_application || !ns_string || !ns_date) return -1;

    // The application object already exists and is already launched: `webview_create` runs the main
    // loop once itself, to get its window built inside the launch notification, and returns only
    // after it has stopped it again. So there is no `finishLaunching` owed here.
    app = ((msg_cls_t)msg_send)(ns_application, get_sel("sharedApplication"));
    if (!app) return -1;

    // NSDefaultRunLoopMode IS the string "kCFRunLoopDefaultMode", so it can be built rather than
    // linked -- which is what keeps CoreFoundation off the link line.
    mode = ((msg_str_t)msg_send)(ns_string, get_sel("stringWithUTF8String:"), "kCFRunLoopDefaultMode");
    past = ((msg_cls_t)msg_send)(ns_date, get_sel("distantPast"));
    if (!mode || !past) return -1;

    // Both are handed back autoreleased, and every pump drains a pool.
    ((msg_cls_t)msg_send)(mode, get_sel("retain"));
    ((msg_cls_t)msg_send)(past, get_sel("retain"));

    sel_next = get_sel("nextEventMatchingMask:untilDate:inMode:dequeue:");
    sel_send = get_sel("sendEvent:");
    sel_update = get_sel("updateWindows");

    return 1;
}

// **Dequeuing is not enough: AppKit delivers an event to its window by `sendEvent:` and nothing
// else.** Running the run loop alone -- `CFRunLoopRunInMode` -- lets events accumulate in the
// application's queue and dispatches none of them, so the window draws and never answers a click.
// This is the loop `-[NSApplication run]` is, with the blocking taken out of it.
int sysl_webview_pump(void) {
    if (ready == 0) ready = start();
    if (ready < 0) return -1;

    void *pool = pool_push();
    int handled = 0;

    for (;;) {
        // NSEventMaskAny is every bit set; `distantPast` makes the wait a poll.
        void *event = ((msg_next_t)msg_send)(app, sel_next, ~0ULL, past, mode, 1);
        if (!event) break;
        ((msg_arg_t)msg_send)(app, sel_send, event);
        handled++;

        // A window server that is delivering faster than this is drained must not hold the loop for
        // ever -- the caller has its own timers to run.
        if (handled >= 64) break;
    }

    ((msg_void_t)msg_send)(app, sel_update);
    pool_pop(pool);

    return handled;
}

#elif defined(__linux__) || defined(__unix__)

#include <dlfcn.h>

static int ready;
static int (*iteration)(void *, int);

static int start(void) {
    iteration = (int (*)(void *, int))dlsym(RTLD_DEFAULT, "g_main_context_iteration");
    return iteration ? 1 : -1;
}

// glib's own non-blocking turn. `NULL` is the default context, which is the one GTK runs on, and the
// second argument is whether to block -- false, for the reason the macOS branch polls.
int sysl_webview_pump(void) {
    if (ready == 0) ready = start();
    if (ready < 0) return -1;

    int handled = 0;

    while (handled < 64 && iteration(NULL, 0)) handled++;

    return handled;
}

#elif defined(_WIN32)

#include <windows.h>

int sysl_webview_pump(void) {
    MSG msg;
    int handled = 0;

    while (handled < 64 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        handled++;
    }

    return handled;
}

#else

int sysl_webview_pump(void) { return -1; }

#endif
