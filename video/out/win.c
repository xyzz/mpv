#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"

#include "video/mp_image.h"
#include "vo.h"
#include "win.h"

struct vo_win_internal {
    int events;
    struct vo_win_size win_size;
};

struct vo_win *vo_win_create(struct mpv_global *global, struct mp_log *log,
                             struct input_ctx *input_ctx, int flags,
                             const struct vo_win_driver *driver)
{
    struct vo_win *win = talloc_ptrtype(NULL, win);
    *win = (struct vo_win){
        .driver = driver,
        .log = mp_log_new(win, log, driver->name),
        .global = global,
        .input_ctx = input_ctx,
        .opts = &global->opts->vo,
        .in = talloc_zero(win, struct vo_win_internal),
    };
    if (driver->preinit(win) < 0) {
        talloc_free(win);
        return NULL;
    }
    return win;
}

struct vo_win *vo_win_create_vo(struct vo *vo, int flags,
                                const struct vo_win_driver *driver)
{
    return vo_win_create(vo->global, vo->log, vo->input_ctx, flags, driver);
}

void vo_win_destroy(struct vo_win *win)
{
    if (win)
        win->driver->uninit(win);
    talloc_free(win);
}

void vo_win_signal_event(struct vo_win *win, int events)
{
    win->in->events |= events;
}

void vo_win_set_size(struct vo_win *win, struct vo_win_size *sz)
{
    win->in->win_size = *sz;
}

void vo_win_get_size(struct vo_win *win, struct vo_win_size *sz)
{
    *sz = win->in->win_size;
}

void vo_win_get_size_vo(struct vo_win *win, struct vo *vo)
{
    struct vo_win_size sz;
    vo_win_get_size(win, &sz);
    vo->dwidth = sz.w;
    vo->dheight = sz.h;
    vo->monitor_par = sz.monitor_par;
}

int vo_win_reconfig(struct vo_win *win, int w, int h, int flags)
{
    return win->driver->reconfig(win, w, h, flags);
}

int vo_win_reconfig_vo(struct vo_win *win, struct vo *vo, int flags)
{
    if (!vo->params)
        return -1;
    int r = vo_win_reconfig(win, vo->params->d_w, vo->params->d_h, flags);
    if (r >= 0)
        vo_win_get_size_vo(win, vo);
    return r;
}

// request: VOCTRL_*
// returns: VO_TRUE/VO_FALSE/VO_NOTIMPL
int vo_win_control(struct vo_win *win, int request, void *data)
{
    return win->driver->control(win, request, data);
}

/* Wait, and return until either new events are available, or the given time
 * is reached (mp_time_us()>=until_time_us), or vo_win_wakeup() is called.
 *  returns: VO_EVENT_* bits
 */
int vo_win_wait_events(struct vo_win *win, int64_t until_time_us)
{
    if (win->in->events)
        until_time_us = 0;
    int r = win->driver->wait_events(win, until_time_us);
    r |= win->in->events;
    win->in->events = 0;
    return r;
}

// Unblock an ongoing vo_win_wait_events() call.
void vo_win_wakeup(struct vo_win *win)
{
    win->driver->wakeup(win);
}
