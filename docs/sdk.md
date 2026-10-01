# Writing SIEOS applications

SIEOS ships headers and libraries for three things an application may want:

| Library | Header | What it gives |
|---------|--------|---------------|
| `libfacet` | `<facet/facet.h>` | windows on the Facet desktop, drawing, widgets, the desktop's colours |
| `libsia` | `<sia/sia.h>` | the assistant: questions and conversations with the registered model |
| `libsieos` | `<sieos.h>` | SIEOS extensions (system information, network helpers) over the C library |

The ABI headers are there too (`<sieos/*.h>`). Everything is installed in two places:

- **The cross toolchain's sysroot** (`make sdk`; `make` does it as part of the disk):
  `build/cross/bin/x86_64-pc-sieos-gcc app.c -lfacet -lsia`.
- **The hard disk**, for the native GCC: `cc app.c -lfacet -lsia` in a SIEOS terminal.
  The examples are in `/usr/src/examples`.

The shared libraries (`libfacet.so.1`, `libsia.so.1`) are in `/usr/lib` on every SIEOS
root, and programs link them by default. For static programs use
`-static -lfacet -lsia -ltls -lsieos`. `make sdk-test` builds the examples both ways
using only the SDK.

## Facet applications

Facet's own applications are ordinary programs built this way: `facet-terminal`,
`facet-files`, `facet-viewer`, `facet-monitor`, `facet-network`, `facet-clock`,
`facet-about` and `facet-message` (sources in `user/facet-apps/`).

A program started from the desktop, or from a terminal on it, finds the desktop through
`FACET_DISPLAY`. A minimal application:

```c
#include <facet/facet.h>

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    gfx_text(s, c.x + 10, c.y + 10, "Hello, Facet", C_TEXT);
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    struct fct_view *v = fct_view_new("Hello", 300, 120);
    v->draw = draw;
    return fct_main();
}
```

### Views

A **view** is a window with callbacks. `fct_main()` runs the event loop and returns when
the last view is gone. The callbacks:

| Callback | When |
|----------|------|
| `draw(v, surface, content)` | the view needs redrawing: after `fct_view_invalidate(v)`, a resize, and when it first appears; `content` is `(0, 0, width, height)` |
| `key(v, const struct fct_key *)` | a key went down (`value` 1) or up (0); `code` is the scan code (`FCT_KEY_UP` ...), `ascii` the character, `mods` `FCT_MOD_SHIFT`/`CTRL`/`ALT` |
| `mouse(v, x, y, kind, buttons)` | `FCT_MOUSE_DOWN`, `UP`, `MOVE` (while dragging) or `DOUBLE`, in content coordinates; with the `FCT_WIN_POINTER` flag also moves without a button, the right and middle buttons, and `FCT_MOUSE_WHEEL` |
| `tick(v)` | about four times a second |
| `resized(v)` | the user resized the window (before the redraw) |
| `pollfd(v)` / `readable(v)` | a descriptor the loop should watch, and "it is readable": terminals watch their pty, the chat example its sia request |
| `text(v, line)` | the desktop typed a line into the window (only for `FCT_WIN_ASSISTANT` windows, the assistant terminal) |
| `destroy(v)` | the view is going away: the user closed it, or `fct_view_close(v)`; free `v->app` |

`fct_view_create(&attr)` takes a `struct fct_window_attr`:
- the title and size;
- a position: `FCT_POS_AUTO` to cascade, `FCT_POS_CENTER` for dialogs, or screen
  coordinates (the screen size is `fct_screen(&w, &h)`);
- the smallest content size;
- flags: `FCT_WIN_POINTER` asks for every pointer event over the content (hover moves,
  the right and middle buttons, the wheel: `FCT_MOUSE_WHEEL`, with the notches, > 0 down,
  in the low-level `struct fct_event`'s `wheel`). Without it a window gets the left
  button's presses, releases and drags.

`ui_button` draws a button pressed while the left button is held on it, and for at least
150 ms after a click, by itself: an application only draws its buttons.

`fct_view_set_title` retitles a view.

`fct_clipboard_set(text, len)` puts text on the desktop's clipboard and
`fct_clipboard_get(&len)` returns a copy of it (NUL-terminated, to free; NULL when
empty): the session's programs share it (the web browser's copy and paste, the
terminals' Ctrl+Shift+C and Ctrl+Shift+V).

A text field is a `struct fct_field` (its `text`, the cursor and the selection;
`masked` for a password, shown as dots and never copied). `fct_field_set` fills
it, `fct_field_draw(s, r, f, focus, hint)` draws it in `r` (the hint while empty
and unfocused), `fct_field_mouse(f, r, x, y, kind)` handles every mouse event
(a click places the cursor, a drag selects, a double-click selects the word;
true when it took the event) and `fct_field_key(f, key)` the keys: Left, Right,
Home, End (with Shift: selecting), Backspace, Delete, Ctrl+A, Ctrl+C, Ctrl+X,
Ctrl+V and typed characters. It returns `FCT_FIELD_CHANGED` when the text
changed, `FCT_FIELD_MOVED` when only the cursor or the selection did, and
`FCT_FIELD_NONE` for a key it leaves to the application (Enter, Tab, Escape, Up,
Down, other Ctrl keys).

### Drawing

`<facet/gfx.h>` draws into a 32-bit surface (`0x00RRGGBB`, clipped to `s->clip`):
- lines, rectangles, circles, triangles;
- gradients, rounded rectangles, shadows, alpha blending;
- blits;
- text in rows of `FONT_H` (16) pixels: `gfx_text`, `gfx_text_bold`, `gfx_text_scaled` (a
  heading, scale times the size), `text_width`, and `text_fit` (how much fits in a
  width).
  - The text is UTF-8, drawn in DejaVu Sans at 13 px.
  - `gfx_char`, `gfx_text_mono` and `gfx_cell_w`/`gfx_cell_h` use a monospace cell, for
    terminals and tables.
  - Without the fonts in `/usr/share/fonts/dejavu`, or with `FACET_FONT=bitmap`, the
    8×16 bitmap font is used instead. `FACET_FONT_SIZE` sets the size.

`<facet/font.h>` gives TrueType fonts at any size:

```c
struct fct_font *f = fct_font_load("/usr/share/fonts/dejavu/DejaVuSans.ttf");
struct fct_face *h1 = fct_face_new(f, 28);                /* 28 px */
fct_face_draw(s, h1, x, y + fct_face_ascent(h1), "Hello", C_TEXT);   /* y: the baseline */
```

`fct_ui_face(FCT_FONT_SANS / _BOLD / _MONO / _MONO_BOLD)` returns the desktop's own faces,
and `fct_ui_face_px` returns them at another size. The glyphs are anti-aliased and
cached per face, and kerning is applied.

`<facet/ui.h>` has the desktop's buttons, panels, meters, icons and the SIEOS logo.
`<facet/theme.h>` has its colours (`C_CONTENT`, `C_TEXT`, `C_ACCENT`, ...).
- The colours are the current skin's (`<facet/skin.h>`: Strata, BeOS style, IRIX style).
- When the user changes skin, the view loop redraws every view, so an application that
  draws with `C_*`, `ui_*` and `icon_draw` follows it. With the low-level API, an
  `FCT_SKIN` event says the skin changed.
- `gfx_poly`, `gfx_stroke` and `gfx_ellipse_aa` draw antialiased shapes.

### Without views

`fct_open`, `fct_window_create`, `fct_window_surface`, `fct_window_damage` and
`fct_next_event` expose the protocol directly, for programs with their own loop. The
library still allocates the buffers and replaces them on resize. The protocol itself is in
`<facet/protocol.h>`:
- **Transport:** fixed 112-byte messages over the AF_UNIX socket `$FACET_DISPLAY`
  (`/tmp/.facet-<uid>`).
- **Buffers:** pixels in POSIX shared memory whose descriptor is passed with `SCM_RIGHTS`;
  Facet maps it read-only.

## The assistant: libsia

libsia uses the model registered with sia for the user running the program
(`~/.sia/config`, from sia's first-use setup or the build). It reads the endpoint, model
name and key, works out the kind of endpoint, and speaks HTTPS (TLS 1.3, libtls). The
program never handles any of that:

```c
#include <sia/sia.h>

char err[256];
char *a = sia_complete(NULL, "Name three prime numbers.", err, sizeof(err));
```

- `sia_available()` and `sia_model()`: is a model registered, and its name.
- `sia_complete(instructions, prompt, err, n)`: one answer (`malloc`'d).
  `instructions` is the system prompt; `NULL` gives sia's default.
- `sia_chat_new(instructions)`, `sia_chat_send(c, text, err, n)`, `sia_chat_reset`,
  `sia_chat_free`: a conversation that remembers earlier turns.
- `sia_chat_send_stream(c, text, piece, ctx, err, n)`: the same, and `piece(ctx, utf8)`
  gets each part of the answer as the model writes it. Parts end on UTF-8 character
  boundaries.
- `sia_chat_send_async(c, text, err, n)` returns a descriptor that becomes readable as the
  answer arrives, which keeps an interactive program responsive: the request runs in a
  child process.
  - Each time the descriptor is readable, `sia_chat_poll(c, &done, err, n)` returns the
    new text. `done` is set once the answer is complete (or on failure, which returns
    `NULL`).
  - `sia_chat_result(c, err, n)` waits for the whole answer instead.
  - `sia_chat_cancel` abandons the request.
  - In a Facet view, return the descriptor from `pollfd` and call `sia_chat_poll` in
    `readable`, as `examples/facet-chat/chat.c` does.
- `sia_plain(utf8)`: an answer as text for the 8-bit console and Facet fonts.

The fuller agent interface, with tools the model can call and the desktop channel, is what
`/bin/sia` and the Facet strip use (`user/libsia/libsia.h`). It is not installed yet.

## Examples

`user/examples/` (on the disk, `/usr/src/examples/`):

| Example | Libraries | What it shows |
|---------|-----------|---------------|
| `facet-hello/hello.c` | libfacet | a window, drawing, a button, keys |
| `sia-ask/ask.c` | libsia | `ask "question"` and a conversation on the command line |
| `facet-chat/chat.c` | libfacet, libsia | a chat window with the model, requests in the background |

In a terminal on the SIEOS desktop:

```sh
cc -O2 -o chat /usr/src/examples/facet-chat/chat.c -lfacet -lsia
./chat &
```
