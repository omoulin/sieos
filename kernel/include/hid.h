/*
 * hid.h - HID keyboards and pointers (USB and I2C): report descriptors,
 * input reports, key repeat.
 */
#ifndef SIEOS_HID_H
#define SIEOS_HID_H

#include "kernel.h"

#define HID_MAX_FIELDS 64
#define HID_MAX_USAGES 12
#define HID_MAX_KEYS   32

enum { HID_APP_OTHER, HID_APP_KEYBOARD, HID_APP_MOUSE };

/* One input item of the report descriptor: count values of size bits each. */
struct hid_field {
    uint8_t report_id;
    uint8_t app;                    /* the application collection it is in */
    uint8_t flags;                  /* bit 0 constant, 1 variable, 2 relative */
    uint8_t nusages;                /* 0: the usage range umin..umax */
    uint16_t page;
    uint16_t bit, size, count;      /* bit offset in the report (after the report ID) */
    int32_t lmin, lmax;
    uint16_t umin, umax;
    uint16_t usages[HID_MAX_USAGES];
};

struct hid {
    char name[24];                  /* for the log: "usb 1-3", "i2c 0-2c" */
    bool boot_kbd, boot_mouse;      /* the boot protocol's fixed reports */
    bool ids;                       /* the reports start with a report ID */
    bool has_kbd, has_mouse;
    bool debug;                     /* log every report */
    int nfields;
    struct hid_field f[HID_MAX_FIELDS];
    uint8_t keys[HID_MAX_KEYS];     /* the keyboard usages held */
    int nkeys;
    uint16_t rep_code;              /* key repeat: the set 1 code, the next time */
    uint64_t rep_at;
    unsigned buttons;
};

/* Parse a report descriptor into h (fields, has_kbd, has_mouse); < 0 if unusable. */
int  hid_parse(struct hid *h, const uint8_t *desc, size_t n);
/* One input report (with its report ID when h->ids), or a boot protocol report. */
void hid_input(struct hid *h, const uint8_t *r, size_t n);
/* Called every timer tick: key repeat. */
void hid_tick(struct hid *h);
/* The device went away: release what it holds. */
void hid_release(struct hid *h);

/* xhci.c: the USB controllers and their keyboards and pointers ("nousb", "usbdebug" on the command line). */
void usb_init(const char *cmdline);
void usb_poll(void);                /* from the timer tick */
bool usb_summary(char *buf, size_t n);
bool boot_option(const char *cmdline, const char *word);

/* i2c_hid.c: HID over I2C on the Intel LPSS controllers ("noi2c", "i2cdebug"). */
void i2c_hid_init(const char *cmdline);
void i2c_hid_poll(void);            /* from the timer tick */
bool i2c_hid_summary(char *buf, size_t n);

#endif
