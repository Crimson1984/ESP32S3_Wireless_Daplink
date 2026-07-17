#pragma once

#include "datlink_protocol.h"
#include "esp_err.h"

typedef void (*gateway_usb_handler_t)(const datlink_usb_frame_t *frame,
                                      void *context);

esp_err_t gateway_usb_init(gateway_usb_handler_t handler, void *context);
esp_err_t gateway_usb_send(const datlink_usb_frame_t *frame);

