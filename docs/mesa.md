# OpenGL, EGL and Vulkan: mesa

The package **mesa** gives SIEOS programs the standard graphics interfaces:
- **OpenGL ES 3.2 and OpenGL 4.6**, through **EGL 1.5**, by Mesa's llvmpipe;
- **Vulkan 1.4**, by Mesa's lavapipe, through the Khronos loader (package
  **vulkan-loader**).

Both draw with the processor, not a graphics card. LLVM compiles the shaders to
x86 code, using AVX2 or AVX-512 when the processor has them, and the work is shared
across the processor's cores. That is fast enough for 2D and simple 3D, demos and
tools; it is not a GPU. Hardware acceleration needs a kernel GPU driver, a later
milestone.

```sh
pkg install mesa          # with zlib and vulkan-loader
glcube --info             # OpenGL ES in a window (also in the SIEOS menu: OpenGL cube)
vkcompute                 # a Vulkan compute test
```

## EGL on Facet

EGL's window system on SIEOS is Facet: its platform "facet", the default one.
- **A window surface** is a Facet window. The program makes it with libfacet,
  and gives EGL its `fct_window *`, SIEOS's `EGLNativeWindowType`.
- **The program keeps its window:** its events (`fct_next_event`), its title,
  its size. EGL only draws into it. A surface follows its window's size.
- **`eglSwapBuffers`** copies the frame into the window's buffer (32-bit XRGB),
  and Facet shows it.
- **Configs:** window surfaces are 8-bit RGB(A), with or without depth and
  stencil; `EGL_EXT_buffer_age` is supported.
- **Also:** pbuffers, surfaceless contexts (`EGL_KHR_surfaceless_context`), and
  the surfaceless platform (`EGL_PLATFORM=surfaceless`) for off-screen work.

The shortest program (glcube's whole source is `user/mesa-demos/glcube.c` in SIEOS's
repository):

```c
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <facet/facet.h>

fct_display *d = fct_open();
struct fct_window_attr a = { .title = "demo", .x = FCT_POS_AUTO, .y = FCT_POS_AUTO, .w = 480, .h = 400 };
fct_window *win = fct_window_create(d, &a);

EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
eglInitialize(dpy, NULL, NULL);
EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
EGLConfig config; EGLint n;
eglChooseConfig(dpy, attribs, &config, 1, &n);
EGLSurface surf = eglCreateWindowSurface(dpy, config, win, NULL);
EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
eglMakeCurrent(dpy, surf, surf, ctx);
for (;;) {
    /* fct_next_event(d, &ev, 0) ...; glClear(...); draw ... */
    eglSwapBuffers(dpy, surf);
}
```

Build it with `gcc demo.c -lEGL -lGLESv2 -lfacet -lm`. The headers and libraries are in
`/usr/pkg/include` and `/usr/pkg/lib`, which the compiler searches.

**Desktop OpenGL** (4.6, compatibility profile) is there too: `eglBindAPI(EGL_OPENGL_API)`
before `eglCreateContext`, and its functions from `eglGetProcAddress` (there is no
`libGL.so`, which is GLX's: X11).

## Vulkan

`vulkan-loader` installs `libvulkan.so` and the Vulkan headers. The loader finds
the drivers in `/usr/pkg/share/vulkan/icd.d`, where mesa puts lavapipe's. lavapipe
does compute, and rendering into images. Showing them in a Facet window (a
`VkSurfaceKHR` for Facet) is not done yet: a program copies an image into its
window itself.

```sh
gcc prog.c -lvulkan
```

## How it is built

- **LLVM 22** (its libraries, the X86 target) is cross-built for SIEOS
  (`make llvm-sieos`, into `build/ports/llvm-sieos`; `ports/llvm/sieos.patch`) and
  linked into Mesa's libraries.
- **Mesa 26.2** is built with Meson (`ports/pkgs/mesa/recipe`). Its `sieos.patch`:
  - SIEOS is a system of its own (`sieos`, `__sieos__`), a DRI system without kernel
    GPU drivers, like GNU/Hurd;
  - the EGL platform `facet`: `src/egl/drivers/dri2/platform_facet.c`, through Mesa's
    software rasteriser loader (the frames it presents are copied into the window);
  - SIEOS's EGL native types (`EGL/eglplatform.h`).
- **The Vulkan loader** (`ports/pkgs/vulkan-loader`, `sieos.patch`): SIEOS is a Unix
  platform; the program's path comes from `getexecname()`.
- The build host needs `cmake`, `meson`, `ninja` and `glslangValidator` (README).
