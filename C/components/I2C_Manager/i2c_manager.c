#include "i2c_manager.h"

#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_PORT I2C_NUM_0

static i2c_master_bus_handle_t bus_handle;

esp_err_t i2c_manager_init(void)
{
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(
        &i2c_mst_config,
        &bus_handle
    );
}

i2c_master_bus_handle_t i2c_manager_get_bus_handle(void)
{
    return bus_handle;
}