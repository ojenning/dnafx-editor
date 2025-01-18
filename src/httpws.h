#ifndef DNAFX_HTTPWS
#define DNAFX_HTTPWS

/* Server management */
int dnafx_httpws_init(uint16_t port);
void dnafx_httpws_deinit(void);

/* Task completion */
void dnafx_httpws_task_done(int code, void *result, void *user_data);

#endif
