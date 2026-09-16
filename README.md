# webview

A native window with the platform's own browser engine in it, for
[sysl](https://github.com/sysl-lang/sysl) — bound to
[webview](https://github.com/webview/webview).

No bundled engine and no Chromium: **WKWebView** on macOS, **WebKitGTK** on Linux, **WebView2** on
Windows. A whole application is a window, some HTML, and a handful of functions the page can call.

```sysl
import sh.sysl.webview.{webview, Hint}

var clicks = 0

val w = webview()?

w.title("sysl · webview")?
w.size(560, 420, Hint.Minimum)?

w.bind("count", (args) ->
    clicks += 1
    Ok(f"${clicks}%d"))?

w.html(page)?
w.run()?
```

```
sh/sysl/webview/
    webview.sysl    the binding
    tests.sysl      7 tests — see Testing, which is the honest part of this file
    c/
        c.sysl      webview as C declares it, and the numbers its header computes
package.hocon       who this package is, and what it needs of the machine
```

**There is no shim** — no C at all in this repository.

## Installing it

webview publishes **no releases and no tags**, ships **no pkg-config file**, and is not in
Homebrew. So the tap carries a formula that pins the commit this binding was written against, builds
the shared library with CMake, and writes the `.pc` upstream lacks:

```
brew install sysl-lang/tap/webview
```

```hocon
dependencies {
  webview { git = "github.com/sysl-lang/webview", version = "0.2.0" }
}
```

Nothing else is needed — pkg-config supplies the include and link lines, so no flags are passed.

**On Linux** the distribution's own `libwebview` (where there is one) works if it ships a `.pc`;
otherwise build it the way the formula does. `Formula/webview.rb` in the tap is the reference.

### Why the formula and not just a build

Two reasons, and the second is the one that bites.

**Upstream ships no `.pc`.** Without one a consumer passes `--include-path` and `--link-path` by
hand, on every build, forever.

**And a stock build cannot be run.** webview's CMake gives the library a `SOVERSION`, so the install
name is `@rpath/libwebview.0.12.dylib`. sysl's `--link-path` emits `-L` and no `-rpath`, so a
program links cleanly and then dies at startup:

```
dyld: Library not loaded: @rpath/libwebview.0.12.dylib
      Reason: no LC_RPATH's found
```

which reads as a missing library sitting exactly where it was said to be. The formula passes
`-DCMAKE_INSTALL_NAME_DIR=#{opt_lib}` so the library says where it lives. (The underlying gap is
filed against the compiler as card 0414.)

## The two facts the API is shaped by

**`run` blocks and does not return until the window closes**, and it must be called on the thread
that made the webview. So everything is set up first — title, size, content, and every `bind` —
and anything that happens afterwards happens inside a callback.

**A `bind` callback is the only channel from the page to sysl.** `bind("name", f)` puts an
asynchronous `window.name(...)` on the page; calling it runs `f` with the arguments as a JSON string,
and what `f` answers resolves or rejects the page's promise. `eval` is the way back.

```sysl
w.bind("save", (args) -> Ok("null"))?     // args is a JSON array, e.g. "[\"note\",42]"
w.eval("document.title = 'saved'")?        // fire-and-forget; answers nothing
```

`on_load(js)` runs a script before every page's own scripts on every navigation, which is where a
binding is announced to a page that needs it early.

**Nothing here parses or builds JSON**, deliberately: a binding that picked a JSON library would pick
it for every consumer. `sh.sysl.json` is one import away.

### Closures are retained — a bound one for ever, a dispatched one until it has run

A bound closure is kept for the life of the webview. It has to be — the page may call it at any
moment until the window closes, and nothing on this side can know when the last call was made.
`unbind` stops the page seeing the name and does not release the closure.

`dispatch(f)` — the one method safe to call from another thread — **releases its closure once it has
run**, as of 0.3.0. It used to be kept on the same list a binding is on, so a program dispatching
once a frame grew its retained set for ever; there is no longer a reason to dispatch sparingly.

## A program that already has an event loop: `pump`

**`run` takes the thread and does not give it back**, which is the whole of the problem for a program
built on libuv, or on anything else with a loop of its own. Such a program keeps its loop and takes
one turn of the *platform's* loop from inside it:

```sysl
val w = open_window()?

w.html(page)?

tick.start(0, 8, () ->                 // a libuv timer, about 120 Hz
    w.pump().unwrap()

    if w.closed()
        lp.stop())?

lp.run()?                              // the ordinary libuv loop, which knows nothing about windows
```

- **`pump()`** handles whatever the platform has already delivered and returns at once, answering how
  many events it dispatched. Sixty-four is the cap, so a burst cannot hold up the caller's own timers.
- **`closed()`** is what ends the loop, since a pump never returns of its own accord the way `run`
  does.
- **`run` and `pump` are alternatives.** Pick one.

**The tick is the latency**, so 8 ms is a click answered within 8 ms; an idle process at that rate
costs nothing measurable.

`pump.c` is the only thing in this package that is not webview — webview publishes no way to take a
single turn, so it is taken against the platform: `nextEventMatchingMask:` + `sendEvent:` on macOS,
`g_main_context_iteration` on GTK, `PeekMessage`/`DispatchMessage` on Windows. **Dequeuing alone is
not enough on macOS**: running the run loop without `sendEvent:` draws the window and answers no
clicks. Every symbol is reached with `dlsym` rather than linked, because a `@link` goes on every
consumer's line on every platform and `-lobjc` is not a thing a Linux box has.

### A page's call answered later: `bind_async`

A `bind` closure has to produce the whole answer before it returns, so anything it waits for is
waited for with the window frozen. `bind_async` hands the closure the call's **id** instead and
answers nothing; the program calls `answer(id, …)` whenever it likes.

```sysl
w.bind_async("load", (call, args) ->
    read_file(path, (bytes) -> w.answer(call, Ok(as_json(bytes))).unwrap()))?
```

The id is a sysl string and owns its bytes, so it may be kept for as long as the program wants — a
turn, a request, a second. Never answering leaves the page's promise pending, exactly as an unsettled
JavaScript promise would be; answering twice is `NotFound` on the second.

## The boundary

`15 §7` names three shapes only C can reach. webview has none of them: the handle is allocated by the
library, every error code and hint is an `enum` enumerator a `c const` block reads, and both
callbacks take nothing but pointers and an `int` — so `&f` is an address C can call and **no
`@export` is needed**. The one struct that crosses does so by address, and its size and three
offsets are asserted against the C.

**Two things to know before writing any C against this library:**

- **The C header is `webview/api.h`, never `webview/webview.h`.** The latter unconditionally
  includes `c_api_impl.hh`, which is C++; including it from C fails with a wall of errors naming a
  C++ header rather than naming the mistake.
- **`offsetof` needs an explicit `<stddef.h>`.** Without it the `c const` probe fails with *"call to
  undeclared function 'offsetof'"* — and the diagnostic points at the *first* `c const` block in the
  file rather than the one that used it.

### webview is C++, and that costs this package nothing

webview is a C++14 header-only library with a C API layered on it, and **sysl compiles `.c` and
nothing else** — `Project.walkModules` takes `List(".c")` and `PackageConfig.checkCSource` refuses
any other extension. So webview could never be *vendored* here the way miniz or stb are.

It does not matter, because a prebuilt shared library is just a shared library. The one the formula
builds records WebKit, libc++ and libobjc as its own load commands, so the link line is a single
`-lwebview` with no frameworks on it. That last part is load-bearing: **a sysl `@link` directive
names a library and cannot be a `-framework` flag**, so a *static* libwebview could not be linked
from sysl at all.

## Ownership

`Webview` is **one type**, owning the C pointer and carrying the destructor.

**Until v0.2.0 it was two** — a private handle with the destructor plus a copyable value holding a
`&` to it — and this section said the split was forced, because a method receives `self` by value
and so cannot put the box it was called through into anything it builds. The premise is true and the
conclusion was wrong: **`&self` hands a method the box itself**, and had done all along. Nobody
tried it, and the workaround worked, so nothing ever failed to say otherwise. `sysl-lang/lmdb` made
the same mistake independently, each binding reading the other.

The hazard the split was avoiding is real: constructing a fresh value around the same pointer hands
out a second owner and so a second `webview_destroy`. What removes it is `&self` rather than a
second type.

**A method that stores something takes `*self`, and until v0.3.1 three of them took `self`.** A
receiver written `self` is *by value — the method gets a copy*, so `bind`, `bind_async` and
`dispatch` each built their box, pushed it onto **the copy's** list, and returned; the copy died with
the call and released the only strong reference, while webview kept the bare address the push had
handed back. The caller's `Webview` held nothing. The first time a page called a bound name the
trampoline read a freed closure and the process died in `binding_ctx_t::call`, eighteen frames under
a `pump`. This section used to close by saying nothing here needed a real receiver, *because the
closures it keeps are stored on itself* — which is exactly why the receiver has to write through to
the caller's value, and not a reason it need not.

`*self` rather than `&self`: a counted receiver is refused on a stack value, which would make the
type unusable unboxed and untestable without a window, and `*self` is identical to `&self` when the
call comes through a box. A method that stores nothing — `title`, `answer`, `closed` — keeps `self`.
`sysl-lang/lmdb`, whose transactions *are* handed out to a caller, is where `&self` does real work;
`reference/declarations.md § A '&self' method may keep what it was called on` is where that form is
written down.

## Testing — and this is the honest part

```
sysl test .
```

**12 passed, 0 failed.** They cover the struct layout against the C, the version the library reports,
every error code and its round trip, the size hints, that all three trampolines have addresses C can
call, that `pump` takes a real turn of the platform's event loop and that two hundred turns of an
idle one dispatch nothing, and — the three that are new in 0.3.1 — that a registration made by
`bind`, `bind_async` or `dispatch` is on **the caller's** webview afterwards, and that both
trampolines hand their closure sysl strings rather than C's buffers.

**The pump is the one thing here that needs no window**, the platform's event loop belonging to the
process rather than to a window, so those two are real end-to-end checks of the shim: it compiled, it
linked, sysl reached it, and it found the platform's symbols.

**A null `webview_t` is the other**, and it is what lets the receiver be checked at all. Every entry
point in webview's C API runs its work inside an `api_filter`, and the `cast_to_webview` a null
handle fails is a C++ `throw` that filter catches and reports as `WEBVIEW_ERROR_INVALID_ARGUMENT`.
So a `bind` on a handle with nothing behind it really reaches C, is really refused, and refuses
without faulting — which leaves the sysl either side of it, the half that was wrong, as the only
thing under test.

> **They do not open a window, and they cannot.** `webview_create` calls `[NSApplication run]` and
> blocks until the application-did-finish-launching notification, which never arrives in a session
> with no window server attached — `launchctl managername` answers `Background` rather than `Aqua`.
> Verified in plain C as well as through sysl, so it is the environment and not this binding. A test
> that opened a window would **hang rather than fail**, which is the worst way for a test to be
> wrong.

So the suite proves the boundary, the pump, and nothing else past `create`. **Everything else is
proven by [`sysl-lang/webview-demo`](https://github.com/sysl-lang/webview-demo)**, which opens a
window, binds two sysl closures, and lets a page call them — run from a terminal in a graphical
session. If you are evaluating this package, run that, because it is the part the green tick above
does not cover.

> **On a shared Mac, "a graphical session" means the account that is logged in at the console.** A
> shell belonging to any other account has no `Aqua` session however ordinary it looks, so
> `webview_create` hangs there exactly as it does over ssh. `stat -f "%Su" /dev/console` names the
> account that can run the demo; `launchctl print gui/$(id -u)` answering *Domain does not support
> specified action* is the same fact from the other side.

### What the demo does not cover, and `sysl-lang/webview-demo` is the place to put it

The demo predates `pump` and still calls `run`, so **nothing in this repository exercises the pumped
loop with a real window**: that a click arrives within a tick, that `eval` reaches the page between
turns, that a `bind_async` answer resolves a promise the page is already awaiting, and what an idle
pumped process costs. Those want a second program beside the demo.

### AddressSanitizer

```
SYSL_EXTRA_CFLAGS="-fsanitize=address -g" sysl test .
```

Covers **the sysl half and `pump.c`** — webview itself arrives through `pkg_config`, so its objects
are somebody else's build and no flag of ours instruments them, but the shim is C in this tree and so
is compiled with the flag like any vendored source. Clean at 0.3.1 — 12 passed, 0 failed — with
nineteen `asan` symbols in `nm -u` to say the binary was really instrumented.

> **Before sysl 0.0.104** a green ASan run over an unchanged tree proved nothing: `sysl test` cached
> its artifact and the key did not include `SYSL_EXTRA_CFLAGS`, so the uninstrumented binary was
> replayed. Fixed in 0.0.104 — a sanitizer run rebuilds now. `nm -u <binary> | grep -c asan` is still
> worth running, and answers the question it was always best at: *is this binary instrumented*.

## Not bound yet

- **`get_native_handle`** is in the `c` module and not wrapped. What to do with an `NSWindow` is not
  something this package can help with, and typing it would be a claim about a platform.
- **Embedding in an existing window.** `webview_create` takes a native window to embed in; this
  binding always passes null and makes its own.

## Licence

The binding is ISC. webview itself is not carried here — it is the library the machine has, under
the MIT licence.
