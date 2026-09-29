/* i2c_host_test.h - host build of kernel/i2c_hid.c on a simulated controller (make i2c-test). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SIEOS_KERNEL_H
#define SIEOS_POLL_H
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define kprintf printf
#include "../kernel/include/hid.h"

struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision, irq;
    uint32_t bar[6];
};
uint64_t hrtime(void);
uint32_t sim_rd(uint32_t reg);
void sim_wr(uint32_t reg, uint32_t v);
int pci_count(void);
const struct pci_dev *pci_at(int i);
uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t after);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
uint64_t pci_bar_addr(const struct pci_dev *d, int i, bool *io);
void pci_claim(const struct pci_dev *d, const char *driver);
void *mmio_map(uint64_t pa, size_t size);
const void *acpi_table(const char *sig, int n, uint32_t *len);
void kbd_key(uint16_t code, bool release);
void input_mouse(int dx, int dy, unsigned buttons);
void input_mouse_abs(int x, int y, unsigned buttons);
