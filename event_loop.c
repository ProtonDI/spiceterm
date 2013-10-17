/*

     Copyright (C) 2013 Proxmox Server Solutions GmbH

     Copyright: spiceterm is under GNU GPL, the GNU General Public License.

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation; version 2 dated June, 1991.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
     02111-1307, USA.

     Author: Dietmar Maurer <dietmar@proxmox.com>

*/

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <glib.h>

#include <spice/macros.h>
#include "event_loop.h"

static int debug = 0;
    
#define DPRINTF(x, format, ...) { \
    if (x <= debug) { \
        printf("%s: " format "\n" , __FUNCTION__, ## __VA_ARGS__); \
    } \
}

static char *auth_path = "/";
static char *auth_perm = "Sys.Console";
static char clientip[INET6_ADDRSTRLEN];

static GMainLoop *main_loop;

static SpiceCoreInterface core;

typedef struct SpiceTimer {
    GSource *source;
    SpiceTimerFunc func;
    int ms;
    void *opaque;
} Timer;

static SpiceTimer* timer_add(SpiceTimerFunc func, void *opaque)
{
    g_return_val_if_fail(func != NULL, NULL);

    SpiceTimer *timer = g_new0(SpiceTimer, 1);

    timer->func = func;
    timer->opaque = opaque;

    return timer;
}

static gboolean timer_callback(gpointer data)
{
    SpiceTimer *timer = (SpiceTimer *)data;
    g_assert(timer != NULL);
    g_assert(timer->func != NULL);

    timer->func(timer->opaque);

    return FALSE;
}

static void timer_start(SpiceTimer *timer, uint32_t ms)
{
    g_return_if_fail(timer != NULL);
    g_return_if_fail(ms != 0);

    if (timer->source != NULL) {
        g_source_destroy(timer->source);
    }
    
    timer->source = g_timeout_source_new(ms);
    g_assert(timer->source != NULL);

    g_source_set_callback(timer->source, timer_callback, timer, NULL);

    g_source_attach(timer->source, NULL);
}

static void timer_cancel(SpiceTimer *timer)
{
    g_return_if_fail(timer != NULL);

    if (timer->source != NULL) {
        g_source_destroy(timer->source);
        timer->source = NULL;
    }

    timer->ms = 0;
}

static void timer_remove(SpiceTimer *timer)
{
    g_return_if_fail(timer != NULL);

    timer_cancel(timer);
    g_free(timer);
}

struct SpiceWatch {
    GIOChannel *channel;
    guint evid;
    int fd;
    int event_mask;
    SpiceWatchFunc func;
    void *opaque;
};

static gboolean watch_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
    SpiceWatch *watch = (SpiceWatch *)data;

    g_assert(watch != NULL);
    g_assert(watch->func != NULL);

    if (condition & G_IO_OUT) {
        watch->func(watch->fd, SPICE_WATCH_EVENT_WRITE, watch->opaque);
    }

    if (condition & G_IO_IN) {
        watch->func(watch->fd, SPICE_WATCH_EVENT_READ, watch->opaque);
    }

    return TRUE;
}

static GIOCondition event_mask_to_condition(int event_mask)
{
    GIOCondition condition = 0;

    if (event_mask & SPICE_WATCH_EVENT_READ) {
        condition |= G_IO_IN;
    }

    if (event_mask & SPICE_WATCH_EVENT_WRITE) {
        condition |= G_IO_OUT;
    }

    return condition;
}

static SpiceWatch *watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    SpiceWatch *watch = g_new0(SpiceWatch, 1);

    DPRINTF(1, "adding %p, fd=%d", watch, fd);

    watch->fd = fd;
    watch->event_mask = event_mask;
    watch->func = func;
    watch->opaque = opaque;
    watch->channel = g_io_channel_unix_new(fd);

    g_assert(watch->channel != NULL);
    g_io_channel_set_encoding(watch->channel, NULL, NULL);

    GIOCondition condition = event_mask_to_condition(event_mask);
    watch->evid = g_io_add_watch(watch->channel, condition, watch_callback, watch);

    return watch;
}

static void watch_update_mask(SpiceWatch *watch, int event_mask)
{
    g_assert(watch != NULL);

    DPRINTF(1, "fd %d to %d", watch->fd, event_mask);
 
    watch->event_mask = event_mask;

    g_source_remove(watch->evid);

    GIOCondition condition = event_mask_to_condition(event_mask);
    watch->evid = g_io_add_watch(watch->channel, condition, watch_callback, watch);    
}

static void watch_remove(SpiceWatch *watch)
{
    g_assert(watch != NULL);

    DPRINTF(1, "remove %p (fd %d)", watch, watch->fd);

    g_source_remove(watch->evid);
    g_io_channel_unref(watch->channel);

    g_free(watch);
}

static void channel_event(int event, SpiceChannelEventInfo *info)
{
    DPRINTF(1, "channel event con, type, id, event: %d, %d, %d, %d",
            info->connection_id, info->type, info->id, event);
}

void basic_event_loop_mainloop(void)
{
    g_main_loop_run(main_loop);
}

static void ignore_sigpipe(void)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    sigfillset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);
}

static char *
urlencode(char *buf, const char *value)
{
    static const char *hexchar = "0123456789abcdef";
    char *p = buf;
    int i;
    int l = strlen(value);
    for (i = 0; i < l; i++) {
        char c = value[i];
        if (('a' <= c && c <= 'z') ||
            ('A' <= c && c <= 'Z') ||
            ('0' <= c && c <= '9')) {
            *p++ = c;
        } else if (c == 32) {
            *p++ = '+';
        } else {
            *p++ = '%';
            *p++ = hexchar[c >> 4];
            *p++ = hexchar[c & 15];
        }
    }
    *p = 0;

    return p;
}

static int 
pve_auth_verify(const char *clientip, const char *username, const char *passwd)
{
    struct sockaddr_in server;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("pve_auth_verify: socket failed");
        return -1;
    }

    struct hostent *he;
    if ((he = gethostbyname("localhost")) == NULL) {
        fprintf(stderr, "pve_auth_verify: error resolving hostname\n");
        goto err;
    }

    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
    server.sin_family = AF_INET;
    server.sin_port = htons(85);

    if (connect(sfd, (struct sockaddr *)&server, sizeof(server))) {
        perror("pve_auth_verify: error connecting to server");
        goto err;
    }

    char buf[8192];
    char form[8192];

    char *p = form;
    p = urlencode(p, "username");
    *p++ = '=';
    p = urlencode(p, username);

    *p++ = '&';
    p = urlencode(p, "password");
    *p++ = '=';
    p = urlencode(p, passwd);

    *p++ = '&';
    p = urlencode(p, "path");
    *p++ = '=';
    p = urlencode(p, auth_path);

    *p++ = '&';
    p = urlencode(p, "privs");
    *p++ = '=';
    p = urlencode(p, auth_perm);

    sprintf(buf, "POST /api2/json/access/ticket HTTP/1.1\n"
            "Host: localhost:85\n"
            "Connection: close\n"
            "PVEClientIP: %s\n"
            "Content-Type: application/x-www-form-urlencoded\n"
            "Content-Length: %zd\n\n%s\n", clientip, strlen(form), form);
    ssize_t len = strlen(buf);
    ssize_t sb = send(sfd, buf, len, 0);
    if (sb < 0) {
        perror("pve_auth_verify: send failed");
        goto err;
    }
    if (sb != len) {
        fprintf(stderr, "pve_auth_verify: partial send error\n");
        goto err;
    }

    len = recv(sfd, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
        perror("pve_auth_verify: recv failed");
        goto err;
    }

    buf[len] = 0;

    //printf("DATA:%s\n", buf);

    shutdown(sfd, SHUT_RDWR);

    if (!strncmp(buf, "HTTP/1.1 200 OK", 15)) {
        return 0;
    }

    char *firstline = strtok(buf, "\n");
    
    fprintf(stderr, "auth failed: %s\n", firstline);

    return -1;

err:
    shutdown(sfd, SHUT_RDWR);
    return -1;
}

static int
verify_credentials(const char *username, const char *password)
{
    return pve_auth_verify(clientip, username, password);
}

SpiceCoreInterface *basic_event_loop_init(void)
{
    main_loop = g_main_loop_new(NULL, FALSE);

    memset(&core, 0, sizeof(core));
    core.base.major_version = SPICE_INTERFACE_CORE_MAJOR;
    core.base.minor_version = SPICE_INTERFACE_CORE_MINOR;
    core.timer_add = timer_add;
    core.timer_start = timer_start;
    core.timer_cancel = timer_cancel;
    core.timer_remove = timer_remove;
    core.watch_add = watch_add;
    core.watch_update_mask = watch_update_mask;
    core.watch_remove = watch_remove;
    core.channel_event = channel_event;

    core.auth_plain_verify_credentials = verify_credentials;

    ignore_sigpipe();

    return &core;
}
