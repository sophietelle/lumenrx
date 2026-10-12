// SPDX-License-Identifier: GPL-2.0
#ifdef __KERNEL__
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#else
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define div_u64(a, b) ((a) / (b))
#define fallthrough __attribute__((fallthrough))
#endif

enum fmt { INTUOS, INTUOS_V1, INTUOS_V2, INTUOS_V3, BAMBOO };

struct tablet {
  const char *name;
  int max_x, max_y; /* device units */
  int width_um, height_um;
  u8 fmt, ninit, init[2][2];
};

struct pen {
  int x, y, pressure, buttons;
};

#define LE16(i) (r[i] | r[(i) + 1] << 8)

static __always_inline int parse(int fmt, const u8 *r, int size,
                                 struct pen *p) {
  switch (fmt) {
  case INTUOS:
    if (size < 8 || r[0] != 0x02)
      return 0;
    if (!(r[1] & 0x40))
      break;
    p->x = LE16(2);
    p->y = LE16(4);
    p->pressure = LE16(6);
    p->buttons = r[1] >> 1 & 3;
    return 1;
  case BAMBOO:
    if (size < 8 || r[0] != 0x02 || r[1] & 0x40) /* 0x40: mouse puck */
      return 0;
    if (!(r[1] & 0x80))
      break;
    p->x = LE16(2);
    p->y = LE16(4);
    p->pressure = r[1] & 1 ? r[6] | (r[7] & 3) << 8 : 0;
    p->buttons = r[1] >> 1 & 3;
    return 1;
  case INTUOS_V1:
    if (size < 10 || (r[0] != 0x02 && r[0] != 0x10) ||
        (r[0] == 0x10 && r[1] == 0x20))
      return 0;
    if (r[1] == 0x80)
      break;
    if ((r[1] & 0x0a) != 0x0a) { /* 0x0a: rotation report, position only */
      if (!(r[1] & 0x20))
        return 0;
      p->pressure = r[6] << 3 | (r[7] & 0xc0) >> 5 | (r[1] & 1);
      p->buttons = r[1] >> 1 & 3;
    }
    p->x = (r[2] << 8 | r[3]) << 1 | (r[9] >> 1 & 1);
    p->y = (r[4] << 8 | r[5]) << 1 | (r[9] & 1);
    return 1;
  case INTUOS_V3:
    if (size >= 9 && r[0] == 0x1f && r[1] == 0x01) {
      p->x = LE16(3);
      p->y = LE16(5);
      p->pressure = LE16(7);
      p->buttons = r[2] >> 1 & 3;
      return 1;
    }
    if (size < 11 || r[0] != 0x1e)
      return 0;
    fallthrough;
  case INTUOS_V2:
    if (size < 10)
      return 0;
    if (r[0] == 0x10) {
      if (!(r[1] & 0x20))
        break;
    } else if (r[0] == 0x1e && size >= 11) {
      r++; /* same layout but shifted one byte */
    } else {
      return 0;
    }
    p->x = LE16(2) | r[4] << 16;
    p->y = LE16(5) | r[7] << 16;
    p->pressure = LE16(8);
    p->buttons = r[1] >> 1 & 3;
    return 1;
  }

  /* pen unreachable */
  if (!(p->pressure | p->buttons))
    return 0;
  p->pressure = p->buttons = 0;
  return 1;
}

struct axis {
  int negate; /* 0 or -1; in ^ -1 is -in - 1 */
  int shift;
  int size;
};

struct xform {
  int swap;
  struct axis x, y;
};

static u64 um_to_units(unsigned int um, int tablet_units, int tablet_um) {
  return div_u64((u64)um * tablet_units, tablet_um);
}

static int area_start(unsigned int centre_um, int area, int tablet,
                      int tablet_um) {
  int max_start = tablet - area;
  u64 centre;

  if (!centre_um)
    return max_start / 2;
  centre = um_to_units(centre_um, tablet, tablet_um);
  if (centre < (u64)area / 2)
    return 0;
  if (centre - area / 2 > (u64)max_start)
    return max_start;
  return centre - area / 2;
}

static int axis_init(struct axis *a, int tablet, int tablet_um,
                     unsigned int area_um, unsigned int centre_um,
                     int reversed) {
  u64 area = area_um ? um_to_units(area_um, tablet, tablet_um) : (u64)tablet;
  int start;

  if (area > (u64)tablet)
    return -1;
  a->size = area;
  start = area_start(centre_um, a->size, tablet, tablet_um);
  a->negate = reversed ? -1 : 0;
  a->shift = reversed ? start + a->size + 1 : -start;
  return 0;
}

static int xform_init(struct xform *f, const struct tablet *t,
                      unsigned int w_um, unsigned int h_um, unsigned int x_um,
                      unsigned int y_um, unsigned int rot) {
  int sideways = rot & 1; /* quarter turns */
  int x_reversed = rot == 2 || rot == 3;
  int y_reversed = rot == 1 || rot == 2;

  if (rot > 3)
    return -1;
  f->swap = sideways;
  if (sideways)
    return axis_init(&f->x, t->max_y, t->height_um, w_um, y_um, x_reversed) ||
           axis_init(&f->y, t->max_x, t->width_um, h_um, x_um, y_reversed);
  return axis_init(&f->x, t->max_x, t->width_um, w_um, x_um, x_reversed) ||
         axis_init(&f->y, t->max_y, t->height_um, h_um, y_um, y_reversed);
}

static __always_inline int axis_apply(const struct axis *a, int in) {
  int out = (in ^ a->negate) + a->shift;

  if (out > a->size)
    out = a->size;
  if (out < 0)
    out = 0;
  return out;
}

static __always_inline void xform(const struct xform *f, int pen_x, int pen_y,
                                  int *x, int *y) {
  *x = axis_apply(&f->x, f->swap ? pen_y : pen_x);
  *y = axis_apply(&f->y, f->swap ? pen_x : pen_y);
}

#ifdef __KERNEL__

enum { CFG_W, CFG_H, CFG_X, CFG_Y, CFG_ROT, CFG_PRESSURE };

struct lumenrx {
  struct input_dev *input;
  struct xform f;
  struct pen pen;
  int x, y;    /* last sent */
  u8 btn, fmt; /* btn: last sent; bit 0 tip, bits 1-2 pen buttons */
  u16 click;   /* raw pressure above this is a click */
  /* raw_event never reads past here */
  unsigned int cfg[CFG_PRESSURE]; /* what f was built from */
} ____cacheline_aligned;
static_assert(offsetof(struct lumenrx, cfg) == SMP_CACHE_BYTES);

/* defaults for new devices */
static unsigned int area_w, area_h, area_x, area_y, rotation;
static unsigned short pressure;
module_param(area_w, uint, 0444);
MODULE_PARM_DESC(area_w, "area width in um, 0 = whole tablet");
module_param(area_h, uint, 0444);
MODULE_PARM_DESC(area_h, "area height in um, 0 = whole tablet");
module_param(area_x, uint, 0444);
MODULE_PARM_DESC(area_x, "area centre in um, 0 = tablet centre");
module_param(area_y, uint, 0444);
MODULE_PARM_DESC(area_y, "area centre in um, 0 = tablet centre");
module_param(rotation, uint, 0444);
MODULE_PARM_DESC(rotation, "tablet rotation: 0-3 for 0, 90, 180, 270 deg");
module_param(pressure, ushort, 0444);
MODULE_PARM_DESC(pressure, "raw pressure for a click");

/*
 * a negative return makes hid-core skip hid_report_raw_event(), which has
 * nothing left to do for this device
 */
#define DONE (-ENODATA)

static int lumenrx_raw_event(struct hid_device *hdev, struct hid_report *report,
                             u8 *data, int size) {
  struct lumenrx *lx = hid_get_drvdata(hdev);
  struct pen p = lx->pen;
  int x, y, btn, m;

  if (!parse(lx->fmt, data, size, &p))
    return DONE;
  lx->pen = p;
  xform(&lx->f, p.x, p.y, &x, &y);
  btn = (p.pressure > lx->click) | p.buttons << 1;
  /* m: bit 0 x moved, bit 1 y moved, bits 2-4 the buttons that flipped */
  m = (x != lx->x) | (y != lx->y) << 1 | (btn ^ lx->btn) << 2;
  if (!m)
    return DONE;
  lx->x = x;
  lx->y = y;
  lx->btn = btn;

  /* Values are re-read from lx so that only lx and m stay live across the
   * calls. */
  if (m & 1)
    input_event(lx->input, EV_ABS, ABS_X, lx->x);
  if (m & 2)
    input_event(lx->input, EV_ABS, ABS_Y, lx->y);
  if (unlikely(m >> 2)) {
    if (m & 4)
      input_event(lx->input, EV_KEY, BTN_LEFT, lx->btn & 1);
    if (m & 8)
      input_event(lx->input, EV_KEY, BTN_RIGHT, lx->btn >> 1 & 1);
    if (m & 16)
      input_event(lx->input, EV_KEY, BTN_MIDDLE, lx->btn >> 2);
  }
  input_event(lx->input, EV_SYN, SYN_REPORT, 0);
  return DONE;
}

#define TABLET(pid, ...)                                                       \
  {HID_USB_DEVICE(0x056a, pid),                                                \
   .driver_data = (kernel_ulong_t) & (const struct tablet){__VA_ARGS__}},
static const struct hid_device_id lumenrx_ids[] = {
#include "tablets.h"
    {}};
MODULE_DEVICE_TABLE(hid, lumenrx_ids);

/* tablets boot emulating a mouse, and fall back to that across suspend. */
static int lumenrx_set_mode(struct hid_device *hdev) {
  const struct tablet *t =
      (const void *)hid_match_id(hdev, lumenrx_ids)->driver_data;
  int i, ret;

  for (i = 0; i < t->ninit; i++) {
    u8 *buf = kmemdup(t->init[i], 2, GFP_KERNEL); /* USB wants heap memory */

    if (!buf)
      return -ENOMEM;
    ret = hid_hw_raw_request(hdev, buf[0], buf, 2, HID_FEATURE_REPORT,
                             HID_REQ_SET_REPORT);
    kfree(buf);
    if (ret < 0)
      hid_warn(hdev, "init report %d failed: %d\n", i, ret);
  }
  return 0;
}

static int lumenrx_apply(struct hid_device *hdev, struct lumenrx *lx) {
  const struct tablet *t =
      (const void *)hid_match_id(hdev, lumenrx_ids)->driver_data;

  int max[2] = {t->max_x, t->max_y}, um[2] = {t->width_um, t->height_um};
  const unsigned int *c = lx->cfg;
  struct input_dev *input;
  struct xform f;
  int ret;

  if (xform_init(&f, t, c[CFG_W], c[CFG_H], c[CFG_X], c[CFG_Y], c[CFG_ROT])) {
    hid_err(hdev, "bad rotation, or area larger than the %dx%d um tablet\n",
            t->width_um, t->height_um);
    return -EINVAL;
  }

  input = input_allocate_device();
  if (!input)
    return -ENOMEM;

  input->name = t->name;
  input->phys = hdev->phys;
  input->dev.parent = &hdev->dev;
  input->id.bustype =
      BUS_VIRTUAL; /* with Wacom's ids userspace expects a tablet */

  input_set_capability(input, EV_KEY, BTN_LEFT);
  input_set_capability(input, EV_KEY, BTN_RIGHT);
  input_set_capability(input, EV_KEY, BTN_MIDDLE);
  input_set_abs_params(input, ABS_X, 0, f.x.size, 0, 0);
  input_set_abs_params(input, ABS_Y, 0, f.y.size, 0, 0);
  input_abs_set_res(input, ABS_X,
                    max[f.swap] * 1000 / um[f.swap]); /* units per mm */
  input_abs_set_res(input, ABS_Y, max[!f.swap] * 1000 / um[!f.swap]);

  ret = input_register_device(input);
  if (ret) {
    input_free_device(input);
    return ret;
  }

  if (lx->input)
    input_unregister_device(lx->input);

  lx->input = input;
  lx->f = f;
  lx->x = lx->y = -1; /* the new device has seen nothing yet */
  lx->btn = 0;

  hid_info(hdev, "%s, area %dx%d units, rotation %u\n", t->name, f.x.size,
           f.y.size, c[CFG_ROT]);

  return 0;
}

static ssize_t cfg_show(struct device *dev, struct device_attribute *attr,
                        char *buf);
static ssize_t cfg_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count);
#define CFG_ATTR(name) __ATTR(name, 0644, cfg_show, cfg_store)
static struct device_attribute cfg_attrs[] = {
    [CFG_W] = CFG_ATTR(area_w),     [CFG_H] = CFG_ATTR(area_h),
    [CFG_X] = CFG_ATTR(area_x),     [CFG_Y] = CFG_ATTR(area_y),
    [CFG_ROT] = CFG_ATTR(rotation), [CFG_PRESSURE] = CFG_ATTR(pressure)};
static struct attribute *lumenrx_attrs[] = {&cfg_attrs[CFG_W].attr,
                                            &cfg_attrs[CFG_H].attr,
                                            &cfg_attrs[CFG_X].attr,
                                            &cfg_attrs[CFG_Y].attr,
                                            &cfg_attrs[CFG_ROT].attr,
                                            &cfg_attrs[CFG_PRESSURE].attr,
                                            NULL};
ATTRIBUTE_GROUPS(lumenrx);

static ssize_t cfg_show(struct device *dev, struct device_attribute *attr,
                        char *buf) {
  struct lumenrx *lx = dev_get_drvdata(dev);
  int i = attr - cfg_attrs;

  return sysfs_emit(buf, "%u\n", i == CFG_PRESSURE ? lx->click : lx->cfg[i]);
}

static ssize_t cfg_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count) {
  struct hid_device *hdev = to_hid_device(dev);
  struct lumenrx *lx = hid_get_drvdata(hdev);
  int i = attr - cfg_attrs, ret;
  unsigned int v, old;

  ret = kstrtouint(buf, 0, &v);
  if (ret)
    return ret;

  if (i == CFG_PRESSURE) {
    if (v > U16_MAX)
      return -EINVAL;
    lx->click = v;
    return count;
  }

  /* hid-core drops reports while this is held, so raw_event stays out */
  down(&hdev->driver_input_lock);
  old = lx->cfg[i];
  lx->cfg[i] = v;
  ret = lumenrx_apply(hdev, lx);
  if (ret)
    lx->cfg[i] = old;
  up(&hdev->driver_input_lock);
  return ret ?: count;
}

/* runs after remove() or after a failed probe */
static void lumenrx_free(void *data) {
  struct lumenrx *lx = data;

  if (lx->input)
    input_unregister_device(lx->input);
  kfree(lx);
}

static int lumenrx_probe(struct hid_device *hdev,
                         const struct hid_device_id *id) {
  const struct tablet *t = (const void *)id->driver_data;
  struct lumenrx *lx;
  int ret;

  /* the pen is usually on usb interface 0 */
  if (!hid_is_usb(hdev) || to_usb_interface(hdev->dev.parent)
                                   ->cur_altsetting->desc.bInterfaceNumber != 0)
    return -ENODEV;

  /* not devm_kzalloc: its header would push the struct across two cache lines.
   */
  lx = kzalloc(sizeof(*lx), GFP_KERNEL);
  if (!lx)
    return -ENOMEM;
  ret = devm_add_action_or_reset(&hdev->dev, lumenrx_free, lx);
  if (ret)
    return ret;
  lx->cfg[CFG_W] = area_w;
  lx->cfg[CFG_H] = area_h;
  lx->cfg[CFG_X] = area_x;
  lx->cfg[CFG_Y] = area_y;
  lx->cfg[CFG_ROT] = rotation;
  lx->click = pressure;
  lx->fmt = t->fmt;
  hid_set_drvdata(hdev, lx);
  ret = lumenrx_apply(hdev, lx);
  if (ret)
    return ret;

  ret = hid_parse(hdev);
  if (ret)
    return ret;

  ret = hid_hw_start(hdev, 0);
  if (ret)
    return ret;

  lumenrx_set_mode(hdev);

  ret = hid_hw_open(hdev); /* start polling; nothing else will ask for it */
  if (ret) {
    hid_hw_stop(hdev);
    return ret;
  }

  return 0;
}

static void lumenrx_remove(struct hid_device *hdev) {
  hid_hw_close(hdev);
  hid_hw_stop(hdev);
}

static struct hid_driver lumenrx_driver = {
    .name = "lumenrx",
    .id_table = lumenrx_ids,
    .probe = lumenrx_probe,
    .remove = lumenrx_remove,
    .raw_event = lumenrx_raw_event,
    .driver = {.dev_groups = lumenrx_groups},
#ifdef CONFIG_PM
    .resume = lumenrx_set_mode,
    .reset_resume = lumenrx_set_mode,
#endif
};
module_hid_driver(lumenrx_driver);

MODULE_DESCRIPTION("Minimal low-latency Wacom pen driver");
MODULE_LICENSE("GPL");

#else /* parser test built as a binary */

static int at(const struct xform *f, int tx, int ty, int x, int y) {
  int ox, oy;

  xform(f, tx, ty, &ox, &oy);
  return ox == x && oy == y;
}

int main(void) {
  struct tablet t = {
      .max_x = 15200, .max_y = 9500, .width_um = 152000, .height_um = 95000};
  struct xform f;
  struct pen p = {0};
  u8 in[10] = {0x02, 0xe5, 0x34, 0x12, 0x78, 0x56, 0xff, 0x03},
     out[10] = {0x02, 0x80};
  u8 other[10] = {0x11};
  u8 v1[10] = {0x02, 0xe1, 0x57, 0x4c, 0x36, 0x92, 0xff, 0xc0, 0, 0x03};
  u8 v2[10] = {0x10, 0x23, 0x60, 0xaf, 0x00, 0xa0, 0x73, 0x00, 0xff, 0x0f};
  u8 ext[11] = {0x1e, 0x00, 0x23, 0x60, 0xaf, 0x00,
                0xa0, 0x73, 0x00, 0xff, 0x0f};
  int i;

  assert(xform_init(&f, &t, 0, 0, 0, 0, 4));
  assert(xform_init(&f, &t, 0, 0, 0, 0, 90));
  assert(xform_init(&f, &t, 153000, 95000, 0, 0, 0));
  assert(xform_init(&f, &t, 152000, 95000, 0, 0, 1));
  assert(xform_init(&f, &t, ~0u, ~0u, 0, 0, 0));

  assert(!xform_init(&f, &t, 100000, 50000, 0, 0, 0));
  assert(f.x.size == 10000 && f.y.size == 5000);
  assert(at(&f, 7600, 4750, 5000, 2500)); /* centre -> centre */
  assert(at(&f, 0, 9500, 0, 5000));       /* outside clamps */

  assert(!xform_init(&f, &t, 0, 0, 0, 0, 2));
  assert(at(&f, 0, 0, 15200, 9500) && at(&f, 15200, 9500, 0, 0));

  /* rotation as in otd, at 1 (90 deg) the tablet's top-right is 0,0 */
  assert(!xform_init(&f, &t, 0, 0, 0, 0, 1));
  assert(f.x.size == 9500 && f.y.size == 15200);
  assert(at(&f, 15200, 0, 0, 0) && at(&f, 0, 9500, 9500, 15200));

  assert(!xform_init(&f, &t, 50000, 100000, 0, 0, 3));
  assert(f.x.size == 5000 && f.y.size == 10000);
  assert(at(&f, 0, 9500, 0, 0)); /* bottom-left -> top-left clamped */
  assert(at(&f, 7600, 4750, 2500, 5000));

  assert(!xform_init(&f, &t, 100000, 50000, 60000, 30000, 0));
  assert(at(&f, 1000, 500, 0, 0) && at(&f, 6000, 3000, 5000, 2500));
  /* the centre is in the tablet's own frame, whatever the rotation */
  assert(!xform_init(&f, &t, 100000, 50000, 60000, 30000, 2));
  assert(at(&f, 11000, 5500, 0, 0) && at(&f, 6000, 3000, 5000, 2500));
  assert(!xform_init(&f, &t, 100000, 50000, 10000, ~0u, 0));
  assert(at(&f, 0, 4500, 0, 0) && at(&f, 10000, 9500, 10000, 5000));
  assert(!xform_init(&f, &t, 50000, 100000, 20000, 0, 1) &&
         at(&f, 10000, 2250, 0, 0));

  assert(!xform_init(&f, &t, 101600, 50800, 76200, 47500, 0));
  assert(f.x.size == 10160 && at(&f, 2540, 2210, 0, 0));

  assert(!parse(INTUOS, out, 10, &p)); /* nothing held, nothing to release */
  assert(!parse(INTUOS, in, 7, &p));   /* too short to read */
  assert(parse(INTUOS, in, 10, &p));
  assert(p.x == 0x1234 && p.y == 0x5678 && p.pressure == 0x3ff &&
         p.buttons == 2);
  assert(parse(INTUOS, out, 10, &p) && p.x == 0x1234 && !p.pressure &&
         !p.buttons);
  assert(!parse(INTUOS, other, 10, &p));

  for (i = 0; i < 2; i++) {
    p = (struct pen){0};
    assert(parse(INTUOS_V2, i ? ext : v2, i ? 11 : 10, &p));
    assert(p.x == 44896 && p.y == 29600 && p.pressure == 4095 &&
           p.buttons == 1);
  }
  assert(!parse(INTUOS_V2, ext, 10, &p) && !parse(INTUOS_V3, ext, 10, &p));
  assert(!parse(INTUOS_V3, v2, 10, &p) && parse(INTUOS_V3, ext, 11, &p));

  assert(!parse(INTUOS_V1, v1, 9, &p) && parse(INTUOS_V1, v1, 10, &p));
  assert(p.x == 44697 && p.y == 27941 && p.pressure == 2047 && !p.buttons);

  puts("ok");
  return 0;
}

#endif
