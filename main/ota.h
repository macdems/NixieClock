#ifndef NIXIE_CLOCK_OTA_H
#define NIXIE_CLOCK_OTA_H

void ota_task(void* param);

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
void ota_cancel_rollback();
#endif

#endif  // NIXIE_CLOCK_OTA_H
