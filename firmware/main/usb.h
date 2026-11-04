#pragma once

// Starts TinyUSB (CDC + MSC + HID) and the CDC command task. Also provides hw_print/hw_key/hw_dfu for keymap.c.
void usb_init(void);
