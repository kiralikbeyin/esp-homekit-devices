/*
 * Advanced Button Manager
 *
 * Copyright 2018 José A. Jiménez (@RavenSystem)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Based on Button library by Maxin Kulkin (@MaximKulkin), licensed under the MIT License.
 * https://github.com/maximkulkin/esp-homekit-demo/blob/master/examples/button/button.c
 */

#include <string.h>
#include <etstimer.h>
#include <esplibs/libmain.h>
#include "adv_button.h"

#define DEBOUNCE_FRECUENCY  50
#define DOUBLEPRESS_TIME    400
#define LONGPRESS_TIME      450
#define VERYLONGPRESS_TIME  1200
#define HOLDPRESS_TIME      10000

typedef struct _adv_button {
    uint8_t gpio;
    
    button_callback_fn singlepress_callback_fn;
    button_callback_fn doublepress_callback_fn;
    button_callback_fn longpress_callback_fn;
    button_callback_fn verylongpress_callback_fn;
    button_callback_fn holdpress_callback_fn;

    uint8_t press_count;
    ETSTimer press_timer;
    ETSTimer hold_timer;
    uint32_t last_event_time;

    struct _adv_button *next;
} adv_button_t;

static adv_button_t *buttons = NULL;
static uint8_t used_gpio;

static adv_button_t *button_find_by_gpio(const uint8_t gpio) {
    adv_button_t *button = buttons;
    while (button && button->gpio != gpio) {
        button = button->next;
    }

    return button;
}

static void push_down_timer_callback() {
    timer_set_run(FRC1, false);
    timer_set_interrupts(FRC1, false);
    
    if (gpio_read(used_gpio) == 0) {
        adv_button_t *button = button_find_by_gpio(used_gpio);
        sdk_os_timer_arm(&button->hold_timer, HOLDPRESS_TIME, 0);
        button->last_event_time = xTaskGetTickCountFromISR();
    }
}

static void push_up_timer_callback() {
    timer_set_run(FRC2, false);
    timer_set_interrupts(FRC2, false);
    
    if (gpio_read(used_gpio) == 1) {
        adv_button_t *button = button_find_by_gpio(used_gpio);
        sdk_os_timer_disarm(&button->hold_timer);
        uint32_t now = xTaskGetTickCountFromISR();
        
        if (now - button->last_event_time > VERYLONGPRESS_TIME / portTICK_PERIOD_MS) {
            // Very Long button pressed
            button->press_count = 0;
            if (button->verylongpress_callback_fn) {
                button->verylongpress_callback_fn(used_gpio);
            } else if (button->longpress_callback_fn) {
                button->longpress_callback_fn(used_gpio);
            } else {
                button->singlepress_callback_fn(used_gpio);
            }
        } else if (now - button->last_event_time > LONGPRESS_TIME / portTICK_PERIOD_MS) {
            // Long button pressed
            button->press_count = 0;
            if (button->longpress_callback_fn) {
                button->longpress_callback_fn(used_gpio);
            } else {
                button->singlepress_callback_fn(used_gpio);
            }
        } else if (button->doublepress_callback_fn) {
            button->press_count++;
            if (button->press_count > 1) {
                // Double button pressed
                sdk_os_timer_disarm(&button->press_timer);
                button->press_count = 0;
                button->doublepress_callback_fn(used_gpio);
            } else {
                sdk_os_timer_arm(&button->press_timer, DOUBLEPRESS_TIME, 0);
            }
        } else {
            button->singlepress_callback_fn(used_gpio);
        }
    }
}

static void adv_button_intr_callback(const uint8_t gpio) {
    adv_button_t *button = button_find_by_gpio(gpio);
    if (!button) {
        return;
    }
    used_gpio = gpio;
    
    if (gpio_read(used_gpio) == 1) {
        timer_set_frequency(FRC2, DEBOUNCE_FRECUENCY);
        timer_set_interrupts(FRC2, true);
        timer_set_run(FRC2, true);
    } else {
        timer_set_interrupts(FRC1, true);
        timer_set_run(FRC1, true);
    }
}

static void no_function_callback(uint8_t gpio) {
    printf("!!! AdvButton: No function defined\n");
}

static void adv_button_single_callback(void *arg) {
    adv_button_t *button = arg;
    // Single button pressed
    button->press_count = 0;
    button->singlepress_callback_fn(button->gpio);
}

static void adv_button_hold_callback(void *arg) {
    adv_button_t *button = arg;
    
    if (gpio_read(button->gpio) == 0) {
        // Hold button pressed
        button->press_count = 0;
        if (button->holdpress_callback_fn) {
            button->holdpress_callback_fn(button->gpio);
        } else {
            no_function_callback(button->gpio);
        }
    }
}

int adv_button_create(const uint8_t gpio) {
    adv_button_t *button = button_find_by_gpio(gpio);
    if (button) {
        return -1;
    }

    button = malloc(sizeof(adv_button_t));
    memset(button, 0, sizeof(*button));
    button->gpio = gpio;
    
    if (buttons == NULL) {
        timer_set_interrupts(FRC1, false);
        timer_set_run(FRC1, false);
        timer_set_interrupts(FRC2, false);
        timer_set_run(FRC2, false);
        
        _xt_isr_attach(INUM_TIMER_FRC1, push_down_timer_callback, NULL);
        _xt_isr_attach(INUM_TIMER_FRC2, push_up_timer_callback, NULL);
        
        timer_set_frequency(FRC1, DEBOUNCE_FRECUENCY);
    }

    button->next = buttons;
    buttons = button;

    if (button->gpio != 0) {
        gpio_enable(button->gpio, GPIO_INPUT);
    }
    
    gpio_set_pullup(button->gpio, true, true);
    gpio_set_interrupt(button->gpio, GPIO_INTTYPE_EDGE_ANY, adv_button_intr_callback);

    sdk_os_timer_disarm(&button->hold_timer);
    sdk_os_timer_setfn(&button->hold_timer, adv_button_hold_callback, button);
    sdk_os_timer_disarm(&button->press_timer);
    sdk_os_timer_setfn(&button->press_timer, adv_button_single_callback, button);
    
    button->singlepress_callback_fn = no_function_callback;
    
    return 0;
}

int adv_button_register_callback_fn(const uint8_t gpio, button_callback_fn callback, const uint8_t button_callback_type) {
    adv_button_t *button = button_find_by_gpio(gpio);
    if (!button) {
        return -1;
    }
    
    switch (button_callback_type) {
        case 1:
            if (callback) {
                button->singlepress_callback_fn = callback;
            } else {
                button->singlepress_callback_fn = no_function_callback;
            }
            break;
            
        case 2:
            button->doublepress_callback_fn = callback;
            break;
            
        case 3:
            button->longpress_callback_fn = callback;
            break;
            
        case 4:
            button->verylongpress_callback_fn = callback;
            break;
            
        case 5:
            button->holdpress_callback_fn = callback;
            break;
            
        default:
            return -2;
            break;
    }
    
    return 0;
}

void adv_button_destroy(const uint8_t gpio) {
    if (!buttons)
        return;

    adv_button_t *button = NULL;
    if (buttons->gpio == gpio) {
        button = buttons;
        buttons = buttons->next;
    } else {
        adv_button_t *b = buttons;
        while (b->next) {
            if (b->next->gpio == gpio) {
                button = b->next;
                b->next = b->next->next;
                break;
            }
        }
    }

    if (button) {
        sdk_os_timer_disarm(&button->hold_timer);
        sdk_os_timer_disarm(&button->press_timer);
        gpio_set_interrupt(button->gpio, GPIO_INTTYPE_EDGE_ANY, NULL);
        if (button->gpio != 0) {
            gpio_disable(button->gpio);
        }
    }
}