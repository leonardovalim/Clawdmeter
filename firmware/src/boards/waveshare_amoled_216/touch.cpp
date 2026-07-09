#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

// The CO5300 can't rotate in hardware, so display.cpp rotates every strip on
// the CPU at flush time (see rotate_strip). LVGL therefore keeps drawing in an
// unrotated coordinate space while the panel — and the touch controller bonded
// to it — report physical coordinates. Undo the display's rotation here so a
// finger on a widget lands on that widget in LVGL space.
//
// rotate_strip maps LVGL (x,y) -> panel:
//   r=1 (90° CW)  (x,y) -> (S-1-y, x)
//   r=2 (180°)    (x,y) -> (S-1-x, S-1-y)
//   r=3 (270° CW) (x,y) -> (y, S-1-x)
// What follows is the exact inverse of each.
static void unrotate_touch(uint16_t* x, uint16_t* y) {
    const uint16_t S = LCD_WIDTH;   // panel is square, so one side suffices
    const uint16_t px = *x, py = *y;
    switch (imu_hal_rotation_quadrant()) {
    case 1: *x = py;            *y = S - 1 - px;   break;
    case 2: *x = S - 1 - px;    *y = S - 1 - py;   break;
    case 3: *x = S - 1 - py;    *y = px;           break;
    default: break;                                 // r=0: identity
    }
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
    unrotate_touch(x, y);
}
