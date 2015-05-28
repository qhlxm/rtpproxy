/*
 * Copyright (c) 2010-2014 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#if defined(HAVE_CONFIG_H)
#include "config_pp.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rtpp_log.h"
#include "rtpp_types.h"
#include "rtpp_notify.h"
#include "rtpp_queue.h"
#include "rtpp_tnotify_tgt.h"
#include "rtpp_wi.h"

struct rtpp_notify_wi
{
    int len;
    struct rtpp_tnotify_target *rttp;
    rtpp_log_t glog;
    char notify_buf[0];
};

struct rtpp_notify_priv {
    struct rtpp_notify_obj pub;
    struct rtpp_queue *nqueue;
    struct rtpp_wi *sigterm;
    pthread_t thread_id;
    rtpp_log_t glog;
};

#define PUB2PVT(pubp)      ((struct rtpp_notify_priv *)((char *)(pubp) - offsetof(struct rtpp_notify_priv, pub)))

static int rtpp_notify_schedule(struct rtpp_notify_obj *,
  struct rtpp_tnotify_target *, const char *);
static void rtpp_notify_dtor(struct rtpp_notify_obj *);
static void do_timeout_notification(struct rtpp_notify_wi *, int);

static void
rtpp_notify_queue_run(void *arg)
{
    struct rtpp_wi *wi;
    struct rtpp_notify_wi *wi_data;
    struct rtpp_notify_priv *pvt;

    pvt = (struct rtpp_notify_priv *)arg;
    for (;;) {
        wi = rtpp_queue_get_item(pvt->nqueue, 0);
        if (rtpp_wi_get_type(wi) == RTPP_WI_TYPE_SGNL) {
            rtpp_wi_free(wi);
            break;
        }
        wi_data = rtpp_wi_data_get_ptr(wi, sizeof(struct rtpp_notify_wi), 0);

        /* main work here */
        do_timeout_notification(wi_data, 1);

        /* deallocate wi */
        rtpp_wi_free(wi);
    }
}

struct rtpp_notify_obj *
rtpp_notify_ctor(rtpp_log_t glog)
{
    struct rtpp_notify_priv *pvt;

    pvt = malloc(sizeof(struct rtpp_notify_priv));
    if (pvt == NULL) {
        goto e0;
    }
    memset(pvt, '\0', sizeof(struct rtpp_notify_priv));
    pvt->nqueue = rtpp_queue_init(1, "rtpp_notify");
    if (pvt->nqueue == NULL) {
        goto e1;
    }

    /* Pre-allocate sigterm, so that we don't have any malloc() in dtor() */
    pvt->sigterm = rtpp_wi_malloc_sgnl(SIGTERM, NULL, 0);
    if (pvt->sigterm == NULL) {
        goto e2;
    }

    if (pthread_create(&pvt->thread_id, NULL, (void *(*)(void *))&rtpp_notify_queue_run, pvt) != 0) {
        goto e3;
    }

    pvt->glog = glog;
    pvt->pub.schedule = &rtpp_notify_schedule;
    pvt->pub.dtor = &rtpp_notify_dtor;

    return (&pvt->pub);

e3:
    rtpp_wi_free(pvt->sigterm);
e2:
    rtpp_queue_destroy(pvt->nqueue);
e1:
    free(pvt);
e0:
    return (NULL);
}

static void
rtpp_notify_dtor(struct rtpp_notify_obj *pub)
{
    struct rtpp_notify_priv *pvt;

    pvt = PUB2PVT(pub);

    rtpp_queue_put_item(pvt->sigterm, pvt->nqueue);
    pthread_join(pvt->thread_id, NULL);
    rtpp_queue_destroy(pvt->nqueue);
    free(pvt);
}

static int
rtpp_notify_schedule(struct rtpp_notify_obj *pub,
  struct rtpp_tnotify_target *rttp, const char *notify_tag)
{
    struct rtpp_notify_wi *wi_data;
    struct rtpp_wi *wi;
    int len;
    struct rtpp_notify_priv *pvt;

    pvt = PUB2PVT(pub);

    /* string, \0 and \n */
    len = strlen(notify_tag) + 2;

    wi = rtpp_wi_malloc_udata((void **)&wi_data,
      sizeof(struct rtpp_notify_wi) + len);
    if (wi == NULL) {
        return (-1);
    }
    memset(wi_data, '\0', sizeof(struct rtpp_notify_wi) + len);

    wi_data->rttp = rttp;
    wi_data->len = len;

    len = snprintf(wi_data->notify_buf, len, "%s\n", notify_tag);

    wi_data->glog = pvt->glog;

    rtpp_queue_put_item(wi, pvt->nqueue);
    return (0);
}

static void
reconnect_timeout_handler(rtpp_log_t log, struct rtpp_tnotify_target *rttp)
{

    assert (rttp->socket_name != NULL && rttp->connected == 0);

    if (rttp->fd == -1) {
        rtpp_log_write(RTPP_LOG_DBUG, log, "connecting timeout socket");
    } else {
        rtpp_log_write(RTPP_LOG_DBUG, log, "reconnecting timeout socket");
        close(rttp->fd);
    }
    rttp->fd = socket(rttp->socket_type, SOCK_STREAM, 0);
    if (rttp->fd == -1) {
        rtpp_log_ewrite(RTPP_LOG_ERR, log, "can't create timeout socket");
        return;
    }

    if (connect(rttp->fd, (struct sockaddr *)&(rttp->remote), rttp->remote_len) == -1) {
        rtpp_log_ewrite(RTPP_LOG_ERR, log, "can't connect to timeout socket");
    } else {
        rttp->connected = 1;
    }
}

static void
do_timeout_notification(struct rtpp_notify_wi *wi, int retries)
{
    int result;

    if (wi->rttp->connected == 0) {
        reconnect_timeout_handler(wi->glog, wi->rttp);

        /* If connect fails, no notification will be sent */
        if (wi->rttp->connected == 0) {
            rtpp_log_write(RTPP_LOG_ERR, wi->glog, "unable to send timeout notification");
            return;
        }
    }

    do {
        result = send(wi->rttp->fd, wi->notify_buf, wi->len - 1, 0);
    } while (result == -1 && errno == EINTR);

    if (result < 0) {
        wi->rttp->connected = 0;
        rtpp_log_ewrite(RTPP_LOG_ERR, wi->glog, "failed to send timeout notification");
        if (retries > 0)
            do_timeout_notification(wi, retries - 1);
    }
}
