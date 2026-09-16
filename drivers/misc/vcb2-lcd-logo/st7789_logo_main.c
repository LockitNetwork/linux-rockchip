// SPDX-License-Identifier: GPL-2.0
//
// st7789_logo.c - Minimal ST7789 logo display driver
//
// No framebuffer, no DRM. Probes as SPI device, sends init sequence,
// blasts logo pixel data, enables backlight. Then hands the SPI device over
// to another driver (spidev by default) so userspace can take over painting.
//
// Hardware (BananaPi M7 / RK3588):
//   SPI1.0  Mode3  40MHz
//   GPIO109 = RESET
//   GPIO115 = DC (data/command)
//   GPIO100 = Backlight
//   CS managed by SPI controller (CS0)

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

// logo.bin is a raw RGB565 big-endian file (240x240 = 115200 bytes).
// Generate with:  python3 img2bin.py logo.png logo.bin
// It is linked into the module via logo_data.S (assembly incbin).
#include "logo_data.h"

// ---------------------------------------------------------------------------
// ST7789 command set
// ---------------------------------------------------------------------------
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT 0x11
#define ST7789_NORON 0x13
#define ST7789_INVON 0x21
#define ST7789_DISPON 0x29
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_MADCTL 0x36
#define ST7789_COLMOD 0x3A
#define ST7789_PORCTRL 0xB2
#define ST7789_GCTRL 0xB7
#define ST7789_VCOMS 0xBB
#define ST7789_LCMCTRL 0xC0
#define ST7789_VDVVRHEN 0xC2
#define ST7789_VRHS 0xC3
#define ST7789_VDVS 0xC4
#define ST7789_FRCTRL2 0xC6
#define ST7789_PWCTRL1 0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

// MADCTL bits
#define MADCTL_MY BIT(7)
#define MADCTL_MX BIT(6)
#define MADCTL_MV BIT(5)
#define MADCTL_ML BIT(4)
#define MADCTL_BGR BIT(3)

// MX | MV: upright on the BananaPi M7 panel as mounted. (MY | MV, the
// periph.io ROTATION_270 value, renders the image rotated by 180°.)
#define ST7789_MADCTL_ROTATION (MADCTL_MX | MADCTL_MV)

// Display geometry
#define WIDTH 240
#define HEIGHT 240

// Offset of the visible 240x240 area inside the controller's 240x320 frame
// memory, in the rotated (post-MADCTL) coordinate system; with MV set, the
// 320-long axis is X. Measured on the panel: MY|MV needed X = 70, so the
// 180°-flipped MX|MV needs X = 320 - 240 - 70 = 10.
#define X_OFFSET 0
#define Y_OFFSET 0

// SPI chunk size for transfers (keep within controller limits); also the
// size of the DMA-safe bounce buffer
#define SPI_CHUNK 4096

// Driver that takes over the SPI device once the logo is painted; an empty
// string keeps st7789-logo bound (e.g. insmod st7789_logo.ko handoff_driver=).
static char *handoff_driver = "spidev";
module_param(handoff_driver, charp, 0444);
MODULE_PARM_DESC(
    handoff_driver,
    "driver to rebind the SPI device to after painting (empty: none)");

// Runs the handoffs; drained on module exit, so no work outlives the module.
static struct workqueue_struct *st7789_handoff_wq;

struct st7789_logo {
  struct spi_device *spi;
  struct gpio_desc *reset;
  struct gpio_desc *dc;
  struct gpio_desc *backlight;
  u8 *txbuf; // kmalloc'd, DMA-safe
};

// ---------------------------------------------------------------------------
// Low-level SPI helpers
// ---------------------------------------------------------------------------

// SPI transfers must use DMA-safe (kmalloc'd) memory: the controller switches
// to DMA for transfers larger than its FIFO. Stack variables and the logo in
// module .rodata are not DMA-safe, so every write is copied into d->txbuf.
static int st7789_write(struct st7789_logo *d, const u8 *buf, size_t len) {
  int ret;

  while (len > 0) {
    size_t chunk = min_t(size_t, len, SPI_CHUNK);

    memcpy(d->txbuf, buf, chunk);
    ret = spi_write(d->spi, d->txbuf, chunk);
    if (ret)
      return ret;
    buf += chunk;
    len -= chunk;
  }

  return 0;
}

static int st7789_cmd(struct st7789_logo *d, u8 cmd) {
  gpiod_set_value_cansleep(d->dc, 0);
  return st7789_write(d, &cmd, 1);
}

static int st7789_data(struct st7789_logo *d, const u8 *buf, size_t len) {
  gpiod_set_value_cansleep(d->dc, 1);
  return st7789_write(d, buf, len);
}

static int st7789_data_u8(struct st7789_logo *d, u8 val) {
  return st7789_data(d, &val, 1);
}

// ---------------------------------------------------------------------------
// Hardware reset
// ---------------------------------------------------------------------------

// reset-gpios is GPIO_ACTIVE_LOW in DT: logical 1 asserts reset (pin low),
// logical 0 releases it (pin high).
static void st7789_reset(struct st7789_logo *d) {
  gpiod_set_value_cansleep(d->reset, 0);
  msleep(10);
  gpiod_set_value_cansleep(d->reset, 1);
  msleep(10);
  gpiod_set_value_cansleep(d->reset, 0);
  msleep(120);
}

// ---------------------------------------------------------------------------
// Initialisation sequence
// Matches what the periph.io Go driver does for this display.
// ---------------------------------------------------------------------------

static int st7789_init_display(struct st7789_logo *d) {
  int ret;

  st7789_reset(d);

#define CMD(c)                                                                 \
  do {                                                                         \
    ret = st7789_cmd(d, c);                                                    \
    if (ret)                                                                   \
      return ret;                                                              \
  } while (0)
#define DAT(v)                                                                 \
  do {                                                                         \
    ret = st7789_data_u8(d, v);                                                \
    if (ret)                                                                   \
      return ret;                                                              \
  } while (0)

  CMD(ST7789_SWRESET);
  msleep(150);

  CMD(ST7789_SLPOUT);
  msleep(10);

  // Porch control
  CMD(ST7789_PORCTRL);
  DAT(0x0C);
  DAT(0x0C);
  DAT(0x00);
  DAT(0x33);
  DAT(0x33);

  // Gate control
  CMD(ST7789_GCTRL);
  DAT(0x35);

  // VCOMS
  CMD(ST7789_VCOMS);
  DAT(0x19);

  // LCM control
  CMD(ST7789_LCMCTRL);
  DAT(0x2C);

  // VDV/VRH enable
  CMD(ST7789_VDVVRHEN);
  DAT(0x01);

  // VRHS
  CMD(ST7789_VRHS);
  DAT(0x12);

  // VDVS
  CMD(ST7789_VDVS);
  DAT(0x20);

  // Frame rate 60 Hz (FRCTRL2)
  CMD(ST7789_FRCTRL2);
  DAT(0x0F);

  // Power control
  CMD(ST7789_PWCTRL1);
  DAT(0xA4);
  DAT(0xA1);

  // Positive gamma
  CMD(ST7789_PVGAMCTRL);
  DAT(0xD0);
  DAT(0x04);
  DAT(0x0D);
  DAT(0x11);
  DAT(0x13);
  DAT(0x2B);
  DAT(0x3F);
  DAT(0x54);
  DAT(0x4C);
  DAT(0x18);
  DAT(0x0D);
  DAT(0x0B);
  DAT(0x1F);
  DAT(0x23);

  // Negative gamma
  CMD(ST7789_NVGAMCTRL);
  DAT(0xD0);
  DAT(0x04);
  DAT(0x0C);
  DAT(0x11);
  DAT(0x13);
  DAT(0x2C);
  DAT(0x3F);
  DAT(0x44);
  DAT(0x51);
  DAT(0x2F);
  DAT(0x1F);
  DAT(0x1F);
  DAT(0x20);
  DAT(0x23);

  // 16-bit colour (RGB565)
  CMD(ST7789_COLMOD);
  DAT(0x55);

  // Memory access / rotation
  CMD(ST7789_MADCTL);
  DAT(ST7789_MADCTL_ROTATION);

  // Column address: X_OFFSET .. X_OFFSET + WIDTH - 1
  CMD(ST7789_CASET);
  DAT(X_OFFSET >> 8);
  DAT(X_OFFSET & 0xFF);
  DAT((X_OFFSET + WIDTH - 1) >> 8);
  DAT((X_OFFSET + WIDTH - 1) & 0xFF);

  // Row address: Y_OFFSET .. Y_OFFSET + HEIGHT - 1
  CMD(ST7789_RASET);
  DAT(Y_OFFSET >> 8);
  DAT(Y_OFFSET & 0xFF);
  DAT((Y_OFFSET + HEIGHT - 1) >> 8);
  DAT((Y_OFFSET + HEIGHT - 1) & 0xFF);

  CMD(ST7789_INVON);
  CMD(ST7789_NORON);
  msleep(10);
  CMD(ST7789_DISPON);
  msleep(10);

#undef CMD
#undef DAT

  return 0;
}

// ---------------------------------------------------------------------------
// Send logo pixel data
// ---------------------------------------------------------------------------

static int st7789_send_logo(struct st7789_logo *d) {
  int ret;

  ret = st7789_cmd(d, ST7789_RAMWR);
  if (ret)
    return ret;

  return st7789_data(d, logo_data, LOGO_DATA_SIZE); // from logo_data.h
}

// ---------------------------------------------------------------------------
// Handoff to another driver
// ---------------------------------------------------------------------------

// Detaching a driver from within its own probe() would deadlock on the device
// lock, so the switch runs from a work item after probe() has returned. The
// detach frees all devm allocations (including struct st7789_logo), hence the
// separately kmalloc'd context.
struct st7789_handoff {
  struct work_struct work;
  struct spi_device *spi; // holds a device reference
};

static void st7789_handoff_work(struct work_struct *work) {
  struct st7789_handoff *h = container_of(work, struct st7789_handoff, work);
  struct device *dev = &h->spi->dev;
  int ret;

  // Same as `echo <driver> > /sys/bus/spi/devices/spiX.Y/driver_override`:
  // spi_match_device() then matches by driver name only.
  ret = driver_set_override(dev, &h->spi->driver_override, handoff_driver,
                            strlen(handoff_driver));
  if (ret) {
    dev_err(dev, "handoff: setting driver_override failed: %d\n", ret);
    goto out;
  }

  // Detaches this driver (remove() leaves the panel as it is), then
  // rescans the bus. If the new driver is not registered yet, the device
  // stays unbound and is picked up via the override once it is.
  ret = device_reprobe(dev);
  if (ret)
    dev_err(dev, "handoff to %s failed: %d\n", handoff_driver, ret);
  else
    dev_info(dev, "handed off to %s\n", handoff_driver);

out:
  put_device(dev);
  kfree(h);
}

static int st7789_schedule_handoff(struct spi_device *spi) {
  struct st7789_handoff *h;

  if (!handoff_driver[0])
    return 0;

  h = kzalloc(sizeof(*h), GFP_KERNEL); // not devm: freed by the work itself
  if (!h)
    return -ENOMEM;

  h->spi = spi;
  get_device(&spi->dev);
  INIT_WORK(&h->work, st7789_handoff_work);
  queue_work(st7789_handoff_wq, &h->work);
  return 0;
}

// ---------------------------------------------------------------------------
// SPI driver probe / remove
// ---------------------------------------------------------------------------

static int st7789_logo_probe(struct spi_device *spi) {
  struct st7789_logo *d;
  int ret;

  d = devm_kzalloc(&spi->dev, sizeof(*d), GFP_KERNEL);
  if (!d)
    return -ENOMEM;

  d->txbuf = devm_kmalloc(&spi->dev, SPI_CHUNK, GFP_KERNEL);
  if (!d->txbuf)
    return -ENOMEM;

  d->spi = spi;
  spi_set_drvdata(spi, d);

  // SPI parameters — must match Mode3 @ 40 MHz
  spi->mode = SPI_MODE_3;
  spi->bits_per_word = 8;
  spi->max_speed_hz = 40000000;
  ret = spi_setup(spi);
  if (ret) {
    dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
    return ret;
  }

  // GPIOs
  d->reset = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
  if (IS_ERR(d->reset)) {
    dev_err(&spi->dev, "failed to get reset GPIO: %ld\n", PTR_ERR(d->reset));
    return PTR_ERR(d->reset);
  }

  d->dc = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
  if (IS_ERR(d->dc)) {
    dev_err(&spi->dev, "failed to get dc GPIO: %ld\n", PTR_ERR(d->dc));
    return PTR_ERR(d->dc);
  }

  d->backlight = devm_gpiod_get_optional(&spi->dev, "bl", GPIOD_OUT_LOW);
  if (IS_ERR(d->backlight)) {
    dev_err(&spi->dev, "failed to get backlight GPIO: %ld\n",
            PTR_ERR(d->backlight));
    return PTR_ERR(d->backlight);
  }

  ret = st7789_init_display(d);
  if (ret) {
    dev_err(&spi->dev, "display init failed: %d\n", ret);
    return ret;
  }

  ret = st7789_send_logo(d);
  if (ret) {
    dev_err(&spi->dev, "logo send failed: %d\n", ret);
    return ret;
  }

  if (d->backlight)
    gpiod_set_value_cansleep(d->backlight, 1);

  dev_info(&spi->dev, "logo displayed\n");

  // Not fatal: the logo is up, only userspace won't get its spidev node.
  ret = st7789_schedule_handoff(spi);
  if (ret)
    dev_warn(&spi->dev, "handoff not scheduled: %d\n", ret);

  return 0;
}

static void st7789_logo_remove(struct spi_device *spi) {
  // Intentionally leaves the panel alone: the logo and the backlight stay
  // on for the next owner. devm releases the GPIOs; the pins keep their
  // current levels.
}

// ---------------------------------------------------------------------------
// Device tree match table
// ---------------------------------------------------------------------------

static const struct of_device_id st7789_logo_of_match[] = {
    {.compatible = "viviana,st7789-logo"}, {}};
MODULE_DEVICE_TABLE(of, st7789_logo_of_match);

static const struct spi_device_id st7789_logo_id[] = {{"st7789-logo", 0}, {}};
MODULE_DEVICE_TABLE(spi, st7789_logo_id);

static struct spi_driver st7789_logo_driver = {
    .driver =
        {
            .name = "st7789-logo",
            .of_match_table = st7789_logo_of_match,
        },
    .id_table = st7789_logo_id,
    .probe = st7789_logo_probe,
    .remove = st7789_logo_remove,
};

static int __init st7789_logo_init(void) {
  int ret;

  st7789_handoff_wq = alloc_workqueue("st7789_logo_handoff", 0, 0);
  if (!st7789_handoff_wq)
    return -ENOMEM;

  ret = spi_register_driver(&st7789_logo_driver);
  if (ret)
    destroy_workqueue(st7789_handoff_wq);
  return ret;
}
module_init(st7789_logo_init);

static void __exit st7789_logo_exit(void) {
  spi_unregister_driver(&st7789_logo_driver);
  // Drains pending handoffs, so the work function can't outlive the module.
  destroy_workqueue(st7789_handoff_wq);
}
module_exit(st7789_logo_exit);

MODULE_AUTHOR("Viviana Cloud");
MODULE_DESCRIPTION("ST7789 minimal logo display — no framebuffer");
MODULE_LICENSE("GPL");
