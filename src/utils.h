#ifndef DNAFX_UTILS
#define DNAFX_UTILS

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <glib.h>

/* Printing and strings */
void dnafx_print_hex(int level, const char *separator, uint8_t *buf, size_t buflen);
void dnafx_trim_string(char *str);

/* Files and directories */
int dnafx_mkdir(const char *dir, mode_t mode);
int dnafx_read_file(const char *filename, gboolean text, uint8_t *buffer, size_t blen);
int dnafx_write_file(const char *filename, gboolean text, uint8_t *buffer, size_t blen);

#endif
