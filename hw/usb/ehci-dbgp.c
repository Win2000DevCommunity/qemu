/*
 * QEMU EHCI Debug Port (DBGP) Implementation
 *
 * Implements hardware state machine for EHCI Debug Port Specification 1.0.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "ehci-dbgp.h"
#include "qemu/log.h"

#define DBGP_TRACE(fmt, ...) \
    qemu_log_mask(LOG_UNIMP, "ehci-dbgp: " fmt "\n", ## __VA_ARGS__)

static void ehci_dbgp_handle_transaction(EHCIDbgpState *s)
{
    uint32_t token = DBGP_PID_GET(s->pids);
    uint32_t len = DBGP_CTRL_LEN(s->control);
    uint8_t ep = s->address & 0x0F;

    if (token == DBGP_PID_SETUP || (token == DBGP_PID_OUT && ep == 0)) {
        /* SETUP Stage: Parse Standard USB Control Request in Data03 / Data47 */
        uint8_t bRequest = (s->data03 >> 8) & 0xFF;
        uint16_t wValue = (s->data03 >> 16) & 0xFFFF;

        if (bRequest == 0x06) {
            /* GET_DESCRIPTOR: Check if querying USB_DT_DEBUG (0x0A) */
            uint8_t descType = (wValue >> 8) & 0xFF;
            if (descType == 0x0A) {
                /* USB_DEBUG_DESCRIPTOR: Length=4, Type=0x0A, InEP=1, OutEP=2 */
                s->ctrl_resp03 = (DBGP_EP_OUT << 24) | (DBGP_EP_IN << 16) |
                                 (0x0A << 8) | 0x04;
                s->ctrl_resp47 = 0;
                s->ctrl_resp_len = 4;
                s->data03 = s->ctrl_resp03;
                s->data47 = 0;
                s->control = DBGP_CTRL_SET_LEN(s->control, 4);
                s->pids = DBGP_PID_SET(DBGP_PID_DATA0, DBGP_PID_ACK);
                DBGP_TRACE("GET_DESCRIPTOR(Debug): returned ep_in=%d ep_out=%d",
                           DBGP_EP_IN, DBGP_EP_OUT);
            } else {
                s->ctrl_resp_len = 0;
                s->pids = DBGP_PID_SET(DBGP_PID_DATA0, DBGP_PID_ACK);
            }
        } else if (bRequest == 0x05) {
            /* SET_ADDRESS */
            s->dev_addr = wValue & 0x7F;
            s->ctrl_resp_len = 0;
            s->pids = DBGP_PID_SET(DBGP_PID_DATA0, DBGP_PID_ACK);
            DBGP_TRACE("SET_ADDRESS(%d)", s->dev_addr);
        } else if (bRequest == 0x03) {
            /* SET_FEATURE (DEBUG_MODE = 6) */
            if (wValue == 6) {
                s->debug_mode = true;
                DBGP_TRACE("SET_FEATURE(DEBUG_MODE) - debug mode active");
            }
            s->ctrl_resp_len = 0;
            s->pids = DBGP_PID_SET(DBGP_PID_DATA0, DBGP_PID_ACK);
        } else {
            /* Generic ACK */
            s->ctrl_resp_len = 0;
            s->pids = DBGP_PID_SET(DBGP_PID_DATA0, DBGP_PID_ACK);
        }

        s->control |= DBGP_CTRL_DONE;
        s->control &= ~DBGP_CTRL_GO;
        return;
    }

    if (token == DBGP_PID_OUT) {
        if (ep == 0) {
            /* Control transfer status / out stage */
            s->pids = DBGP_PID_SET(s->toggle_out, DBGP_PID_ACK);
            s->control |= DBGP_CTRL_DONE;
            s->control &= ~DBGP_CTRL_GO;
            return;
        }

        /* Bulk OUT Stage: Target VM transmits bytes to Host Debugger */
        uint8_t buf[8];
        uint32_t i;

        if (len > 8) {
            len = 8;
        }

        for (i = 0; i < 4 && i < len; i++) {
            buf[i] = (s->data03 >> (i * 8)) & 0xFF;
        }
        for (i = 4; i < 8 && i < len; i++) {
            buf[i] = (s->data47 >> ((i - 4) * 8)) & 0xFF;
        }

        if (qemu_chr_fe_backend_connected(&s->chr) && len > 0) {
            qemu_chr_fe_write_all(&s->chr, buf, len);
        }

        s->pids = DBGP_PID_SET(s->toggle_out, DBGP_PID_ACK);
        s->toggle_out ^= (DBGP_PID_DATA0 ^ DBGP_PID_DATA1);
        s->control |= DBGP_CTRL_DONE;
        s->control &= ~DBGP_CTRL_GO;
        return;
    }

    if (token == DBGP_PID_IN) {
        if (ep == 0) {
            /* Control IN Stage: Return descriptor or status */
            if (s->ctrl_resp_len > 0) {
                s->data03 = s->ctrl_resp03;
                s->data47 = s->ctrl_resp47;
                s->control = DBGP_CTRL_SET_LEN(s->control, s->ctrl_resp_len);
                s->pids = DBGP_PID_SET(s->toggle_in, DBGP_PID_ACK);
                s->ctrl_resp_len = 0;
            } else {
                s->control = DBGP_CTRL_SET_LEN(s->control, 0);
                s->pids = DBGP_PID_SET(s->toggle_in, DBGP_PID_ACK);
            }
            s->control |= DBGP_CTRL_DONE;
            s->control &= ~DBGP_CTRL_GO;
            return;
        }

        /* Bulk IN Stage: Target VM receives bytes from Host Debugger */
        if (s->rx_count > 0) {
            uint8_t buf[8] = { 0 };
            uint32_t copy_len = len;
            uint32_t i;

            if (copy_len > s->rx_count) {
                copy_len = s->rx_count;
            }
            if (copy_len > 8) {
                copy_len = 8;
            }

            for (i = 0; i < copy_len; i++) {
                buf[i] = s->rx_fifo[s->rx_head];
                s->rx_head = (s->rx_head + 1) % DBGP_FIFO_SIZE;
                s->rx_count--;
            }

            s->data03 = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
            s->data47 = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);

            s->control = DBGP_CTRL_SET_LEN(s->control, copy_len);
            s->pids = DBGP_PID_SET(s->toggle_in, DBGP_PID_ACK);
            s->toggle_in ^= (DBGP_PID_DATA0 ^ DBGP_PID_DATA1);
        } else {
            /* FIFO empty: Return ACK with length 0 so target polling returns CP_GET_NODATA cleanly */
            s->control = DBGP_CTRL_SET_LEN(s->control, 0);
            s->pids = DBGP_PID_SET(s->toggle_in, DBGP_PID_ACK);
        }

        s->control |= DBGP_CTRL_DONE;
        s->control &= ~DBGP_CTRL_GO;
        return;
    }

    /* Fallback */
    s->control |= DBGP_CTRL_DONE;
    s->control &= ~DBGP_CTRL_GO;
}

static uint64_t ehci_dbgp_read(void *opaque, hwaddr addr, unsigned size)
{
    EHCIDbgpState *s = opaque;

    switch (addr) {
    case DBGP_REG_CONTROL:
        return s->control;
    case DBGP_REG_PIDS:
        return s->pids;
    case DBGP_REG_ADDRESS:
        return s->address;
    case DBGP_REG_DATA03:
        return s->data03;
    case DBGP_REG_DATA47:
        return s->data47;
    default:
        return 0;
    }
}

static void ehci_dbgp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    EHCIDbgpState *s = opaque;
    uint32_t val32 = (uint32_t)val;

    switch (addr) {
    case DBGP_REG_CONTROL:
        /* Bit 16 (DONE) is write-1-to-clear */
        if (val32 & DBGP_CTRL_DONE) {
            s->control &= ~DBGP_CTRL_DONE;
        }

        /* Update control flags, preserving hardware state */
        s->control = (s->control & DBGP_CTRL_DONE) |
                     (val32 & ~DBGP_CTRL_DONE);

        /* If DBGP_CTRL_GO is asserted, execute transaction */
        if (s->control & DBGP_CTRL_GO) {
            ehci_dbgp_handle_transaction(s);
        }
        break;

    case DBGP_REG_PIDS:
        s->pids = val32;
        break;

    case DBGP_REG_ADDRESS:
        s->address = val32;
        break;

    case DBGP_REG_DATA03:
        s->data03 = val32;
        break;

    case DBGP_REG_DATA47:
        s->data47 = val32;
        break;

    default:
        break;
    }
}

static const MemoryRegionOps ehci_dbgp_ops = {
    .read = ehci_dbgp_read,
    .write = ehci_dbgp_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int ehci_dbgp_can_receive(void *opaque)
{
    EHCIDbgpState *s = opaque;
    return DBGP_FIFO_SIZE - s->rx_count;
}

static void ehci_dbgp_receive(void *opaque, const uint8_t *buf, int size)
{
    EHCIDbgpState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        if (s->rx_count >= DBGP_FIFO_SIZE) {
            break;
        }
        s->rx_fifo[s->rx_tail] = buf[i];
        s->rx_tail = (s->rx_tail + 1) % DBGP_FIFO_SIZE;
        s->rx_count++;
    }
}

void ehci_dbgp_init(EHCIDbgpState *s, Object *owner)
{
    memory_region_init_io(&s->mem, owner, &ehci_dbgp_ops, s,
                          "ehci-dbgp", EHCI_DBGP_SIZE);
    qemu_chr_fe_set_handlers(&s->chr,
                             ehci_dbgp_can_receive,
                             ehci_dbgp_receive,
                             NULL, NULL, s, NULL, true);
    ehci_dbgp_reset(s);
}

void ehci_dbgp_reset(EHCIDbgpState *s)
{
    s->enabled = true;
    s->claimed = false;
    s->control = DBGP_CTRL_ENABLED | DBGP_CTRL_OWNER;
    s->pids = 0;
    s->address = 0;
    s->data03 = 0;
    s->data47 = 0;
    s->dev_addr = 0;
    s->debug_mode = false;
    s->ctrl_resp03 = 0;
    s->ctrl_resp47 = 0;
    s->ctrl_resp_len = 0;
    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_count = 0;
    s->toggle_in = DBGP_PID_DATA0;
    s->toggle_out = DBGP_PID_DATA1;
}
