/**
  ******************************************************************************
  * @file    modbus_tcp_server.c
  * @brief   Single-client Modbus TCP server task on top of LwIP netconn.
  *
  * The implementation is intentionally simple: at any time at most one TCP
  * client is served. Additional incoming connections are immediately closed,
  * matching how typical single-channel Modbus slaves behave.
  ******************************************************************************
  */

#include "modbus_tcp_server.h"

#include <string.h>

#include "cmsis_os.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"

#include "modbus_app.h"
#include "nanomodbus.h"
#include "settings.h"

/* Physical link state from ethernet_link_thread. */
extern volatile uint8_t g_eth_any_link_up;

/* ---------------------------------------------------------------------------
 * IO context wrapping a netconn for the nanoMODBUS byte-callbacks.
 * ------------------------------------------------------------------------- */
#define MB_TX_BUF_SIZE  280u  /* max Modbus TCP frame: 7 MBAP + 253 PDU */

/* TCP keep-alive parameters (in milliseconds) */
#define MB_KEEPALIVE_IDLE_MS    10000u  /* 10 s idle before first probe  */
#define MB_KEEPALIVE_INTVL_MS    2000u  /*  2 s between probes           */
#define MB_KEEPALIVE_CNT            3u  /*  3 probes → dead after ~16 s  */

/* Max consecutive read-timeouts before we drop the connection (safety net
 * in case TCP keep-alive cannot detect the failure). */
#define MB_MAX_IDLE_TIMEOUTS        6u  /* 6 × 5 s = 30 s */

typedef struct {
    struct netconn* conn;
    struct netbuf*  inbuf;
    char*           inbuf_data;
    u16_t           inbuf_len;
    u16_t           inbuf_pos;
    uint8_t         txbuf[MB_TX_BUF_SIZE];
    u16_t           txbuf_len;
} mb_io_t;

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */
static volatile uint8_t s_client_connected = 0u;
static osThreadId_t     s_server_task      = NULL;

bool modbus_tcp_server_has_client(void)
{
    return s_client_connected != 0u;
}

/* ---------------------------------------------------------------------------
 * nanoMODBUS platform callbacks
 * ------------------------------------------------------------------------- */
static int mb_read_byte(uint8_t* b, int32_t timeout_ms, void* arg)
{
    mb_io_t* io = (mb_io_t*)arg;

    if (io->inbuf == NULL || io->inbuf_pos >= io->inbuf_len) {
        if (io->inbuf != NULL) {
            netbuf_delete(io->inbuf);
            io->inbuf      = NULL;
            io->inbuf_data = NULL;
            io->inbuf_len  = 0;
            io->inbuf_pos  = 0;
        }

        netconn_set_recvtimeout(io->conn,
                                (timeout_ms < 0) ? 0 : (u32_t)timeout_ms);

        const err_t err = netconn_recv(io->conn, &io->inbuf);
        if (err == ERR_TIMEOUT) {
            return 0;
        }
        if (err != ERR_OK) {
            return -1;
        }

        netbuf_data(io->inbuf, (void**)&io->inbuf_data, &io->inbuf_len);
        io->inbuf_pos = 0;
    }

    *b = (uint8_t)io->inbuf_data[io->inbuf_pos++];
    return 1;
}

/* Buffer bytes instead of sending one-by-one.  The complete response is
 * flushed to TCP after nmbs_server_poll() returns (see handle_client). */
static int mb_write_byte(uint8_t b, int32_t timeout_ms, void* arg)
{
    (void)timeout_ms;
    mb_io_t* io = (mb_io_t*)arg;
    if (io->txbuf_len >= MB_TX_BUF_SIZE) {
        return -1;  /* buffer overflow — should never happen */
    }
    io->txbuf[io->txbuf_len++] = b;
    return 1;
}

/* Flush the buffered TX data as a single TCP segment. */
static int mb_flush(mb_io_t* io)
{
    if (io->txbuf_len == 0u) {
        return 0;
    }
    const err_t err = netconn_write(io->conn, io->txbuf, io->txbuf_len,
                                    NETCONN_COPY);
    io->txbuf_len = 0u;
    return (err == ERR_OK) ? 0 : -1;
}

static void mb_sleep(uint32_t ms, void* arg)
{
    (void)arg;
    osDelay(ms);
}

/* ---------------------------------------------------------------------------
 * Per-connection handler
 * ------------------------------------------------------------------------- */
static void handle_client(struct netconn* newconn)
{
    /* Enable TCP keep-alive so a cable-pull is detected within ~16 s. */
    ip_set_option(newconn->pcb.tcp, SOF_KEEPALIVE);
    newconn->pcb.tcp->keep_idle  = MB_KEEPALIVE_IDLE_MS;
    newconn->pcb.tcp->keep_intvl = MB_KEEPALIVE_INTVL_MS;
    newconn->pcb.tcp->keep_cnt   = MB_KEEPALIVE_CNT;

    mb_io_t io = {
        .conn       = newconn,
        .inbuf      = NULL,
        .inbuf_data = NULL,
        .inbuf_len  = 0,
        .inbuf_pos  = 0,
        .txbuf_len  = 0,
    };

    nmbs_platform_conf platform = {
        .transport  = NMBS_TRANSPORT_TCP,
        .read_byte  = mb_read_byte,
        .write_byte = mb_write_byte,
        .sleep      = mb_sleep,
        .arg        = &io,
    };

    nmbs_t mb;
    if (nmbs_server_create(&mb, settings_get()->modbus_slave_id,
                           &platform, modbus_app_get_callbacks()) != NMBS_ERROR_NONE) {
        return;
    }

    /* nmbs_server_poll() returns once it has handled (or failed to handle)
     * a single request. We keep going until the connection drops. */
    nmbs_set_read_timeout(&mb, 5000);
    nmbs_set_byte_timeout(&mb, 1000);

    s_client_connected = 1u;

    uint32_t idle_timeouts = 0u;

    for (;;) {
        io.txbuf_len = 0u;  /* reset TX buffer before each poll */
        const nmbs_error e = nmbs_server_poll(&mb);
        if (e == NMBS_ERROR_NONE) {
            mb_flush(&io);   /* send complete response in one TCP segment */
            modbus_app_notify_request();
            idle_timeouts = 0u;
            continue;
        }
        if (e == NMBS_ERROR_TIMEOUT) {
            idle_timeouts++;
            if (idle_timeouts >= MB_MAX_IDLE_TIMEOUTS || !g_eth_any_link_up) {
                break;  /* 30 s idle → force-close stale connection */
            }
            continue;
        }
        /* Transport error or anything else: the connection is gone. */
        break;
    }

    s_client_connected = 0u;

    if (io.inbuf != NULL) {
        netbuf_delete(io.inbuf);
    }
}

/* ---------------------------------------------------------------------------
 * Server task entry point
 * ------------------------------------------------------------------------- */
static void modbus_tcp_server_thread(void* arg)
{
    (void)arg;

    struct netconn* listener = netconn_new(NETCONN_TCP);
    if (listener == NULL) {
        for (;;) { osDelay(1000); }
    }

    const uint16_t port = settings_get()->modbus_tcp_port;
    if (netconn_bind(listener, IP_ADDR_ANY, port) != ERR_OK) {
        netconn_delete(listener);
        for (;;) { osDelay(1000); }
    }

    netconn_listen(listener);

    for (;;) {
        struct netconn* newconn = NULL;
        if (netconn_accept(listener, &newconn) != ERR_OK || newconn == NULL) {
            osDelay(10);
            continue;
        }
        handle_client(newconn);
        netconn_close(newconn);
        netconn_delete(newconn);
    }
}

/* ---------------------------------------------------------------------------
 * Public start
 * ------------------------------------------------------------------------- */
void modbus_tcp_server_start(void)
{
    if (s_server_task != NULL) {
        return;
    }
    const osThreadAttr_t attr = {
        .name       = "ModbusSrv",
        .stack_size = 2048,
        .priority   = osPriorityNormal,
    };
    s_server_task = osThreadNew(modbus_tcp_server_thread, NULL, &attr);
}
