# MiR: Make it Real

MiR makes the applications its user describes. It is a package, **mir**, not part of the base system: SiPM installs it (or `pkg install mir`). The user says what they want, in their own words; sia asks what it needs to know, then writes the program in C or C++, builds it with SIEOS's own gcc and g++, runs it, tests it and fixes it until it works. The user tries it, asks for changes the same way, and has it installed.

## Using it

- **Open it:** ask sia ("make me an app that…", in the strip or a sia terminal), or choose *MiR (make an app)* in the SIEOS menu. `facet-mir "REQUEST"` opens it with a request. sia offers it, and the menu shows it, once the package is installed; before that, sia says how to install it.
- **The window** is tall and narrow. It shows the conversation: what the user asked, what sia answers, and what it does meanwhile (`> write main.c`, `> build (make)`, the compiler's output, the tests' results).
  - **Send** (or Enter) sends the request typed below. While sia works, Escape or **Stop** interrupts it.
  - **Run** starts the current application: a window application on the desktop, a terminal program in a shell terminal.
  - **Files** opens its folder, and **Install** asks sia to install it. sia asks before installing: **Yes** or **No**.
  - Lines of the conversation are selected with the mouse (a double-click selects a whole message) and copied with Ctrl+C. The wheel, Page Up and Page Down scroll it.
- **Projects:** each application is a folder, `~/apps/NAME`, holding:
  - its sources;
  - a `Makefile` (`make`, `make test`, `make clean`);
  - `test.sh` (its tests, which sia writes);
  - `mir.json`: the name, title, kind (window or terminal), language, summary and whether it is installed.

  Say "continue NAME" to go on with one.
- **Installed applications:**
  - the program is copied to `~/apps/bin`, which every shell has on its `PATH`;
  - window applications also appear in the SIEOS menu's **My apps**.
- **A compiler is needed.** SIEOS installed on a disk or a USB drive has gcc and g++; the live CD does not, and MiR says so.

## The models

**Settings > Assistant** records several models (each an endpoint, a model or deployment name and a key) and marks the one in use:
- **New** adds a model;
- **Use this one**, or a click on a model's dot, switches to it (the strip's sia follows);
- **Remove** deletes it;
- **Test** checks that it answers, and whether it sees images.

The models are kept in `~/.sia/models`; the one in use is also copied to `~/.sia/config`, which every program reads. Both files are mode 0600.

**Whether the model sees images matters to MiR:**
- **The test:** the model is shown a picture of a four-digit number and asked to read it. It runs when the user presses Test, or the first time MiR uses an untested model. Its result is kept with the model (`vision=yes` or `no`).
- **A model that sees images** gets `app_screenshot`. It looks at a window application after starting it and after using it, and checks its layout, its text and the result of its clicks.
- **A model that does not:**
  - MiR says so in its window;
  - sia still checks that the application starts, stays up and responds to input;
  - the user has to look at the window themselves, or choose a model that sees images.

## How it works

- **The window and the agent:**
  - The package (`user/mir`, recipe `ports/pkgs/mir`) holds:
    - `/usr/pkg/bin/facet-mir` (the window, `mir.c`);
    - `/usr/pkg/libexec/mir-agent` (`mir-agent.c` with the tools, `apptools.c`);
    - `/usr/pkg/share/mir` (the guide, templates and examples).

    It is built from the repository (`source = tree:user/mir`) on the SDK's libsia.
  - The window runs `mir-agent`: libsia's agent loop (`sia_agent_main`, the same as sia-agent's) with the role `SIA_ROLE_APP`, MiR's instructions (`sia_set_instructions`), its limits (`sia_set_limits`) and the application tools instead of the commands and the desktop tools.
  - The two speak in JSON lines (`user/sia/sia-agent.c`), so the window stays responsive while sia works.
  - The agent inherits Facet's desktop channel, which it needs to start and watch window applications.
- **The tools** (`user/libsia/apptools.c`):

  | Tool | Does |
  |------|------|
  | `app_list`, `app_open` | the projects; continue one |
  | `app_create` | a new project from a template (`/usr/pkg/share/mir/templates`: window or terminal, C or C++) that builds |
  | `app_describe` | change its title or summary |
  | `app_files`, `app_read`, `app_write` | its files (whole files); `app_read` also reads `/usr/include` and `/usr/pkg/share/mir` |
  | `app_build` | `make`, with the compiler's messages when it fails |
  | `app_run` | a terminal program: runs to its end with arguments and stdin; its exit status (or crash) and output. A window application: started, its window awaited, watched for a few seconds; it stays open |
  | `app_input` | text, a key or a click into the running window |
  | `app_screenshot` | the window's content as an image for the model (only for a model that sees images) |
  | `app_stop`, `app_test` | close it; `make test` |
  | `app_install` | `~/apps/bin` and My apps (the only tool that asks the user first) |

- **The instructions:** MiR's are `/usr/pkg/share/mir/guide.txt`:
  - how to work (understand, plan small, write, build, test, fix, report);
  - what SIEOS offers programs;
  - a summary of libfacet's interface.

  `/usr/pkg/share/mir/examples/animation.c` shows a loop for animations and games.
- **Limits:**
  - The tools only touch the current project's folder: relative paths, no `..`, no hidden files, and nothing written through a symbolic link.
  - Programs run as the user, in their folder, without the desktop channel. Their limits are: processor time (builds 5 minutes, terminal runs 20 seconds by default), memory (2 GB for builds, 1 GB for runs) and file size.
  - A request may take up to 80 model calls.
  - A model may stay silent for 10 minutes before or while answering. A model that reasons can think that long before writing a whole program. MiR shows the time spent ("sia is thinking 2:20"), and Escape interrupts. After that, the request fails with "no answer within 600 seconds". The strip and the terminals wait 5 minutes.
- **Images:**
  - A screenshot is a PNG made by Facet from the window's buffer (`{"op":"snapshot"}`; libsia's `sia_png`, with its own deflate: a few kilobytes for a window).
  - It is attached to the model's next turn as a user message.
  - Once seen, it is replaced by its caption, so the conversation stays small.
- **Testing without a model:** `tools/mock_azure.py CERT KEY PORT LOG KEY MODEL yes|no` plays a model that does or does not see images:
  - it reads the vision test's number from the PNG;
  - it scripts a MiR session that makes a window counter (with a compile error to fix, a click and two screenshots to compare) or a C++ word-count tool with its tests.
