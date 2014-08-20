/*
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <X11/Xlib.h>
#include <GL/glx.h>

#define MP_GET_GLX_WORKAROUNDS
#include "gl_header_fixes.h"

#include "x11_common.h"
#include "gl_common.h"
#include "win.h"

struct glx_context {
    struct vo_win *x11;
    XVisualInfo *vinfo;
    GLXContext context;
    GLXFBConfig fbc;
    Display *display;
    int screen;
    Window window;
    GL *gl;
    int depth[3];
};

static bool create_context_x11_old(struct vo_win *win)
{
    struct glx_context *priv = win->priv;
    Display *display = priv->display;
    GL *gl = priv->gl;

    if (priv->context)
        return true;

    if (!priv->vinfo) {
        MP_FATAL(win, "Can't create a legacy GLX context without X visual\n");
        return false;
    }

    GLXContext new_context = glXCreateContext(display, priv->vinfo, NULL,
                                              True);
    if (!new_context) {
        MP_FATAL(win, "Could not create GLX context!\n");
        return false;
    }

    if (!glXMakeCurrent(display, priv->window, new_context)) {
        MP_FATAL(win, "Could not set GLX context!\n");
        glXDestroyContext(display, new_context);
        return false;
    }

    const char *glxstr = glXQueryExtensionsString(display, priv->screen);

    mpgl_load_functions(gl, (void *)glXGetProcAddressARB, glxstr, win->log);

    priv->context = new_context;

    if (!glXIsDirect(display, new_context))
        gl->mpgl_caps &= ~MPGL_CAP_NO_SW;

    return true;
}

typedef GLXContext (*glXCreateContextAttribsARBProc)
    (Display*, GLXFBConfig, GLXContext, Bool, const int*);

static bool create_context_x11_gl3(struct vo_win *win, int gl_version, bool debug)
{
    struct glx_context *priv = win->priv;
    Display *display = priv->display;

    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        (glXCreateContextAttribsARBProc)
            glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");

    const char *glxstr = glXQueryExtensionsString(display, priv->screen);
    bool have_ctx_ext = glxstr && !!strstr(glxstr, "GLX_ARB_create_context");

    if (!(have_ctx_ext && glXCreateContextAttribsARB)) {
        return false;
    }

    int context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, MPGL_VER_GET_MAJOR(gl_version),
        GLX_CONTEXT_MINOR_VERSION_ARB, MPGL_VER_GET_MINOR(gl_version),
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        GLX_CONTEXT_FLAGS_ARB, debug ? GLX_CONTEXT_DEBUG_BIT_ARB : 0,
        None
    };
    GLXContext context = glXCreateContextAttribsARB(display, priv->fbc, 0,
                                                    True, context_attribs);
    if (!context) {
        MP_ERR(win, "Could not create GL3 context. Retrying with legacy context.\n");
        return false;
    }

    // set context
    if (!glXMakeCurrent(display, priv->window, context)) {
        MP_FATAL(win, "Could not set GLX context!\n");
        glXDestroyContext(display, context);
        return false;
    }

    priv->context = context;

    mpgl_load_functions(priv->gl, (void *)glXGetProcAddress, glxstr, win->log);

    if (!glXIsDirect(display, context))
        priv->gl->mpgl_caps &= ~MPGL_CAP_NO_SW;

    return true;
}

// The GL3/FBC initialization code roughly follows/copies from:
//  http://www.opengl.org/wiki/Tutorial:_OpenGL_3.0_Context_Creation_(GLX)
// but also uses some of the old code.

static GLXFBConfig select_fb_config(struct vo_win *win, const int *attribs,
                                    int flags)
{
    struct glx_context *priv = win->priv;
    int fbcount;
    GLXFBConfig *fbc = glXChooseFBConfig(priv->display, priv->screen,
                                         attribs, &fbcount);
    if (!fbc)
        return NULL;

    // The list in fbc is sorted (so that the first element is the best).
    GLXFBConfig fbconfig = fbcount > 0 ? fbc[0] : NULL;

    if (flags & VOFLAG_ALPHA) {
        for (int n = 0; n < fbcount; n++) {
            XVisualInfo *v = glXGetVisualFromFBConfig(priv->display, fbc[n]);
            if (!v)
                continue;
            // This is a heuristic at best. Note that normal 8 bit Visuals use
            // a depth of 24, even if the pixels are padded to 32 bit. If the
            // depth is higher than 24, the remaining bits must be alpha.
            // Note: vinfo->bits_per_rgb appears to be useless (is always 8).
            unsigned long mask = v->depth == 32 ?
                (unsigned long)-1 : (1 << (unsigned long)v->depth) - 1;
            if (mask & ~(v->red_mask | v->green_mask | v->blue_mask)) {
                fbconfig = fbc[n];
                break;
            }
        }
    }

    XFree(fbc);

    return fbconfig;
}

static void set_glx_attrib(int *attribs, int name, int value)
{
    for (int n = 0; attribs[n * 2 + 0] != None; n++) {
        if (attribs[n * 2 + 0] == name) {
            attribs[n * 2 + 1] = value;
            break;
        }
    }
}

static int create_context(struct vo_win *win, int gl_version, int flags)
{
    struct glx_context *priv = win->priv;
    Display *display = priv->display;

    int glx_major, glx_minor;

    // FBConfigs were added in GLX version 1.3.
    if (!glXQueryVersion(display, &glx_major, &glx_minor) ||
        (MPGL_VER(glx_major, glx_minor) <  MPGL_VER(1, 3)))
    {
        MP_ERR(win, "GLX version older than 1.3.\n");
        return false;
    }

    int glx_attribs[] = {
        GLX_STEREO, False,
        GLX_X_RENDERABLE, True,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 1,
        GLX_GREEN_SIZE, 1,
        GLX_BLUE_SIZE, 1,
        GLX_ALPHA_SIZE, 0,
        GLX_DOUBLEBUFFER, True,
        None
    };
    GLXFBConfig fbc = NULL;
    if (flags & VOFLAG_ALPHA) {
        set_glx_attrib(glx_attribs, GLX_ALPHA_SIZE, 1);
        fbc = select_fb_config(win, glx_attribs, flags);
        if (!fbc) {
            set_glx_attrib(glx_attribs, GLX_ALPHA_SIZE, 0);
            flags &= ~VOFLAG_ALPHA;
        }
    }
    if (flags & VOFLAG_STEREO) {
        set_glx_attrib(glx_attribs, GLX_STEREO, True);
        fbc = select_fb_config(win, glx_attribs, flags);
        if (!fbc) {
            MP_ERR(win, "Could not find a stereo visual,"
                        " 3D will probably not work!\n");
            set_glx_attrib(glx_attribs, GLX_STEREO, False);
            flags &= ~VOFLAG_STEREO;
        }
    }
    if (!fbc)
        fbc = select_fb_config(win, glx_attribs, flags);
    if (!fbc) {
        MP_ERR(win, "no GLX support present\n");
        return false;
    }

    priv->fbc = fbc;
    priv->vinfo = glXGetVisualFromFBConfig(display, fbc);
    if (priv->vinfo) {
        MP_VERBOSE(win, "GLX chose visual with ID 0x%x\n",
                   (int)priv->vinfo->visualid);
    } else {
        MP_WARN(win, "Selected GLX FB config has no associated X visual\n");
    }


    glXGetFBConfigAttrib(display, fbc, GLX_RED_SIZE, &priv->depth[0]);
    glXGetFBConfigAttrib(display, fbc, GLX_GREEN_SIZE, &priv->depth[1]);
    glXGetFBConfigAttrib(display, fbc, GLX_BLUE_SIZE, &priv->depth[2]);

    priv->window = vo_x11_create_gl_window(priv->x11, priv->vinfo, flags);

    bool success = false;
    if (gl_version >= MPGL_VER(3, 0))
        success = create_context_x11_gl3(win, gl_version, flags & VOFLAG_GL_DEBUG);
    if (!success)
        success = create_context_x11_old(win);
    return success;
}

static void swap_buffers(struct vo_win *win)
{
    struct glx_context *priv = win->priv;
    glXSwapBuffers(priv->display, priv->window);
}

static GL *get_gl(struct vo_win *win)
{
    struct glx_context *priv = win->priv;
    return priv->gl;
}

static int preinit(struct vo_win *win)
{
    struct vo_win *x11 = vo_win_create_win(win, 0, &win_driver_x11);
    if (!x11)
        return -1;
    struct vo_x11_state *x11_ctx = x11->priv;
    struct glx_context *priv = win->priv = talloc(NULL, struct glx_context);
    *priv = (struct glx_context){
        .x11 = x11,
        .gl = talloc_zero(priv, struct GL),
        .display = x11_ctx->display,
        .screen = x11_ctx->screen,
    };
    return 0;
}

static void uninit(struct vo_win *win)
{
    struct glx_context *priv = win->priv;

    if (priv->vinfo)
        XFree(priv->vinfo);
    if (priv->context) {
        glXMakeCurrent(priv->display, None, NULL);
        glXDestroyContext(priv->display, priv->context);
    }

    vo_win_destroy(priv->x11);
    talloc_free(priv);
}

static int reconfig(struct vo_win *win, int w, int h, int flags)
{
    struct glx_context *priv = win->priv;

    return vo_win_reconfig(priv->x11, w, h, flags);
}

static int control(struct vo_win *win, int request, void *arg)
{
    struct glx_context *priv = win->priv;

    if (request == VOCTRL_GET_BIT_DEPTH) {
        memcpy(arg, priv->depth, sizeof(priv->depth));
        return VO_TRUE;
    }

    return vo_win_control(priv->x11, request, arg);
}

static int wait_events(struct vo_win *win, int64_t wait_until_us)
{
    struct glx_context *priv = win->priv;

    int r = vo_win_wait_events(priv->x11, wait_until_us);
    if (r & VO_EVENT_RESIZE) {
        struct vo_win_size sz;
        vo_win_get_size(priv->x11, &sz);
        vo_win_set_size(win, &sz);
    }
    return r;
}

static void wakeup(struct vo_win *win)
{
    struct glx_context *priv = win->priv;

    vo_win_wakeup(priv->x11);
}

const struct vo_win_driver win_driver_x11_gl = {
    .name = "x11",
    .preinit = preinit,
    .uninit = uninit,
    .reconfig = reconfig,
    .control = control,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .gl = &(const struct vo_win_gl_driver){
        .create_context = create_context,
        .get_gl = get_gl,
        .swap_buffers = swap_buffers,
    },
};
