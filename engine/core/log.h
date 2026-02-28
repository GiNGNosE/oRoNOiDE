#pragma once
#include <cstdio>

#define ORO_LOG_LEVEL_TRACE 0
#define ORO_LOG_LEVEL_DEBUG 1
#define ORO_LOG_LEVEL_INFO 2
#define ORO_LOG_LEVEL_WARN 3
#define ORO_LOG_LEVEL_ERROR 4
#define ORO_LOG_LEVEL_FATAL 5

#ifndef ORO_LOG_MIN_LEVEL
#define ORO_LOG_MIN_LEVEL ORO_LOG_LEVEL_INFO
#endif

#define ORO_LOG_TRACE(fmt, ...) \
    do { if (ORO_LOG_LEVEL_TRACE >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[TRACE] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define ORO_LOG_DEBUG(fmt, ...) \
    do { if (ORO_LOG_LEVEL_DEBUG >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define ORO_LOG_INFO(fmt, ...) \
    do { if (ORO_LOG_LEVEL_INFO >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[INFO] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define ORO_LOG_WARN(fmt, ...) \
    do { if (ORO_LOG_LEVEL_WARN >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[WARN] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define ORO_LOG_ERROR(fmt, ...) \
    do { if (ORO_LOG_LEVEL_ERROR >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define ORO_LOG_FATAL(fmt, ...) \
    do { if (ORO_LOG_LEVEL_FATAL >= ORO_LOG_MIN_LEVEL) \
        fprintf(stderr, "[FATAL] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
