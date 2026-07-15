/**
  ******************************************************************************
  * @file    app.c
  * @brief   Application orchestrator. Wires the modular drivers together,
  *          handles factory-reset, brings up the network interface in the
  *          configured mode and runs the housekeeping loop (LED ticks,
  *          watchdog, deferred Modbus actions).
  ******************************************************************************
  */

#include "app.h"
#include <string.h>

#include "cmsis_os.h"
#include "iwdg.h"
#include "lwip/api.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "main.h"
#include "stm32f4xx_hal.h"

#include "button_module.h"
#include "led_module.h"
#include "modbus_app.h"
#include "modbus_tcp_server.h"
#include "rtd_module.h"
#include "settings.h"
#include "temp_module.h"

/* The LwIP MX_LWIP_Init() exposes its struct netif so that we can override
 * the addressing after MX_LWIP_Init() has run. */
extern struct netif gnetif;
extern ETH_HandleTypeDef heth;

/* Physical link state from ethernet_link_thread (ethernetif.c). */
extern volatile uint8_t g_eth_any_link_up;

/* Module accessors implemented in modbus_app.c. */
uint8_t modbus_app_take_pending_save(void);
uint8_t modbus_app_take_pending_reboot(void);
uint8_t modbus_app_take_pending_factory_reset(void);
uint8_t modbus_app_take_pending_bootloader(void);
uint32_t modbus_app_last_request_tick(void);

/* ---------------------------------------------------------------------------
 * Sub-tasks
 * ------------------------------------------------------------------------- */

/* RTD acquisition task: reads all four MAX31865 channels once per scan
 * period (configurable, default 250 ms). */
static void rtd_task(void* arg)
{
    (void)arg;
    uint32_t tick = osKernelGetTickCount();
    for (;;) {
        rtd_module_tick();
        tick += rtd_module_scan_period_ms();
        osDelayUntil(tick);
    }
}

/* LED task: 10 ms tick. Drives the main STAT_LED state machine and the four
 * per-channel status LEDs (solid = active, off = inactive, fast blink =
 * converter fault). */
static void led_task(void* arg)
{
    (void)arg;
    const uint16_t period_ms = 10u;
    uint32_t tick = osKernelGetTickCount();
    for (;;) {
        led_module_tick(period_ms);
        rtd_module_led_tick(period_ms);
        tick += period_ms;
        osDelayUntil(tick);
    }
}

/* ---------------------------------------------------------------------------
 * Factory reset routine
 *
 * Order matters: commit defaults to Flash first (HAL_FLASH erase of sector 11
 * blocks the CPU and prevents the LED task from running for ~1-2 s), then
 * play the visual confirmation burst, then reboot. The LED task must already
 * be running when this routine is entered (see app_run()).
 * ------------------------------------------------------------------------- */
static void perform_factory_reset(void)
{
    /* Reset and persist defaults to Flash. */
    settings_reset_to_defaults();
    settings_save();

    /* Enter the sticky LED_STATE_FACTORY_RESET — the LED task drives the
     * configurable periodic ON/OFF blink on its 10 ms tick until the reboot
     * below. Default cadence is 100 ms ON / 100 ms OFF, configurable via
     * led_module_set_factory_reset_timing(). */
    led_module_signal_factory_reset();

    /* 3.5 s wait — long enough for the operator to see the indication at
     * the default 5 Hz cadence (and reasonable headroom at slower settings).
     * The LED keeps blinking the whole time. */
    const uint32_t deadline = HAL_GetTick() + 3500u;
    while (HAL_GetTick() < deadline) {
        HAL_IWDG_Refresh(&hiwdg);
        osDelay(50);
    }
    NVIC_SystemReset();
}

/* ---------------------------------------------------------------------------
 * Network bring-up
 * ------------------------------------------------------------------------- */
static void apply_network_config(void)
{
    const settings_t* s = settings_get();

    if (s->use_dhcp) {
        /* MX_LWIP_Init() already started DHCP. Nothing more to do. */
        return;
    }

    /* Static configuration: stop DHCP if it happens to be running and load
     * the static address. */
    dhcp_release_and_stop(&gnetif);

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   s->ip[0],      s->ip[1],      s->ip[2],      s->ip[3]);
    IP4_ADDR(&mask, s->netmask[0], s->netmask[1], s->netmask[2], s->netmask[3]);
    IP4_ADDR(&gw,   s->gateway[0], s->gateway[1], s->gateway[2], s->gateway[3]);

    netif_set_addr(&gnetif, &ip, &mask, &gw);
}

/* ---------------------------------------------------------------------------
 * Housekeeping
 * ------------------------------------------------------------------------- */
static void update_led_state_from_traffic(void)
{
    /* No physical link on either KSZ8863 port → distinct pattern. */
    if (!g_eth_any_link_up) {
        led_module_set_state(LED_STATE_NO_LINK);
        return;
    }

    const uint32_t now      = HAL_GetTick();
    const uint32_t last_req = modbus_app_last_request_tick();

    /* "Polling" if a Modbus client is currently connected AND we have seen
     * a request in the last 5 s. Otherwise "no polling". */
    extern bool modbus_tcp_server_has_client(void);
    const bool has_client    = modbus_tcp_server_has_client();
    const bool recent_traffic = (last_req != 0u) && ((now - last_req) <= 5000u);

    led_module_set_state((has_client && recent_traffic)
                         ? LED_STATE_POLLING
                         : LED_STATE_NO_POLLING);
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */
void app_run(void)
{
    /* Load settings (or defaults) before anything that consumes them. */
    settings_init();
    settings_t* s = settings_get();

    /* Initialise the LED module and spawn its task BEFORE the button check.
     * The factory-reset burst (10 short blinks) is driven by led_module_tick()
     * which runs from led_task; if the task is not yet running, the burst is
     * silently dropped. */
    led_module_init((uint8_t)s->led_mode);
    const osThreadAttr_t led_attr_early = {
        .name = "LED", .stack_size = 256, .priority = osPriorityLow
    };
    osThreadNew(led_task, NULL, &led_attr_early);

    /* If the FACT_RES button is held at startup, blank-load defaults. We do
     * this synchronously here so that the rest of the boot uses defaults. */
    if (button_wait_held(BUTTON_HOLD_FOR_FACTORY_RESET_MS)) {
        perform_factory_reset();
        /* Not reached. */
    }

    /* Initialise the remaining hardware drivers. */
    rtd_module_init();
    temp_module_init();
    modbus_app_init();

    /* Apply network configuration (static or DHCP). */
    apply_network_config();

    /* HAL_ETH_Init() has already run by the time we get here (LwIP init
     * called it from low_level_init()), so SMI/MIIM access is available.
     * Ensure KSZ8863 port 3 is in RMII mode (bit 6 of Global Control 4). */
    {
        uint32_t tmp = 0;
        HAL_ETH_ReadPHYRegister(&heth, 0, 6, &tmp);  /* Global Control 4 */
        if ((tmp & 0x0040u) == 0u) {
            tmp |= 0x0040u;
            HAL_ETH_WritePHYRegister(&heth, 0, 6, tmp);
        }
    }

    /* Spawn the RTD acquisition task. The LED task was started earlier so that
     * the factory-reset burst is visible during the boot-time button-hold
     * path (see above). */
    const osThreadAttr_t rtd_attr  = {
        .name = "RTD", .stack_size = 768, .priority = osPriorityHigh
    };
    osThreadNew(rtd_task, NULL, &rtd_attr);

    /* Spawn Modbus TCP server. */
    modbus_tcp_server_start();

    /* Housekeeping loop: feeds the watchdog, processes deferred actions and
     * keeps the LED state in sync with the Modbus traffic. */
    uint32_t tick = osKernelGetTickCount();
    for (;;) {
        HAL_IWDG_Refresh(&hiwdg);
        update_led_state_from_traffic();

        if (modbus_app_take_pending_save()) {
            HAL_IWDG_Refresh(&hiwdg);
            settings_save();
        }
        if (modbus_app_take_pending_factory_reset()) {
            perform_factory_reset();
            /* Not reached. */
        }
        if (modbus_app_take_pending_bootloader()) {
            /* Ask the bootloader to stay active after the reset: write the
             * shared magic into the no-init RAM cell, then reset. The cell
             * survives a warm reset; the bootloader consumes it on entry. */
            *(volatile uint32_t *)BOOT_REQUEST_FLAG_ADDR = BOOT_REQUEST_MAGIC;
            const uint32_t bl_deadline = HAL_GetTick() + 200u;
            while (HAL_GetTick() < bl_deadline) {
                HAL_IWDG_Refresh(&hiwdg);
                osDelay(20);
            }
            NVIC_SystemReset();
        }
        if (modbus_app_take_pending_reboot()) {
            const uint32_t deadline = HAL_GetTick() + 200u;
            while (HAL_GetTick() < deadline) {
                HAL_IWDG_Refresh(&hiwdg);
                osDelay(20);
            }
            NVIC_SystemReset();
        }

        tick += 100u;
        osDelayUntil(tick);
    }
}
