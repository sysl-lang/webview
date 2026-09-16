// One turn of the PLATFORM's event loop, for a program whose own loop owns the thread.
//
// webview's C API has `webview_run`, which takes the thread and does not give it back until the
// window closes. A program that already has an event loop -- libuv, in everything this is written
// for -- cannot give the thread away, so it drives the platform's loop from inside its own instead:
// a repeating timer whose callback is `sysl_webview_pump`.
//
// **This is the only thing in this package that is not webview.** webview publishes no way to take a
// single turn, so the turn is taken against the platform directly.

#ifndef SYSL_WEBVIEW_PUMP_H
#define SYSL_WEBVIEW_PUMP_H

// Handles whatever the platform has already delivered and returns at once.
//
// Answers how many platform events were dispatched, or **-1 where this platform's loop could not be
// reached** -- which is every platform but macOS, GTK and Windows, and any of those three whose
// symbols are not in the process.
int sysl_webview_pump(void);

#endif
