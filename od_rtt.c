#include "od_rtt.h"
#include "SEGGER_RTT.h"
#include <sys/reent.h>

void od_rtt_init(void)
{
  SEGGER_RTT_Init();
}

int _write(int file, const char *ptr, int len)
{
  (void)file;
  if (len <= 0) {
    return 0;
  }
  SEGGER_RTT_Write(0, ptr, (unsigned)len);
  return len;
}

int _write_r(struct _reent *r, int file, const char *ptr, int len)
{
  (void)r;
  return _write(file, ptr, len);
}
