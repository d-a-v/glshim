#include <execinfo.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "glx.h"

#include "../gl/text.h"

#include <EGL/egl.h>

bool eglInitialized = false;
EGLDisplay eglDisplay;
EGLSurface eglSurface;
EGLConfig eglConfigs[1];

int8_t CheckEGLErrors() {
    EGLenum error;
    char *errortext;

    error = eglGetError();

    if (error != EGL_SUCCESS && error != 0) {
        switch (error) {
            case EGL_NOT_INITIALIZED:     errortext = "EGL_NOT_INITIALIZED"; break;
            case EGL_BAD_ACCESS:          errortext = "EGL_BAD_ACCESS"; break;
            case EGL_BAD_ALLOC:           errortext = "EGL_BAD_ALLOC"; break;
            case EGL_BAD_ATTRIBUTE:       errortext = "EGL_BAD_ATTRIBUTE"; break;
            case EGL_BAD_CONTEXT:         errortext = "EGL_BAD_CONTEXT"; break;
            case EGL_BAD_CONFIG:          errortext = "EGL_BAD_CONFIG"; break;
            case EGL_BAD_CURRENT_SURFACE: errortext = "EGL_BAD_CURRENT_SURFACE"; break;
            case EGL_BAD_DISPLAY:         errortext = "EGL_BAD_DISPLAY"; break;
            case EGL_BAD_SURFACE:         errortext = "EGL_BAD_SURFACE"; break;
            case EGL_BAD_MATCH:           errortext = "EGL_BAD_MATCH"; break;
            case EGL_BAD_PARAMETER:       errortext = "EGL_BAD_PARAMETER"; break;
            case EGL_BAD_NATIVE_PIXMAP:   errortext = "EGL_BAD_NATIVE_PIXMAP"; break;
            case EGL_BAD_NATIVE_WINDOW:   errortext = "EGL_BAD_NATIVE_WINDOW"; break;
            default:                      errortext = "unknown"; break;
        }

        printf("ERROR: EGL Error detected: %s (0x%X)\n", errortext, error);
        return 1;
    }

    return 0;
}

static int get_config_default(int attribute, int *value) {
    switch (attribute) {
        case GLX_USE_GL:
        case GLX_RGBA:
        case GLX_DOUBLEBUFFER:
            *value = 1;
            break;
        case GLX_STEREO:
            *value = 0;
            break;
        case GLX_AUX_BUFFERS:
            *value = 0;
            break;
        case GLX_RED_SIZE:
            *value = 5;
            break;
        case GLX_GREEN_SIZE:
            *value = 6;
            break;
        case GLX_BLUE_SIZE:
            *value = 5;
            break;
        case GLX_ALPHA_SIZE:
            *value = 8;
            break;
        case GLX_DEPTH_SIZE:
            *value = 16;
            break;
        case GLX_STENCIL_SIZE:
        case GLX_ACCUM_RED_SIZE:
        case GLX_ACCUM_GREEN_SIZE:
        case GLX_ACCUM_BLUE_SIZE:
        case GLX_ACCUM_ALPHA_SIZE:
            *value = 0;
            break;
        case GLX_RENDER_TYPE:
            *value = GLX_RGBA_BIT | GLX_COLOR_INDEX_BIT;
            break;
        case GLX_VISUAL_ID:
            *value = 1;
            break;
        case GLX_FBCONFIG_ID:
            *value = 1;
            break;
        case GLX_DRAWABLE_TYPE:
            *value = GLX_WINDOW_BIT;
            break;
        case 2: // apparently this is bpp
            *value = 16;
            return 0;
        default:
            printf("libGL: unknown attrib %i\n", attribute);
            *value = 0;
            return 1;
    }
    return 0;
}

// hmm...
static EGLContext eglContext;
static GLXContext glxContext;
static Display *g_display;

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif
static bool g_showfps = false;
static bool g_usefb = false;
static bool g_vsync = false;
static bool g_xrefresh = false;
static bool g_stacktrace = false;
static bool g_bcm_active = false;
#ifndef BCMHOST
static bool g_bcmhost = false;
#else
static bool g_bcmhost = true;
#endif

static int fbdev = -1;
static int swap_interval = 1;

static void init_display(Display *display) {
    if (! g_display) {
        g_display = XOpenDisplay(NULL);
    }
    if (g_usefb) {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    } else {
        eglDisplay = eglGetDisplay(g_display);
    }
}

static void init_vsync() {
    fbdev = open("/dev/fb0", O_RDONLY);
    if (fbdev < 0) {
        fprintf(stderr, "Could not open /dev/fb0 for vsync.\n");
    }
}

static void xrefresh() {
    system("xrefresh");
}

static void signal_handler(int sig) {
    if (g_xrefresh)
        xrefresh();

#ifdef BCMHOST
    if (g_bcm_active) {
        g_bcm_active = false;
        bcm_host_deinit();
    }
#endif

    if (g_stacktrace) {
        switch (sig) {
            case SIGBUS:
            case SIGFPE:
            case SIGILL:
            case SIGSEGV: {
                void *array[10];
                size_t size = backtrace(array, 10);
                if (! size) {
                    printf("No stacktrace. Compile with -funwind-tables.\n");
                } else {
                    printf("Stacktrace: %lu\n", size);
                    backtrace_symbols_fd(array, size, 2);
                }
                break;
            }
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void scan_env() {
    static bool first = true;
    if (! first)
        return;

    first = false;
    printf("libGL: built on %s %s\n", __DATE__, __TIME__);
    #define env(name, global, message)                    \
        char *env_##name = getenv(#name);                 \
        if (env_##name && strcmp(env_##name, "1") == 0) { \
            printf("libGL: " message "\n");               \
            global = true;                                \
        }

    env(LIBGL_XREFRESH, g_xrefresh, "xrefresh will be called on cleanup");
    env(LIBGL_STACKTRACE, g_stacktrace, "stacktrace will be printed on crash");
    if (g_xrefresh || g_stacktrace || g_bcmhost) {
        // TODO: a bit gross. Maybe look at this: http://stackoverflow.com/a/13290134/293352
        signal(SIGBUS, signal_handler);
        signal(SIGFPE, signal_handler);
        signal(SIGILL, signal_handler);
        signal(SIGSEGV, signal_handler);
        if (g_xrefresh || g_bcmhost) {
            signal(SIGINT, signal_handler);
            signal(SIGQUIT, signal_handler);
            signal(SIGTERM, signal_handler);
        }
        if (g_xrefresh)
            atexit(xrefresh);
#ifdef BCMHOST
            atexit(bcm_host_deinit);
#endif
    }
    env(LIBGL_FB, g_usefb, "framebuffer output enabled");
    env(LIBGL_FPS, g_showfps, "fps counter enabled");
    env(LIBGL_VSYNC, g_vsync, "vsync enabled");
    if (g_vsync) {
        init_vsync();
    }
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) {
    scan_env();
    PROXY_GLES(glXCreateContext);
    EGLint configAttribs[] = {
#ifdef PANDORA
        EGL_RED_SIZE, 5,
        EGL_GREEN_SIZE, 6,
        EGL_BLUE_SIZE, 5,
#endif
        EGL_DEPTH_SIZE, 16,
#ifdef USE_ES2
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
#else
        EGL_BUFFER_SIZE, 16,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
#endif
        EGL_NONE
    };

#ifdef USE_ES2
    EGLint attrib_list[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
#else
    EGLint *attrib_list = NULL;
#endif


#ifdef BCMHOST
    if (! g_bcm_active) {
        g_bcm_active = true;
        bcm_host_init();
    }
#endif

    GLXContext fake = malloc(sizeof(struct __GLXContextRec));
    if (eglDisplay != NULL) {
        eglMakeCurrent(eglDisplay, NULL, NULL, EGL_NO_CONTEXT);
        if (eglContext != NULL) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = NULL;
        }
        if (eglSurface != NULL) {
            eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = NULL;
        }
    }

    // make an egl context here...
    EGLBoolean result;
    if (eglDisplay == NULL || eglDisplay == EGL_NO_DISPLAY) {
        init_display(dpy);
        if (eglDisplay == EGL_NO_DISPLAY) {
            printf("Unable to create EGL display.\n");
            return fake;
        }
    }

    // first time?
    if (eglInitialized == false) {
        eglBindAPI(EGL_OPENGL_ES_API);
        result = eglInitialize(eglDisplay, NULL, NULL);
        if (result != EGL_TRUE) {
            printf("Unable to initialize EGL display.\n");
            return fake;
        }
        eglInitialized = true;
    }

    int configsFound;
    result = eglChooseConfig(eglDisplay, configAttribs, eglConfigs, 1, &configsFound);
    CheckEGLErrors();
    if (result != EGL_TRUE || configsFound == 0) {
        printf("No EGL configs found.\n");
        return fake;
    }
    eglContext = eglCreateContext(eglDisplay, eglConfigs[0], EGL_NO_CONTEXT, attrib_list);
    CheckEGLErrors();

    // need to return a glx context pointing at it
    fake->display = g_display;
    fake->direct = true;
    fake->xid = 1;
    return fake;
}

GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list) {
    PROXY_GLES(glXCreateContextAttribsARB);
    return glXCreateContext(dpy, NULL, NULL, direct);
}

void glXDestroyContext(Display *dpy, GLXContext ctx) {
    PROXY_GLES(glXDestroyContext);
    if (eglContext) {
        EGLBoolean result = eglDestroyContext(eglDisplay, eglContext);
        if (eglSurface != NULL) {
            eglDestroySurface(eglDisplay, eglSurface);
        }

        if (result != EGL_TRUE) {
            printf("Failed to destroy EGL context.\n");
        }
        if (fbdev >= 0) {
            close(fbdev);
            fbdev = -1;
        }
    }
    return;
}

Display *glXGetCurrentDisplay() {
    PROXY_GLES(glXGetCurrentDisplay);
    if (g_display && eglContext) {
        return g_display;
    }
    return NULL;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList) {
    PROXY_GLES(glXChooseVisual);

    // apparently can't trust the Display I'm passed?
    if (g_display == NULL) {
        g_display = XOpenDisplay(NULL);
    }
    int depth = DefaultDepth(g_display, screen);
    XVisualInfo *visual = (XVisualInfo *)malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(g_display, screen, depth, TrueColor, visual);
    return visual;
}

/*
EGL_BAD_MATCH is generated if draw or read are not compatible with context
or if context is set to EGL_NO_CONTEXT and draw or read are not set to
EGL_NO_SURFACE, or if draw or read are set to EGL_NO_SURFACE and context is
not set to EGL_NO_CONTEXT.
*/

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
    PROXY_GLES(glXMakeCurrent);
    if (eglDisplay != NULL) {
        eglMakeCurrent(eglDisplay, NULL, NULL, EGL_NO_CONTEXT);
        if (eglSurface != NULL) {
            eglDestroySurface(eglDisplay, eglSurface);
        }
    }
    // call with NULL to just destroy old stuff.
    if (! ctx) {
        return true;
    }
    if (eglDisplay == NULL) {
        init_display(dpy);
    }

    if (g_usefb)
        drawable = 0;
    eglSurface = eglCreateWindowSurface(eglDisplay, eglConfigs[0], drawable, NULL);
    CheckEGLErrors();

    EGLBoolean result = eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
    CheckEGLErrors();
    if (result) {
        return true;
    }
    return false;
}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, int read, GLXContext ctx) {
    PROXY_GLES(glXMakeContextCurrent);
    return glXMakeCurrent(dpy, draw, ctx);
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    static int frames = 0;
    if (g_showfps) {
        // framerate counter
        static float avg, fps = 0;
        static int frame1, last_frame, frame, now, current_frames;
        struct timeval out;
        gettimeofday(&out, NULL);
        now = out.tv_sec;
        frame++;
        current_frames++;

        if (frame == 1) {
            frame1 = now;
        } else if (frame1 < now) {
            if (last_frame < now) {
                float change = current_frames / (float)(now - last_frame);
                float weight = 0.7;
                if (! fps) {
                    fps = change;
                } else {
                    fps = (1 - weight) * fps + weight * change;
                }
                current_frames = 0;

                avg = frame / (float)(now - frame1);
                printf("libGL fps: %.2f, avg: %.2f\n", fps, avg);
            }
        }

        last_frame = now;

        if (fps > 0) {
            char buf[17] = {0};
            snprintf(buf, 16, "%.2f fps\n", fps);
            text_draw(4, 17, buf);
        }
    }

    PROXY_GLES(glXSwapBuffers);
    render_raster();
    if (g_vsync && fbdev >= 0) {
        // TODO: can I just return if I don't meet vsync over multiple frames?
        // this will just block otherwise.
        int arg = 0;
        for (int i = 0; i < swap_interval; i++) {
            ioctl(fbdev, FBIO_WAITFORVSYNC, &arg);
        }
    }
    eglSwapBuffers(eglDisplay, eglSurface);
    CheckEGLErrors();
}

int glXGetConfig(Display *display, XVisualInfo *visual, int attribute, int *value) {
    PROXY_GLES(glXGetConfig);
    return get_config_default(attribute, value);
}

const char *glXQueryExtensionsString(Display *dpy, int screen) {
    PROXY_GLES(glXQueryExtensionsString);
    const char *extensions = {
        "GLX_ARB_create_context "
        "GLX_ARB_create_context_profile "
        "GLX_EXT_create_context_es2_profile "
    };
    return extensions;
}

const char *glXQueryServerString(Display *dpy, int screen, int name) {
    PROXY_GLES(glXQueryServerString);
    return "";
}

Bool glXQueryExtension(Display *display, int *errorBase, int *eventBase) {
    PROXY_GLES(glXQueryExtension);
    if (errorBase)
        *errorBase = 0;

    if (eventBase)
        *eventBase = 0;

    return true;
}

Bool glXQueryVersion(Display *dpy, int *maj, int *min) {
    PROXY_GLES(glXQueryVersion);
    // TODO: figure out which version we want to pretend to implement
    *maj = 1;
    *min = 4;
    return true;
}

const char *glXGetClientString(Display *display, int name) {
    PROXY_GLES(glXGetClientString);
    // TODO: return actual data here
    switch (name) {
        case GLX_VENDOR: break;
        case GLX_VERSION: break;
        case GLX_EXTENSIONS: break;
    }
    return "";
}

// stubs for glfw (GLX 1.3)
GLXContext glXGetCurrentContext() {
    PROXY_GLES(glXGetCurrentContext);
    // hack to make some games start
    return glxContext ? glxContext : (void *)1;
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nelements) {
    PROXY_GLES(glXChooseFBConfig);
    *nelements = 1;
    GLXFBConfig *configs = malloc(sizeof(GLXFBConfig) * *nelements);
    return configs;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements) {
    PROXY_GLES(glXGetFBConfigs);
    *nelements = 1;
    GLXFBConfig *configs = malloc(sizeof(GLXFBConfig) * *nelements);
    return configs;
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value) {
    PROXY_GLES(glXGetFBConfigAttrib);
    return get_config_default(attribute, value);
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config) {
    PROXY_GLES(glXGetVisualFromFBConfig);
    if (g_display == NULL) {
        g_display = XOpenDisplay(NULL);
    }
    XVisualInfo *visual = (XVisualInfo *)malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(g_display, 0, 16, TrueColor, visual);
    return visual;
}

GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct) {
    PROXY_GLES(glXCreateNewContext);
    return glXCreateContext(dpy, 0, share_list, direct);
}

int glXSwapIntervalMESA(unsigned int interval) {
    printf("glXSwapInterval(%i)\n", interval);
    if (! g_vsync)
        printf("Enable LIBGL_VSYNC=1 if you want to use vsync.\n");
    swap_interval = interval;
    return 0;
}

int glXSwapIntervalSGI(int interval) {
    return glXSwapIntervalMESA(interval);
}

void glXSwapIntervalEXT(Display *display, GLXDrawable drawable, int interval) {
    glXSwapIntervalMESA(interval);
}

// misc stubs
void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask) {
    PROXY_GLES(glXCopyContext);
}

GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *visual, Pixmap pixmap) {
    PROXY_GLES(glXCreateGLXPixmap);
} // should return GLXPixmap

void glXDestroyGLXPixmap(Display *dpy, void *pixmap) {
    PROXY_GLES(glXDestroyGLXPixmap);
} // really wants a GLXpixmap

GLXDrawable glXGetCurrentDrawable() {
    PROXY_GLES(glXGetCurrentDrawable);
} // this should actually return GLXDrawable. Good luck.

Bool glXIsDirect(Display *dpy, GLXContext ctx) {
    PROXY_GLES(glXIsDirect);
    return true;
}

void glXUseXFont(Font font, int first, int count, int list) {
    PROXY_GLES(glXUseXFont);
}

void glXWaitGL() {
    PROXY_GLES(glXWaitGL);
}

void glXWaitX() {
    PROXY_GLES(glXWaitX);
}

Bool glXReleaseBuffersMESA(Display *dpy, GLXDrawable drawable) {
    PROXY_GLES(glXReleaseBuffersMESA);
}
