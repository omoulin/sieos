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
vklogo --info             # Vulkan in a window: the SIEOS logo turning (SIEOS menu: Vulkan logo)
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
does compute, and rendering; its images are shown in Facet windows through
**`VK_SIEOS_facet_surface`**, SIEOS's own instance extension (not a Khronos one):
- `vkCreateFacetSurfaceSIEOS` makes a `VkSurfaceKHR` from a `fct_window *`
  (`VkFacetSurfaceCreateInfoSIEOS`). Its header is `<vulkan/vulkan_facet.h>`, which
  `<vulkan/vulkan.h>` includes when `VK_USE_PLATFORM_FACET_SIEOS` is defined.
- Enable `VK_KHR_surface` and `VK_SIEOS_facet_surface` on the instance, and get the
  function with `vkGetInstanceProcAddr`, as for any extension.
- **The surface:** its current extent is the window's size; formats
  `B8G8R8A8_UNORM` and `B8G8R8A8_SRGB` (the window's XRGB pixels); present modes FIFO,
  mailbox and immediate; 2 to 8 images.
- **Presenting** copies the image into the window and shows it. After the window
  changes size, acquiring and presenting return `VK_SUBOPTIMAL_KHR`: make the swapchain
  again at the new extent.

```c
#define VK_USE_PLATFORM_FACET_SIEOS
#include <vulkan/vulkan.h>

const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_SIEOS_FACET_SURFACE_EXTENSION_NAME };
/* vkCreateInstance with exts, then: */
PFN_vkCreateFacetSurfaceSIEOS create =
    (PFN_vkCreateFacetSurfaceSIEOS)vkGetInstanceProcAddr(instance, "vkCreateFacetSurfaceSIEOS");
VkFacetSurfaceCreateInfoSIEOS ci = { .sType = VK_STRUCTURE_TYPE_FACET_SURFACE_CREATE_INFO_SIEOS,
                                     .window = win };      /* a fct_window * */
VkSurfaceKHR surface;
create(instance, &ci, NULL, &surface);
/* then a device with VK_KHR_swapchain and a swapchain on the surface, as on any system */
```

```sh
gcc prog.c -lvulkan -lfacet
```

`user/mesa-demos/vklogo.c` is a whole program: the logo's mesh, a depth buffer, a
pipeline with push constants, and the swapchain made again when the window is resized.

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
  platform; the program's path comes from `getexecname()`; `VK_SIEOS_facet_surface` is
  among the instance extensions it knows, and it makes the drivers' surfaces from the
  ones it gives programs (asking each driver for `vkCreateFacetSurfaceSIEOS` by name).
- **The extension** is in the Vulkan headers (`ports/vulkan-headers/sieos.patch`:
  `vulkan_facet.h`, and the surface's platform in `vk_icd.h`) and in Mesa's `vk.xml`;
  Mesa's window-system code shows the swapchain's images in the window
  (`src/vulkan/wsi/wsi_common_facet.c`), and lavapipe offers the extension.
- The build host needs `cmake`, `meson`, `ninja` and `glslangValidator` (README).
