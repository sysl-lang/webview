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

### Closures are retained

A bound closure is kept for the life of the webview. It has to be — the page may call it at any
moment until the window closes, and nothing on this side can know when the last call was made.
`unbind` stops the page seeing the name and does not release the closure.

`dispatch(f)` — the one method safe to call from another thread — retains its closure too, and there
that is a real cost rather than a necessity: **a program that dispatches once per frame grows its
retained set forever.** Dispatch occasionally, to hand a finished result back to the window, and let
the page drive anything per-frame.

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

Nothing here actually needs `&self` today, because a `Webview` hands nothing that outlives a call
back to a caller — the closures it keeps are stored on itself. `sysl-lang/lmdb`, whose transactions
*are* handed out, is where the receiver does real work; `reference/declarations.md § A '&self'
method may keep what it was called on` is where the form is written down.

## Testing — and this is the honest part

```
sysl test .
```

**7 passed, 0 failed.** They cover the struct layout against the C, the version the library reports,
every error code and its round trip, the size hints, and that both trampolines have addresses C can
call.

> **They do not open a window, and they cannot.** `webview_create` calls `[NSApplication run]` and
> blocks until the application-did-finish-launching notification, which never arrives in a session
> with no window server attached — `launchctl managername` answers `Background` rather than `Aqua`.
> Verified in plain C as well as through sysl, so it is the environment and not this binding. A test
> that opened a window would **hang rather than fail**, which is the worst way for a test to be
> wrong.

So the suite proves the boundary and nothing past `create`. **Everything else is proven by
[`sysl-lang/webview-demo`](https://github.com/sysl-lang/webview-demo)**, which opens a window, binds
two sysl closures, and lets a page call them — run from a terminal in a graphical session. If you
are evaluating this package, run that, because it is the part the green tick above does not cover.

### AddressSanitizer

```
SYSL_EXTRA_CFLAGS="-fsanitize=address -g" sysl test .
```

Covers **the sysl half only** — webview arrives through `pkg_config`, so its objects are somebody
else's build and no flag of ours instruments them.

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
