/*
 * vklogo - Vulkan in a Facet window (VK_SIEOS_facet_surface, Mesa's
 * lavapipe): the SIEOS logo in 3D, turning.  The logo of docs/logo.svg as
 * the screen saver draws it: a blue cube seen from a corner (the light face
 * on top, the middle one on the left, the deep one on the right), dark seams
 * along its edges, the amber node with its dark ring on the near corner.
 * Space pauses; Escape or closing the window quits; the window may be
 * resized (the swapchain follows).
 *
 *   vklogo [--info] [--frames N]
 *     --info      print the device and the surface's formats and present modes
 *     --frames N  quit after N frames, printing the frame rate (a test)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define VK_USE_PLATFORM_FACET_SIEOS
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <facet/facet.h>
#include <vulkan/vulkan.h>

#include "logo_frag_spv.h"
#include "logo_vert_spv.h"

#define CHECK(call)                                                                                  \
    do {                                                                                             \
        VkResult r_ = (call);                                                                        \
        if (r_ != VK_SUCCESS) {                                                                      \
            fprintf(stderr, "vklogo: %s failed: %d\n", #call, r_);                                   \
            exit(1);                                                                                 \
        }                                                                                            \
    } while (0)

/* ---------------------------------------------------------------- the logo's mesh */

struct vertex {
    float pos[3], normal[3], color[3];
};

static struct vertex *mesh;
static uint32_t nmesh, capmesh;

static void put(const float p[3], const float n[3], const float c[3])
{
    if (nmesh == capmesh) {
        capmesh = capmesh ? capmesh * 2 : 1024;
        mesh = realloc(mesh, capmesh * sizeof(*mesh));
        if (!mesh)
            exit(1);
    }
    struct vertex *v = &mesh[nmesh++];
    memcpy(v->pos, p, sizeof(v->pos));
    memcpy(v->normal, n, sizeof(v->normal));
    memcpy(v->color, c, sizeof(v->color));
}

static void rgb(float c[3], unsigned hex)
{
    c[0] = ((hex >> 16) & 0xFF) / 255.0f;
    c[1] = ((hex >> 8) & 0xFF) / 255.0f;
    c[2] = (hex & 0xFF) / 255.0f;
}

/* One face of a cube of half-size h: the square at axis = sign * d, inset by e. */
static void cube_face(int axis, float sign, float h, float d, float e, unsigned color)
{
    static const float sq[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    float p[4][3], n[3] = { 0, 0, 0 }, c[3];
    n[axis] = sign;
    rgb(c, color);
    for (int i = 0; i < 4; i++) {
        p[i][axis] = sign * d;
        p[i][(axis + 1) % 3] = sq[i][0] * (h - e);
        p[i][(axis + 2) % 3] = sq[i][1] * (h - e);
    }
    /* counter-clockwise seen from outside, whichever the sign */
    static const int front[6] = { 0, 1, 2, 0, 2, 3 }, back[6] = { 0, 2, 1, 0, 3, 2 };
    const int *t = sign > 0 ? front : back;
    for (int k = 0; k < 6; k++)
        put(p[t[k]], n, c);
}

/* A point of a sphere (centre c, radius r) at latitude/longitude. */
static void sphere(const float ctr[3], float r, int slices, int stacks, unsigned color)
{
    float c[3];
    rgb(c, color);
    for (int i = 0; i < stacks; i++)
        for (int j = 0; j < slices; j++) {
            float q[4][3], n[4][3];
            for (int k = 0; k < 4; k++) {
                int a = i + (k == 2 || k == 3), b = j + (k == 1 || k == 2);
                float th = (float)M_PI * a / stacks, ph = 2 * (float)M_PI * b / slices;
                n[k][0] = sinf(th) * cosf(ph);
                n[k][1] = cosf(th);
                n[k][2] = sinf(th) * sinf(ph);
                for (int m = 0; m < 3; m++)
                    q[k][m] = ctr[m] + r * n[k][m];
            }
            static const int t[6] = { 0, 2, 1, 0, 3, 2 };
            for (int k = 0; k < 6; k++)
                put(q[t[k]], n[t[k]], c);
        }
}

/* A torus around axis (unit), centred at ctr: radii R and r. */
static void torus(const float ctr[3], const float axis[3], float R, float r, int seg, int sides, unsigned color)
{
    float c[3], u[3], v[3];
    rgb(c, color);
    /* u, v: the plane of the ring */
    float tmp[3] = { fabsf(axis[0]) < 0.9f ? 1.0f : 0.0f, fabsf(axis[0]) < 0.9f ? 0.0f : 1.0f, 0 };
    u[0] = axis[1] * tmp[2] - axis[2] * tmp[1];
    u[1] = axis[2] * tmp[0] - axis[0] * tmp[2];
    u[2] = axis[0] * tmp[1] - axis[1] * tmp[0];
    float l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int m = 0; m < 3; m++)
        u[m] /= l;
    v[0] = axis[1] * u[2] - axis[2] * u[1];
    v[1] = axis[2] * u[0] - axis[0] * u[2];
    v[2] = axis[0] * u[1] - axis[1] * u[0];
    for (int i = 0; i < seg; i++)
        for (int j = 0; j < sides; j++) {
            float q[4][3], n[4][3];
            for (int k = 0; k < 4; k++) {
                int a = i + (k == 1 || k == 2), b = j + (k == 2 || k == 3);
                float ph = 2 * (float)M_PI * a / seg, th = 2 * (float)M_PI * b / sides;
                float dir[3], nn[3];
                for (int m = 0; m < 3; m++)
                    dir[m] = cosf(ph) * u[m] + sinf(ph) * v[m];
                for (int m = 0; m < 3; m++) {
                    nn[m] = cosf(th) * dir[m] + sinf(th) * axis[m];
                    q[k][m] = ctr[m] + R * dir[m] + r * nn[m];
                    n[k][m] = nn[m];
                }
            }
            static const int t[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; k++)
                put(q[t[k]], n[t[k]], c);
        }
}

static void build_logo(void)
{
    const float h = 0.43f, e = 0.055f * 0.43f;
    for (int axis = 0; axis < 3; axis++)                 /* the dark cube: the seams */
        for (int sg = -1; sg <= 1; sg += 2)
            cube_face(axis, (float)sg, h, h, 0, 0x1C1C1C);
    cube_face(1, 1, h, h * 1.004f, e, 0x8FB8E6);         /* +Y: the light face, on top */
    cube_face(2, 1, h, h * 1.004f, e, 0x6A95D2);         /* +Z: the middle one, on the left */
    cube_face(0, 1, h, h * 1.004f, e, 0x4A78BC);         /* +X: the deep one, on the right */
    cube_face(1, -1, h, h * 1.004f, e, 0x4A78BC);
    cube_face(2, -1, h, h * 1.004f, e, 0x4A78BC);
    cube_face(0, -1, h, h * 1.004f, e, 0x6A95D2);
    /* the node on the corner (h, h, h), and its dark ring facing out of the corner */
    const float corner[3] = { h, h, h }, k = 1.0f / sqrtf(3);
    const float axis[3] = { k, k, k };
    sphere(corner, 0.36f * h, 32, 16, 0xD9A35F);
    torus(corner, axis, 0.40f * h, 0.06f * h, 48, 8, 0x1C1C1C);
}

/* ---------------------------------------------------------------- matrices (column-major) */

static void mat_mul(float *r, const float *a, const float *b)
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + row] * b[c * 4 + k];
            t[c * 4 + row] = s;
        }
    memcpy(r, t, sizeof(t));
}

static void mat_rot(float *m, float angle, float x, float y, float z)
{
    float c = cosf(angle), s = sinf(angle);
    float r[16] = { x * x * (1 - c) + c,     y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
                    x * y * (1 - c) - z * s, y * y * (1 - c) + c,     y * z * (1 - c) + x * s, 0,
                    x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c,     0,
                    0, 0, 0, 1 };
    memcpy(m, r, sizeof(r));
}

/* Vulkan's clip space: y down, depth 0..1. */
static void mat_perspective(float *m, float fovy, float aspect, float near, float far)
{
    float f = 1.0f / tanf(fovy / 2);
    float r[16] = { f / aspect, 0, 0, 0, 0, -f, 0, 0, 0, 0, far / (near - far), -1,
                    0, 0, far * near / (near - far), 0 };
    memcpy(m, r, sizeof(r));
}

/* ---------------------------------------------------------------- Vulkan */

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qf;
static VkSurfaceKHR surface;
static VkRenderPass pass;
static VkPipelineLayout layout;
static VkPipeline pipe;
static VkCommandPool cpool;
static VkBuffer vbuf;
static VkDeviceMemory vmem;
static VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
static const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

/* the swapchain and what depends on its size */
static VkSwapchainKHR swapchain;
static VkExtent2D extent;
static uint32_t nimages;
static VkImage images[8];
static VkImageView views[8];
static VkFramebuffer fbs[8];
static VkImage depth;
static VkDeviceMemory depth_mem;
static VkImageView depth_view;

#define FRAMES 2
static VkCommandBuffer cbs[FRAMES];
static VkSemaphore acquired[FRAMES], rendered[8];
static VkFence fences[FRAMES];

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "vklogo: no suitable memory\n");
    exit(1);
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size,
                                    .pCode = code };
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static void destroy_swapchain(void)
{
    vkDeviceWaitIdle(dev);
    for (uint32_t i = 0; i < nimages; i++) {
        vkDestroyFramebuffer(dev, fbs[i], NULL);
        vkDestroyImageView(dev, views[i], NULL);
        vkDestroySemaphore(dev, rendered[i], NULL);
    }
    vkDestroyImageView(dev, depth_view, NULL);
    vkDestroyImage(dev, depth, NULL);
    vkFreeMemory(dev, depth_mem, NULL);
    vkDestroySwapchainKHR(dev, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
}

static void create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
    extent = caps.currentExtent;
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount)
        want = caps.maxImageCount;
    if (want > 8)
        want = 8;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = want,
        .imageFormat = format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain));
    nimages = 8;
    CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &nimages, images));

    VkImageCreateInfo di = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = depth_format, .extent = { extent.width, extent.height, 1 }, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT };
    CHECK(vkCreateImage(dev, &di, NULL, &depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, depth, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = memory_type(req.memoryTypeBits, 0) };
    CHECK(vkAllocateMemory(dev, &ai, NULL, &depth_mem));
    CHECK(vkBindImageMemory(dev, depth, depth_mem, 0));
    VkImageViewCreateInfo dv = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = depth,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = depth_format,
                                 .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(dev, &dv, NULL, &depth_view));

    for (uint32_t i = 0; i < nimages; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                     .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &vi, NULL, &views[i]));
        VkImageView att[2] = { views[i], depth_view };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
                                       .attachmentCount = 2, .pAttachments = att, .width = extent.width,
                                       .height = extent.height, .layers = 1 };
        CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fbs[i]));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHECK(vkCreateSemaphore(dev, &si, NULL, &rendered[i]));
    }
}

static void setup(fct_window *win, bool info)
{
    const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_SIEOS_FACET_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vklogo",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
    VkResult r0 = vkCreateInstance(&ii, NULL, &inst);
    if (r0 != VK_SUCCESS) {
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
        vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
        fprintf(stderr, "vklogo: no Vulkan instance with %s (error %d); the instance extensions are:", exts[1], r0);
        for (uint32_t i = 0; i < ne; i++)
            fprintf(stderr, " %s", ep[i].extensionName);
        fprintf(stderr, "\n");
        exit(1);
    }

    PFN_vkCreateFacetSurfaceSIEOS create_surface =
        (PFN_vkCreateFacetSurfaceSIEOS)vkGetInstanceProcAddr(inst, "vkCreateFacetSurfaceSIEOS");
    if (!create_surface) {
        fprintf(stderr, "vklogo: no VK_SIEOS_facet_surface\n");
        exit(1);
    }
    VkFacetSurfaceCreateInfoSIEOS sci = { .sType = VK_STRUCTURE_TYPE_FACET_SURFACE_CREATE_INFO_SIEOS, .window = win };
    CHECK(create_surface(inst, &sci, NULL, &surface));

    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
        fprintf(stderr, "vklogo: no Vulkan device\n");
        exit(1);
    }
    uint32_t nq = 8;
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qf == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok)
            qf = i;
    }
    if (qf == UINT32_MAX) {
        fprintf(stderr, "vklogo: no queue presents to the window\n");
        exit(1);
    }

    uint32_t nf = 8;
    VkSurfaceFormatKHR sf[8];
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, sf));
    format = sf[0].format;
    if (info) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        printf("device: %s, Vulkan %u.%u.%u\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        printf("surface formats:");
        for (uint32_t i = 0; i < nf; i++)
            printf(" %d", sf[i].format);
        uint32_t nm = 8;
        VkPresentModeKHR pm[8];
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, pm);
        printf("\npresent modes:");
        for (uint32_t i = 0; i < nm; i++)
            printf(" %d", pm[i]);
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
        printf("\nsurface: %ux%u, %u to %u images\n", caps.currentExtent.width, caps.currentExtent.height,
               caps.minImageCount, caps.maxImageCount);
        fflush(stdout);
    }

    float prio = 1;
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    CHECK(vkCreateDevice(pd, &di, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &queue);

    VkAttachmentDescription att[2] = {
        { .format = format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        { .format = depth_format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                 .pColorAttachments = &cref, .pDepthStencilAttachment = &dref };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                   .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub,
                                   .dependencyCount = 1, .pDependencies = &dep };
    CHECK(vkCreateRenderPass(dev, &rpi, NULL, &pass));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 * sizeof(float) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
    VkShaderModule vs = shader(logo_vert_spv, sizeof(logo_vert_spv)), fs = shader(logo_frag_spv, sizeof(logo_frag_spv));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription bind = { 0, sizeof(struct vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 6 * sizeof(float) },
    };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                 .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
                                                 .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                 .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                             .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsi,
        .layout = layout, .renderPass = pass,
    };
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);

    /* the mesh, in host-visible memory */
    VkDeviceSize size = nmesh * sizeof(struct vertex);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bi, NULL, &vbuf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, vbuf, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(dev, &ai, NULL, &vmem));
    CHECK(vkBindBufferMemory(dev, vbuf, vmem, 0));
    void *p;
    CHECK(vkMapMemory(dev, vmem, 0, size, 0, &p));
    memcpy(p, mesh, size);
    vkUnmapMemory(dev, vmem);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES };
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs));
    for (int i = 0; i < FRAMES; i++) {
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        CHECK(vkCreateSemaphore(dev, &si, NULL, &acquired[i]));
        CHECK(vkCreateFence(dev, &fi, NULL, &fences[i]));
    }
    create_swapchain();
}

/* One frame; false when the swapchain must be made again (the window's size changed). */
static bool draw(int frame, float angle)
{
    vkWaitForFences(dev, 1, &fences[frame], VK_TRUE, UINT64_MAX);
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired[frame], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
        return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "vklogo: vkAcquireNextImageKHR failed: %d\n", r);
        exit(1);
    }
    vkResetFences(dev, 1, &fences[frame]);

    /* the corner (1, 1, 1) towards the viewer (as the screen saver turns it), then the turn */
    float orient_y[16], orient_x[16], spin[16], model[16], proj[16], view[16], mvp[32];
    mat_rot(orient_y, -(float)M_PI / 4, 0, 1, 0);
    mat_rot(orient_x, asinf(sqrtf(1.0f / 3)), 1, 0, 0);
    mat_rot(spin, angle, 0, 1, 0);
    mat_mul(model, orient_x, orient_y);
    mat_mul(model, spin, model);
    mat_perspective(proj, 0.75f, extent.height ? (float)extent.width / extent.height : 1, 0.5f, 10);
    memcpy(view, (float[16]){ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3.3f, 1 }, sizeof(view));
    mat_mul(mvp, view, model);
    mat_mul(mvp, proj, mvp);
    memcpy(mvp + 16, model, sizeof(model));

    VkCommandBuffer cb = cbs[frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cb, &bbi));
    VkClearValue clear[2] = { { .color = { { 0.10f, 0.11f, 0.14f, 1 } } }, { .depthStencil = { 1, 0 } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
                                  .framebuffer = fbs[index], .renderArea = { { 0, 0 }, extent },
                                  .clearValueCount = 2, .pClearValues = clear };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, extent };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);
    vkCmdDraw(cb, nmesh, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    CHECK(vkEndCommandBuffer(cb));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                        .pWaitSemaphores = &acquired[frame], .pWaitDstStageMask = &wait, .commandBufferCount = 1,
                        .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[index] };
    CHECK(vkQueueSubmit(queue, 1, &si, fences[frame]));
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &rendered[index], .swapchainCount = 1, .pSwapchains = &swapchain,
                            .pImageIndices = &index };
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        return false;
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vklogo: vkQueuePresentKHR failed: %d\n", r);
        exit(1);
    }
    return true;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    bool info = false;
    long max_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info"))
            info = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: vklogo [--info] [--frames N]\n");
            return 2;
        }
    }
    fct_display *d = fct_open();
    if (!d) {
        fprintf(stderr, "vklogo: no Facet desktop (run it on the desktop)\n");
        return 1;
    }
    struct fct_window_attr a = { .title = "vklogo", .x = FCT_POS_AUTO, .y = FCT_POS_AUTO, .w = 440, .h = 440 };
    fct_window *win = fct_window_create(d, &a);
    if (!win) {
        fprintf(stderr, "vklogo: cannot open a window\n");
        return 1;
    }
    build_logo();
    setup(win, info);

    bool paused = false;
    float angle = 0;
    int frame = 0;
    long frames = 0, total = 0;
    double t0 = now(), last = t0, start = t0;
    for (;;) {
        struct fct_event ev;
        int r;
        while ((r = fct_next_event(d, &ev, 0)) > 0) {
            if (ev.type == FCT_CLOSE)
                goto done;
            if (ev.type == FCT_KEY && ev.key.value == 1 && ev.key.ascii == 27)
                goto done;
            if (ev.type == FCT_KEY && ev.key.value == 1 && ev.key.ascii == ' ')
                paused = !paused;
        }
        if (r < 0)
            break;
        double t = now();
        if (!paused)
            angle += (float)(t - last) * 0.8f;
        last = t;
        if (!draw(frame, angle)) {
            destroy_swapchain();
            create_swapchain();
        }
        frame = (frame + 1) % FRAMES;
        frames++;
        total++;
        if (t - t0 >= 1) {
            char title[64];
            snprintf(title, sizeof(title), "vklogo - Vulkan - %.0f frames/s", frames / (t - t0));
            fct_window_set_title(win, title);
            frames = 0;
            t0 = t;
        }
        if (max_frames && total >= max_frames) {
            printf("vklogo: %ld frames in %.2f s: %.1f frames/s\n", total, now() - start, total / (now() - start));
            break;
        }
    }
done:
    vkDeviceWaitIdle(dev);
    destroy_swapchain();
    vkDestroySurfaceKHR(inst, surface, NULL);
    fct_window_destroy(win);
    fct_disconnect(d);
    return 0;
}
