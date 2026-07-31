/**
  ******************************************************************************
  * @file    modbus_tcp_server.c
  * @brief   Single-client Modbus TCP server task on top of LwIP netconn.
  *
  * At most one TCP client is served at a time. The listener is polled
  * non-blocking so a newly arriving connection preempts the current one
  * (newest-wins): this frees the single slot immediately when a master
  * reconnects after a cable pull / switch reboot, instead of waiting for the
  * stale half-open connection to time out. Link-down also drops the client.
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

/* Poll timing: the read timeout bounds how long a single server poll waits
 * for a new request to begin, so the accept loop can preempt with a newer
 * client (newest-wins) within this interval. The byte timeout bounds the gap
 * between bytes once a frame has started. */
#define MB_READ_TIMEOUT_MS        300u
#define MB_BYTE_TIMEOUT_MS       1000u

/* Drop a connected-but-silent client after this long with no valid request,
 * freeing the single slot (safety net alongside TCP keep-alive). */
#define MB_IDLE_DROP_MS         30000u

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
 * Active-client lifecycle helpers
 * ------------------------------------------------------------------------- */

/* Close and free the active client connection and release its RX buffer. */
static void client_close(struct netconn** conn, mb_io_t* io)
{
    if (io->inbuf != NULL) {
        netbuf_delete(io->inbuf);
        io->inbuf      = NULL;
        io->inbuf_data = NULL;
        io->inbuf_len  = 0;
        io->inbuf_pos  = 0;
    }
    if (*conn != NULL) {
        netconn_close(*conn);
        netconn_delete(*conn);
        *conn = NULL;
    }
    s_client_connected = 0u;
}

/* Configure keep-alive and build the nanoMODBUS server context for a freshly
 * accepted client. Returns false if the server context could not be created. */
static bool client_setup(struct netconn* conn, mb_io_t* io, nmbs_t* mb)
{
    /* Enable TCP keep-alive so a cable-pull is also detected at the stack
     * level (~16 s) even if the peer never reconnects. */
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);
    conn->pcb.tcp->keep_idle  = MB_KEEPALIVE_IDLE_MS;
    conn->pcb.tcp->keep_intvl = MB_KEEPALIVE_INTVL_MS;
    conn->pcb.tcp->keep_cnt   = MB_KEEPALIVE_CNT;

    memset(io, 0, sizeof(*io));
    io->conn = conn;

    /* Static so the conf outlives this call regardless of whether nanoMODBUS
     * copies it or keeps the pointer. arg points at the persistent io. */
    static nmbs_platform_conf platform;
    platform.transport  = NMBS_TRANSPORT_TCP;
    platform.read_byte  = mb_read_byte;
    platform.write_byte = mb_write_byte;
    platform.sleep      = mb_sleep;
    platform.arg        = io;

    if (nmbs_server_create(mb, settings_get()->modbus_slave_id,
                           &platform, modbus_app_get_callbacks()) != NMBS_ERROR_NONE) {
        return false;
    }
    nmbs_set_read_timeout(mb, MB_READ_TIMEOUT_MS);
    nmbs_set_byte_timeout(mb, MB_BYTE_TIMEOUT_MS);
    return true;
}

/* ---------------------------------------------------------------------------
 * Server task entry point
 *
 * Single loop, non-blocking accept: newly arriving connections preempt the
 * current client (newest-wins) so the one slot is freed immediately when a
 * master reconnects, and a reconnect storm cannot pile up in the backlog.
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
    /* Accept must never block the loop; we poll it every iteration. */
    netconn_set_nonblocking(listener, 1);

    struct netconn* active = NULL;
    mb_io_t         io      = { .conn = NULL };
    nmbs_t          mb;
    uint32_t        last_activity = 0u;

    for (;;) {
        /* 1. Drain the backlog, keeping only the NEWEST pending connection and
         *    closing any older queued ones (prevents reconnect-storm pile-up). */
        struct netconn* incoming = NULL;
        struct netconn* newest   = NULL;
        while (netconn_accept(listener, &incoming) == ERR_OK && incoming != NULL) {
            if (newest != NULL) {
                netconn_close(newest);
                netconn_delete(newest);
            }
            newest   = incoming;
            incoming = NULL;
        }
        if (newest != NULL) {
            client_close(&active, &io);            /* preempt the current client */
            if (client_setup(newest, &io, &mb)) {
                active         = newest;
                s_client_connected = 1u;
                last_activity  = osKernelGetTickCount();
            } else {
                netconn_close(newest);
                netconn_delete(newest);
            }
        }

        /* 2. Service the active client with one short, bounded poll. */
        if (active != NULL) {
            if (!g_eth_any_link_up) {
                client_close(&active, &io);        /* link down → free the slot */
                continue;
            }
            io.txbuf_len = 0u;
            const nmbs_error e = nmbs_server_poll(&mb);
            if (e == NMBS_ERROR_NONE) {
                mb_flush(&io);                     /* one TCP segment per response */
                modbus_app_notify_request();
                last_activity = osKernelGetTickCount();
            } else if (e == NMBS_ERROR_TIMEOUT) {
                if ((osKernelGetTickCount() - last_activity) >= MB_IDLE_DROP_MS) {
                    client_close(&active, &io);    /* silent peer → free the slot */
                }
            } else {
                client_close(&active, &io);        /* transport error → peer gone */
            }
        } else {
            osDelay(5);
        }
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
