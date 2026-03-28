/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_codec_ctrl_if.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define TICKS(ms) pdMS_TO_TICKS(ms)
#else
#define TICK_PER_MS portTICK_RATE_MS
#endif

#define TAG "I2C_If"

// 👉 Default pins (CHANGE THESE if needed)
#define I2C_DEFAULT_SDA 21
#define I2C_DEFAULT_SCL 22

typedef struct {
    audio_codec_ctrl_if_t base;
    bool                  is_open;
    uint8_t               port;
    uint8_t               addr;
    i2c_master_dev_handle_t dev;
    i2c_master_bus_handle_t bus;
} i2c_ctrl_t;

static int _i2c_ctrl_open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size)
{
    if (ctrl == NULL || cfg == NULL || cfg_size != sizeof(audio_codec_i2c_cfg_t)) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    i2c_ctrl_t *i2c_ctrl = (i2c_ctrl_t *) ctrl;
    audio_codec_i2c_cfg_t *i2c_cfg = (audio_codec_i2c_cfg_t *) cfg;

    i2c_ctrl->port = i2c_cfg->port;
    i2c_ctrl->addr = i2c_cfg->addr;

    // Configure I2C bus (using default pins)
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = i2c_ctrl->port,
        .sda_io_num = I2C_DEFAULT_SDA,
        .scl_io_num = I2C_DEFAULT_SCL,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_ctrl->bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus");
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    // Convert to 7-bit address
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_ctrl->addr >> 1,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(i2c_ctrl->bus, &dev_cfg, &i2c_ctrl->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device");
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    return 0;
}

static bool _i2c_ctrl_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    if (ctrl) {
        i2c_ctrl_t *i2c_ctrl = (i2c_ctrl_t *) ctrl;
        return i2c_ctrl->is_open;
    }
    return false;
}

static int _i2c_ctrl_read_reg(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (ctrl == NULL || data == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    i2c_ctrl_t *i2c_ctrl = (i2c_ctrl_t *) ctrl;

    if (!i2c_ctrl->is_open) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }

    esp_err_t ret = i2c_master_transmit_receive(
        i2c_ctrl->dev,
        (uint8_t *)&addr,
        addr_len,
        (uint8_t *)data,
        data_len,
        TICKS(1000)
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to read from dev %x", i2c_ctrl->addr);
        return ESP_CODEC_DEV_READ_FAIL;
    }

    return ESP_CODEC_DEV_OK;
}

static int _i2c_ctrl_write_reg(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (ctrl == NULL || data == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    i2c_ctrl_t *i2c_ctrl = (i2c_ctrl_t *) ctrl;

    if (!i2c_ctrl->is_open) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }

    uint8_t *buffer = malloc(addr_len + data_len);
    if (!buffer) {
        return ESP_CODEC_DEV_WRITE_FAIL;
    }

    memcpy(buffer, &addr, addr_len);
    if (data_len) {
        memcpy(buffer + addr_len, data, data_len);
    }

    esp_err_t ret = i2c_master_transmit(
        i2c_ctrl->dev,
        buffer,
        addr_len + data_len,
        TICKS(1000)
    );

    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to write to dev %x", i2c_ctrl->addr);
        return ESP_CODEC_DEV_WRITE_FAIL;
    }

    return ESP_CODEC_DEV_OK;
}

static int _i2c_ctrl_close(const audio_codec_ctrl_if_t *ctrl)
{
    if (ctrl == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    i2c_ctrl_t *i2c_ctrl = (i2c_ctrl_t *) ctrl;

    if (i2c_ctrl->dev) {
        i2c_master_bus_rm_device(i2c_ctrl->dev);
    }

    if (i2c_ctrl->bus) {
        i2c_del_master_bus(i2c_ctrl->bus);
    }

    i2c_ctrl->is_open = false;
    return 0;
}

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *i2c_cfg)
{
    if (i2c_cfg == NULL) {
        ESP_LOGE(TAG, "Bad configuration");
        return NULL;
    }

    i2c_ctrl_t *ctrl = calloc(1, sizeof(i2c_ctrl_t));
    if (ctrl == NULL) {
        ESP_LOGE(TAG, "No memory for instance");
        return NULL;
    }

    ctrl->base.open = _i2c_ctrl_open;
    ctrl->base.is_open = _i2c_ctrl_is_open;
    ctrl->base.read_reg = _i2c_ctrl_read_reg;
    ctrl->base.write_reg = _i2c_ctrl_write_reg;
    ctrl->base.close = _i2c_ctrl_close;

    int ret = _i2c_ctrl_open(&ctrl->base, i2c_cfg, sizeof(audio_codec_i2c_cfg_t));
    if (ret != 0) {
        free(ctrl);
        return NULL;
    }

    ctrl->is_open = true;
    return &ctrl->base;
}