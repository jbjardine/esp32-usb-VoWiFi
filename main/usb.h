#ifndef USB_H_
#define USB_H_

#include <stdbool.h>

void usb_request_keypress_send(bool from_isr);
void usb_init(void);
bool usb_is_mounted(void);

#endif // USB_H_
