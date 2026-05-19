#include "i2c_bus.h"

#include "esp_log.h"
#include "board_pins.h"
#include "i2c_bsp.h"

extern "C" {
#include "codec_board.h"
#include "codec_init.h"
}

namespace i2c_bus {

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_handle = nullptr;
static bool s_tried = false;

i2c_master_bus_handle_t handle(void) {
    if (s_handle) return s_handle;
    if (s_tried) return nullptr;
    s_tried = true;

    /* Tell codec_board which board we're on so its init_i2c uses the
     * right SDA/SCL pins from board_cfg.txt's WAVESHARE_S3_EPAPER_1_54
     * entry. set_codec_board_type() is idempotent in upstream impl;
     * voice::init() may also set it later, that's fine. */
    set_codec_board_type("WAVESHARE_S3_EPAPER_1_54");

    int rc = init_i2c(MOCHI_I2C_PORT);
    if (rc != 0) {
        ESP_LOGE(TAG, "codec_board init_i2c(%d) failed: %d",
            MOCHI_I2C_PORT, rc);
        return nullptr;
    }
    s_handle = (i2c_master_bus_handle_t)get_i2c_bus_handle(MOCHI_I2C_PORT);
    if (!s_handle) {
        ESP_LOGE(TAG, "get_i2c_bus_handle(%d) returned NULL",
            MOCHI_I2C_PORT);
        return nullptr;
    }

    /* Hand the same handle to the vendor i2c_bsp singleton. The
     * vendor FT6336 driver (ft6336_bsp.cpp) calls into
     * `I2cMasterBus::instance_` for register reads; if we leave
     * instance_ null, the touch driver crashes the moment it tries
     * to read coords. adoptHandle stamps in our handle without
     * letting the vendor constructor try to acquire its own master.
     * See firmware/vendor/waveshare-eink/i2c_bsp.{h,cpp} for the
     * NOT VENDOR CODE additions that enable this. */
    I2cMasterBus::adoptHandle(s_handle);

    ESP_LOGI(TAG, "i2c bus ready (port=%d, sda=%d, scl=%d)",
        MOCHI_I2C_PORT, MOCHI_I2C_SDA_GPIO, MOCHI_I2C_SCL_GPIO);
    return s_handle;
}

}  /* namespace i2c_bus */
