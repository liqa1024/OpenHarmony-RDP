/*
 * HmRdp - HarmonyOS RDP client
 * Small hilog wrapper shared by the native sources.
 */
#ifndef HMRDP_LOG_H
#define HMRDP_LOG_H

#include <hilog/log.h>

#define HMRDP_LOG_DOMAIN 0xD001
#define HMRDP_LOG_TAG "HmRdpNative"

#define HMRDP_LOGI(...) \
  OH_LOG_Print(LOG_APP, LOG_INFO, HMRDP_LOG_DOMAIN, HMRDP_LOG_TAG, __VA_ARGS__)
#define HMRDP_LOGW(...) \
  OH_LOG_Print(LOG_APP, LOG_WARN, HMRDP_LOG_DOMAIN, HMRDP_LOG_TAG, __VA_ARGS__)
#define HMRDP_LOGE(...) \
  OH_LOG_Print(LOG_APP, LOG_ERROR, HMRDP_LOG_DOMAIN, HMRDP_LOG_TAG, __VA_ARGS__)

#endif  // HMRDP_LOG_H
