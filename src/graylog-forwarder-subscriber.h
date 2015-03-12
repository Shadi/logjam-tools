#ifndef __LOGJAM_GRAYLOG_FORWARDER_SUBSCRIBER_H_INCLUDED__
#define __LOGJAM_GRAYLOG_FORWARDER_SUBSCRIBER_H_INCLUDED__

#include "graylog-forwarder-common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void graylog_forwarder_subscriber(zsock_t *pipe, void *args);

#ifdef __cplusplus
}
#endif

#endif
