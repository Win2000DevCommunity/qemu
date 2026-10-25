/*
 * QEMU EHCI Debug Port (DBGP) Emulation
 *
 * Implements the USB 2.0 EHCI Debug Port Capability (PCI_CAP_ID_DBG, 0x0A)
 * and the USB 2.0 Debug Device Protocol per EHCI Specification 1.0 Section 4.14.
 *
 * Connects the virtual EHCI debug port to a QEMU CharBackend (e.g., Named Pipe
 * or Socket) enabling kernel debugging with WinDbg without physical hardware.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_USB_EHCI_DBGP_H
#define HW_USB_EHCI_DBGP_H

#include "chardev/char-fe.h"
#include "system/memory.h"

/* ---- Layout in BAR 0 ---- */
#define EHCI_DBGP_OFFSET        0x80    /* Offset of debug registers in BAR 0 */
#define EHCI_DBGP_SIZE          0x20    /* 32 bytes register block */
#define EHCI_DBGP_PORT_NUM      1       /* Debug port is physical port 1 */

/* ---- Debug Port Register Offsets (relative to EHCI_DBGP_OFFSET) ---- */
#define DBGP_REG_CONTROL        0x00
#define DBGP_REG_PIDS           0x04
#define DBGP_REG_ADDRESS        0x08
#define DBGP_REG_DATA03         0x0C
#define DBGP_REG_DATA47         0x10

/* ---- Control Register Bits ---- */
#define DBGP_CTRL_OWNER         0x40000000
#define DBGP_CTRL_ENABLED       0x10000000
#define DBGP_CTRL_INUSE         0x08000000
#define DBGP_CTRL_DONE          0x00010000
#define DBGP_CTRL_OUT           0x00040000
#define DBGP_CTRL_GO            0x00080000
#define DBGP_CTRL_ERROR         0x00100000
#define DBGP_CTRL_LEN(x)       ((x) & 0x0F)
#define DBGP_CTRL_SET_LEN(c, l) (((c) & ~0x0F) | ((l) & 0x0F))
#define DBGP_CLAIM              (DBGP_CTRL_OWNER | DBGP_CTRL_ENABLED | DBGP_CTRL_INUSE)

/* ---- USB PIDs ---- */
#define DBGP_PID_OUT            0xE1
#define DBGP_PID_IN             0x69
#define DBGP_PID_SETUP          0x2D
#define DBGP_PID_DATA0          0xC3
#define DBGP_PID_DATA1          0x4B
#define DBGP_PID_ACK            0xD2
#define DBGP_PID_NAK            0x5A

#define DBGP_PID_GET(pids)        ((pids) & 0xFF)
#define DBGP_PID_SET(data, tok)   ((((uint32_t)(data)) << 8) | ((tok) & 0xFF))

/* ---- USB Debug Device Protocol Constants ---- */
#define USB_DEBUG_DEVNUM        127
#define DBGP_EP_IN              1
#define DBGP_EP_OUT             2

#define DBGP_FIFO_SIZE          4096

/* ---- Debug port capability DW layout (PCI config space) ----
 *
 *  Bits [31:29] = BAR number (0 for BAR 0)
 *  Bits [28:16] = Offset in BAR of debug registers
 *  Bits [15: 8] = Next capability pointer
 *  Bits [ 7: 0] = Capability ID (0x0A)
 */
#define EHCI_DBGP_CAP_OFFSET   0x50    /* PCI config offset for this cap */
#define EHCI_DBGP_CAP_SIZE     4       /* 4 bytes in PCI config space */

typedef struct EHCIDbgpState {
    CharFrontend chr;
    MemoryRegion mem;
    bool enabled;
    bool claimed;

    uint32_t control;
    uint32_t pids;
    uint32_t address;
    uint32_t data03;
    uint32_t data47;

    uint8_t dev_addr;           /* Current assigned address (0 initially, then 127) */
    bool debug_mode;

    /* Pending control transfer response */
    uint32_t ctrl_resp03;
    uint32_t ctrl_resp47;
    uint32_t ctrl_resp_len;

    /* Receive FIFO (Host -> Target via chardev) */
    uint8_t rx_fifo[DBGP_FIFO_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;

    /* Data toggle state */
    uint8_t toggle_in;
    uint8_t toggle_out;
} EHCIDbgpState;

void ehci_dbgp_init(EHCIDbgpState *s, Object *owner);
void ehci_dbgp_reset(EHCIDbgpState *s);

#endif /* HW_USB_EHCI_DBGP_H */
