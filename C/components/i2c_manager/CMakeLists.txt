idf_component_register(
    SRCS ${i2c_manager.c}
    iNCLUDE_DIRS "."
    PRIV_REQUIRED i2c_driver
    )