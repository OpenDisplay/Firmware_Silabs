#ifndef SILABS_BBEP_BUSY_INL
#define SILABS_BBEP_BUSY_INL

#ifndef OD_EPD_BUSY_READ_INVERT
#define OD_EPD_BUSY_READ_INVERT 0
#endif

static int silabs_epd_busy_read(BBEPDISP *pBBEP)
{
  int v = digitalRead(pBBEP->iBUSYPin);
#if OD_EPD_BUSY_READ_INVERT
  v = !v;
#endif
  return v;
}

static uint8_t silabs_busy_idle_level(BBEPDISP *pBBEP)
{
  if (!pBBEP) {
    return LOW;
  }
  return (pBBEP->chip_type == BBEP_CHIP_UC81xx) ? HIGH : LOW;
}

void bbepWaitBusy(BBEPDISP *pBBEP)
{
  int iTimeout = 0;
  int iMaxTime = 5000;

  if (!pBBEP) {
    return;
  }
  if (pBBEP->iBUSYPin == 0xff) {
    return;
  }
  delay(10);
  uint8_t busy_idle = silabs_busy_idle_level(pBBEP);
  delay(1);
  if (pBBEP->iFlags & (BBEP_3COLOR | BBEP_4COLOR | BBEP_7COLOR)) {
    iMaxTime = 30000;
  }
  while (iTimeout < iMaxTime) {
    if (silabs_epd_busy_read(pBBEP) == (int)busy_idle) {
      break;
    }
    bbepLightSleep(20, pBBEP->bLightSleep);
    iTimeout += 20;
  }
}

bool bbepIsBusy(BBEPDISP *pBBEP)
{
  if (!pBBEP) {
    return false;
  }
  if (pBBEP->iBUSYPin == 0xff) {
    return false;
  }
  delay(10);
  uint8_t busy_idle = silabs_busy_idle_level(pBBEP);
  delay(1);
  return (silabs_epd_busy_read(pBBEP) != (int)busy_idle);
}

#endif /* SILABS_BBEP_BUSY_INL */
