#ifndef __BROOKESIA_H__
#define __BROOKESIA_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BROOKESIA_AGENT_XIAOZHI,
    BROOKESIA_AGENT_COZE,
    BROOKESIA_AGENT_DOUBAO
} brookesia_agent_t;

typedef struct {
} brookesia_cfg_t;

#define BROOKESIA_DEFAULT_CONFIG() (brookesia_cfg_t){}

esp_err_t brookesia_init(brookesia_cfg_t *cfg);
void brookesia_set_agent(const char *name);
void brookesia_start_session(void);
void brookesia_stop_session(void);

#ifdef __cplusplus
}
#endif

#endif
