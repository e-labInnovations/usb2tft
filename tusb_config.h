
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_ENABLED         1
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// A large receive FIFO lets the host keep streaming while a frame is being
// shifted out to the panel.  Transmit only carries one ack byte per frame.
#define CFG_TUD_CDC_RX_BUFSIZE  4096
#define CFG_TUD_CDC_TX_BUFSIZE  256

#endif
