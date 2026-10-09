#include "driver/i2c_master.h"
#include "i2c_manager.h"

#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_PORT I2C_NUM_0

// i2c master bus configuration
i2c_master_bus_config_t i2c_master_bus_config_structure = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_MASTER_PORT,
    .scl_io_num = I2C_MASTER_SCL_IO,
    .sda_io_num = I2C_MASTER_SDA_IO,
    .glitch_ignore_cnt = 7,
    //.flags.enable_internal_pullup = true,

        /*  Keep in mind that internal pull up resistors were not used
        So Keep in mind to add those externally */        
    };

i2c_master_bus_handle_t bus_handle_structure = NULL;

// i2c master bus creation
i2c_new_master_bus(
        i2c_master_bus_config_structure,
        bus_handle_structure
    );

// i2c master device configuration
i2c_device_config_t i2c_device_config_structure = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = 0x3C,
    .scl_speed_hz = 400000,
};

i2c_master_bus_handle_t device_handle_structure = NULL;

// i2c master device creation
i2c_new_master_device(
        bus_handle_structure,
        i2c_device_config_structure,
        device_handle_structure
    );