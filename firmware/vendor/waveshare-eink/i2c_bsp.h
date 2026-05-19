#ifndef I2C_BSP_H
#define I2C_BSP_H

#include <driver/i2c_master.h>

class I2cMasterBus
{
private:
    i2c_master_bus_handle_t user_i2c_handle = NULL;
    I2cMasterBus(int scl_pin,int sda_pin,int i2c_port);
    /* NOT VENDOR CODE — default-construct path used by adoptHandle()
     * when the bus handle is owned externally. Skips the vendor
     * `i2c_new_master_bus` call. */
    I2cMasterBus() {}
    ~I2cMasterBus();

public:
    int i2c_write_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len);
    int i2c_master_write_read_dev(i2c_master_dev_handle_t dev_handle,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen);
    int i2c_read_buff(i2c_master_dev_handle_t dev_handle,int reg,uint8_t *buf,uint8_t len);
    i2c_master_bus_handle_t Get_I2cBusHandle();
    static I2cMasterBus *instance_;
    static I2cMasterBus *requestInstance(int scl_pin,int sda_pin,int i2c_port);

    /* NOT VENDOR CODE — added for project i2c_bus singleton.
     * Construct (or replace) the singleton with an externally
     * managed handle (e.g. codec_board's bus). Prevents the vendor
     * constructor's `i2c_new_master_bus` call when the bus was
     * already brought up by another component on the same port.
     * See firmware/main/i2c_bus.{cpp,h}. */
    static I2cMasterBus *adoptHandle(i2c_master_bus_handle_t external);
};

#endif