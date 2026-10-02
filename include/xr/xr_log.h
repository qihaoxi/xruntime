#ifndef XR_LOG_H
#define XR_LOG_H

typedef enum
{
	XR_LOG_DEBUG = 0,
	XR_LOG_INFO = 1,
	XR_LOG_WARN = 2,
	XR_LOG_ERROR = 3,
} xr_log_level_t;

void xr_log_set_level(xr_log_level_t lvl);
xr_log_level_t xr_log_get_level(void);
void xr_logf(xr_log_level_t lvl, const char *file, int line, const char *fmt,
	     ...) __attribute__((format(printf, 4, 5)));

#define XR_LOGD(...) xr_logf(XR_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define XR_LOGI(...) xr_logf(XR_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define XR_LOGW(...) xr_logf(XR_LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define XR_LOGE(...) xr_logf(XR_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif
