/*
 * glcube - OpenGL ES 2 in a Facet window through EGL (Mesa, llvmpipe): a
 * lit, rotating cube, its frame rate in the title.  Drag it to turn it;
 * Space pauses; Escape or closing the window quits.
 *
 *   glcube [--info] [--frames N]
 *     --info      print what EGL, OpenGL ES and desktop OpenGL say (vendor, renderer,
 *                 versions; desktop OpenGL on a pbuffer, through eglGetProcAddress)
 *     --frames N  quit after N frames, printing the frame rate (a test)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <facet/facet.h>

static const char *vertex_src =
    "uniform mat4 u_mvp;\n"
    "uniform mat3 u_normal;\n"
    "attribute vec3 a_pos;\n"
    "attribute vec3 a_normal;\n"
    "attribute vec3 a_color;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    vec3 n = normalize(u_normal * a_normal);\n"
    "    float light = 0.25 + 0.75 * max(dot(n, normalize(vec3(0.4, 0.6, 1.0))), 0.0);\n"
    "    v_color = a_color * light;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *fragment_src =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() { gl_FragColor = vec4(v_color, 1.0); }\n";

/* a cube: 6 faces x 4 vertices (position, normal, color) */
static const float faces[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
static const float colors[6][3] = { { 0.95f, 0.35f, 0.25f }, { 0.25f, 0.65f, 0.95f }, { 0.35f, 0.85f, 0.40f },
                                    { 0.95f, 0.80f, 0.25f }, { 0.75f, 0.45f, 0.95f }, { 0.30f, 0.85f, 0.85f } };

static float verts[24 * 9];
static unsigned short indices[36];

static void build_cube(void)
{
    for (int f = 0; f < 6; f++) {
        const float *n = faces[f];
        /* two axes across the face */
        float u[3] = { n[1] != 0 ? 1 : 0, n[1] != 0 ? 0 : 1, 0 };
        if (n[2] == 0 && n[1] == 0) {
            u[0] = 0;
            u[1] = 1;
        }
        float v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
        static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int c = 0; c < 4; c++) {
            float *p = verts + (f * 4 + c) * 9;
            for (int k = 0; k < 3; k++) {
                p[k] = n[k] + corner[c][0] * u[k] + corner[c][1] * v[k];
                p[3 + k] = n[k];
                p[6 + k] = colors[f][k];
            }
        }
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++)
            indices[f * 6 + k] = f * 4 + tri[k];
    }
}

/* 4x4 matrices, column-major as OpenGL takes them */
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

static void mat_rotate(float *m, float angle, float x, float y, float z)
{
    float c = cosf(angle), s = sinf(angle), l = sqrtf(x * x + y * y + z * z);
    x /= l, y /= l, z /= l;
    float r[16] = { x * x * (1 - c) + c,     y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
                    x * y * (1 - c) - z * s, y * y * (1 - c) + c,     y * z * (1 - c) + x * s, 0,
                    x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c,     0,
                    0, 0, 0, 1 };
    memcpy(m, r, sizeof(r));
}

static void mat_perspective(float *m, float fovy, float aspect, float near, float far)
{
    float f = 1.0f / tanf(fovy / 2);
    float r[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) / (near - far), -1,
                    0, 0, 2 * far * near / (near - far), 0 };
    memcpy(m, r, sizeof(r));
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "glcube: shader: %s\n", log);
        exit(1);
    }
    return s;
}

/* Desktop OpenGL through EGL: a context of EGL_OPENGL_API on a small pbuffer, its version. */
static void desktop_gl_info(EGLDisplay dpy)
{
    static const EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                      EGL_NONE };
    static const EGLint pb_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLConfig config;
    EGLint n = 0;
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(dpy, attribs, &config, 1, &n) || n < 1) {
        printf("desktop OpenGL: not available\n");
        eglBindAPI(EGL_OPENGL_ES_API);
        return;
    }
    EGLSurface pb = eglCreatePbufferSurface(dpy, config, pb_attribs);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, NULL);
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLContext cur = eglGetCurrentContext();
    if (pb != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, pb, pb, ctx)) {
        const GLubyte *(*get_string)(GLenum) = (const GLubyte *(*)(GLenum))eglGetProcAddress("glGetString");
        printf("desktop OpenGL: %s, %s\n", get_string ? (const char *)get_string(GL_VERSION) : "?",
               get_string ? (const char *)get_string(GL_SHADING_LANGUAGE_VERSION) : "?");
    } else {
        printf("desktop OpenGL: no context (error 0x%x)\n", eglGetError());
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    eglMakeCurrent(dpy, draw, read, cur);
    if (ctx != EGL_NO_CONTEXT)
        eglDestroyContext(dpy, ctx);
    if (pb != EGL_NO_SURFACE)
        eglDestroySurface(dpy, pb);
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
            fprintf(stderr, "usage: glcube [--info] [--frames N]\n");
            return 2;
        }
    }

    fct_display *d = fct_open();
    if (!d) {
        fprintf(stderr, "glcube: no Facet desktop (run it on the desktop)\n");
        return 1;
    }
    struct fct_window_attr a = { .title = "glcube", .x = FCT_POS_AUTO, .y = FCT_POS_AUTO, .w = 480, .h = 400,
                                 .flags = FCT_WIN_POINTER };
    fct_window *win = fct_window_create(d, &a);
    if (!win) {
        fprintf(stderr, "glcube: cannot open a window\n");
        return 1;
    }

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "glcube: EGL: cannot initialise (error 0x%x)\n", eglGetError());
        return 1;
    }
    static const EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                      EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attribs, &config, 1, &n) || n < 1) {
        fprintf(stderr, "glcube: EGL: no config for a window\n");
        return 1;
    }
    EGLSurface surf = eglCreateWindowSurface(dpy, config, win, NULL);
    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "glcube: EGL: no surface or context (error 0x%x)\n", eglGetError());
        return 1;
    }
    if (info) {
        printf("EGL %d.%d: %s, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
        printf("EGL client APIs: %s\n", eglQueryString(dpy, EGL_CLIENT_APIS));
        printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\nGL_SHADING_LANGUAGE_VERSION: %s\n",
               (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
               (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
        desktop_gl_info(dpy);
        fflush(stdout);
    }

    build_cube();
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vertex_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fragment_src));
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_normal");
    glBindAttribLocation(prog, 2, "a_color");
    glLinkProgram(prog);
    glUseProgram(prog);
    GLint u_mvp = glGetUniformLocation(prog, "u_mvp"), u_normal = glGetUniformLocation(prog, "u_normal");
    GLuint vbo, ibo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    for (int k = 0; k < 3; k++) {
        glEnableVertexAttribArray(k);
        glVertexAttribPointer(k, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(k * 3 * sizeof(float)));
    }
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    float ax = 0.5f, ay = 0.7f;
    bool paused = false, dragging = false;
    int lastx = 0, lasty = 0;
    long frames = 0, total = 0;
    double t0 = now(), last = t0, start = t0;
    for (;;) {
        struct fct_event ev;
        int r;
        while ((r = fct_next_event(d, &ev, 0)) > 0) {
            if (ev.type == FCT_CLOSE)
                goto done;
            if (ev.type == FCT_KEY && ev.key.value == 1) {
                if (ev.key.ascii == 27)
                    goto done;
                if (ev.key.ascii == ' ')
                    paused = !paused;
            } else if (ev.type == FCT_MOUSE) {
                if (ev.kind == FCT_MOUSE_DOWN && (ev.buttons & 1)) {
                    dragging = true;
                    lastx = ev.x, lasty = ev.y;
                } else if (ev.kind == FCT_MOUSE_UP) {
                    dragging = false;
                } else if (ev.kind == FCT_MOUSE_MOVE && dragging) {
                    ay += (ev.x - lastx) * 0.01f;
                    ax += (ev.y - lasty) * 0.01f;
                    lastx = ev.x, lasty = ev.y;
                }
            }
        }
        if (r < 0)
            break;                               /* the desktop is gone */

        double t = now();
        if (!paused && !dragging) {
            ay += (float)(t - last) * 0.9f;
            ax += (float)(t - last) * 0.4f;
        }
        last = t;

        int w, h;
        fct_window_size(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.14f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        float proj[16], rx[16], ry[16], model[16], mvp[16];
        mat_perspective(proj, 0.8f, h ? (float)w / h : 1, 1, 20);
        mat_rotate(rx, ax, 1, 0, 0);
        mat_rotate(ry, ay, 0, 1, 0);
        mat_mul(model, ry, rx);
        model[14] = -6;                          /* pushed away from the eye */
        mat_mul(mvp, proj, model);
        float normal[9] = { model[0], model[1], model[2], model[4], model[5], model[6], model[8], model[9], model[10] };
        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix3fv(u_normal, 1, GL_FALSE, normal);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
        eglSwapBuffers(dpy, surf);

        frames++;
        total++;
        if (t - t0 >= 1) {
            char title[64];
            snprintf(title, sizeof(title), "glcube - %.0f frames/s", frames / (t - t0));
            fct_window_set_title(win, title);
            frames = 0;
            t0 = t;
        }
        if (max_frames && total >= max_frames) {
            printf("glcube: %ld frames in %.2f s: %.1f frames/s\n", total, now() - start, total / (now() - start));
            break;
        }
    }
done:
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    fct_window_destroy(win);
    fct_disconnect(d);
    return 0;
}
