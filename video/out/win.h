#ifndef MP_VO_WIN_H_
#define MP_VO_WIN_H_

#include <stdbool.h>
#include <stdint.h>

struct vo_win;
struct vo_win_size;
struct vo;

struct vo_win_driver {
    const char *name; // used e.g. for vo_opengl backend suboption

    /* Called once in initialization. Doesn't create a visible window yet, but
     * should succeed if showing a window is possible. Determining this is
     * important for autoprobing.
     * On error, uninit will not be called.
     *  returns: < 0 on error, == 0 on success.
     */
    int (*preinit)(struct vo_win *win);

    /* Called before destruction. Not called if preinit failed.
     */
    void (*uninit)(struct vo_win *win);

    /* Show a window with the given size. If a window already exists, resize
     * it, unless the size is the same as the previous window size.
     *  w, h: video size
     *  flags: VOFLAG_* bits
     *  returns: < 0 on error, == 0 on success
     */
    int (*reconfig)(struct vo_win *win, int w, int h, int flags);

    /* Run a VOCTRL_* command. They're all optional.
     *  request: VOCTRL_*
     *  arg: depends on the request
     *  returns: true/false/VO_* error codes (VO_ERROR etc.)
     */
    int (*control)(struct vo_win *win, int request, void *arg);

    /* Run the event loop.
     */
    int (*wait_events)(struct vo_win *win, int64_t wait_until_us);

    /* For threaded backends only. This is the only driver function that
     * needs to be thread-safe. This callback should wake up the GUI event
     * loop, and cause it to call vo_win_backend_dispatch(). That function
     * call should be in the context of the GUI thread, and will call further
     * driver functions.
     * Note that wakeup can return immediately. It doesn't/mustn't have to
     * wait until the GUI thread is done.
     * TODO: clarify how to avoid missed wakeups
     */
    void (*wakeup)(struct vo_win *win);

    /* Optional OpenGL support. Defined in gl_common.h. All of its functions
     * use the same vo_win pointer as context. */
    const struct vo_win_gl_driver *gl;
};

struct vo_win {
    const struct vo_win_driver *driver;
    struct mp_log *log;
    struct mpv_global *global;
    struct input_ctx *input_ctx;
    struct mp_vo_opts *opts;
    struct vo_win_internal *in;

    bool probing;

    // For VOs which use poll() to wait for new events.
    int event_fd;
    int wakeup_pipe[2];

    void *priv; // for free use by vo_win_driver
};

/* Notify the VO about events. Takes care of waking up the VO, if needed.
 *  events: VO_EVENT_* bits
 */
void vo_win_signal_event(struct vo_win *win, int events);

void vo_win_set_size(struct vo_win *win, struct vo_win_size *sz);

/* For threaded backends only. Call this to process requests from the VO
 * thread, usually in reaction to vo_win_driver.wakeup.
 */
//void vo_win_dispatch(struct vo_win *win);

/* Used by the VO to set a wakeup callback. w(c) will be called if the backend
 * has new events for the VO (i.e. vo_win_backend_signal_event() was called).
 */
//void vo_win_set_wakeup_cb(struct vo_win *win, void (*w)(void *), void *c);

struct mpv_global;
struct mp_log;
struct input_ctx;
struct vo_win *vo_win_create(struct mpv_global *global, struct mp_log *log,
                             struct input_ctx *input_ctx, int flags,
                             const struct vo_win_driver *driver);

struct vo_win *vo_win_create_vo(struct vo *vo, int flags,
                                const struct vo_win_driver *driver);
struct vo_win *vo_win_create_win(struct vo_win *win, int flags,
                                 const struct vo_win_driver *driver);

void vo_win_destroy(struct vo_win *win);

// Call win->driver->reconfig().
int vo_win_reconfig(struct vo_win *win, int w, int h, int flags);

// Call vo_win_reconfig(), and take care of copying the new params to the VO.
int vo_win_reconfig_vo(struct vo_win *win, struct vo *vo, int flags);

// request: VOCTRL_*
// returns: VO_TRUE/VO_FALSE/VO_NOTIMPL
int vo_win_control(struct vo_win *win, int request, void *data);

// Return the window size. If none is available, return 0x0.
void vo_win_get_size(struct vo_win *win, struct vo_win_size *sz);
void vo_win_get_size_vo(struct vo_win *win, struct vo *vo);

// Wait, and return until either new events are available, or the given time
// is reached (mp_time_us()>=until_time_us), or vo_win_wakeup() is called.
//  returns: VO_EVENT_* bits
int vo_win_wait_events(struct vo_win *win, int64_t until_time_us);

// Unblock an ongoing vo_win_wait_events() call.
void vo_win_wakeup(struct vo_win *win);

void vo_win_wait_event_fd(struct vo_win *win, int64_t until_time);
void vo_win_wakeup_event_fd(struct vo_win *win);

#endif
