#ifndef NOR_CUSTOM_CONFIG_H
#define NOR_CUSTOM_CONFIG_H

#include "lx_stm32_nor_custom_driver.h"

#define LX_NOR_CUSTOM_DRIVERS   { \
    .name = NOR_CUSTOM_DRIVER_NAME, \
    .id = NOR_CUSTOM_DRIVER_ID, \
    .nor_driver_initialize = lx_stm32_nor_custom_driver_initialize \
}

#endif /* NOR_CUSTOM_CONFIG_H */
