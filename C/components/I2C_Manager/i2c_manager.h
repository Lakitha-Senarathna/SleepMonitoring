#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

#include "driver/i2c_master.h"

esp_err_t i2c_manager_init(void);

i2c_master_bus_handle_t i2c_manager_get_bus_handle(void);

#endif