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
  u8 fmt, ninit,
      init[2][2]; /* init: feature reports selecting raw pen reports */
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

/* out = (in ^ f) + c: f is 0, or -1 to mirror the axis (~in + c). */
struct xform {
  int swap, fx, cx, w, fy, cy, h;
};

static int area_start(unsigned int c_mm, int size, int full, int um) {
  u64 c = div_u64((u64)c_mm * 1000 * full, um);

  if (!c_mm || c < (u64)size / 2)
    return c_mm ? 0 : (full - size) / 2;
  return c - size / 2 > (u64)(full - size) ? full - size : (int)c - size / 2;
}

/*
 * maps a w_mm x h_mm area of the tablet centred at x_mm, y_mm and seen with
 * the tablet turned rot degrees clockwise, onto 0..w, 0..h.
 */
static int xform_init(struct xform *f, const struct tablet *t,
                      unsigned int w_mm, unsigned int h_mm, unsigned int x_mm,
                      unsigned int y_mm, unsigned int rot) {
  int max[2] = {t->max_x, t->max_y}, um[2] = {t->width_um, t->height_um};
  int flipx = rot == 90 || rot == 180, flipy = rot == 180 || rot == 270;
  int fw, fh; /* tablet size as seen after rotation */
  u64 w, h;

  if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
    return -1;
  f->swap = rot == 90 || rot == 270;
  fw = max[f->swap];
  fh = max[!f->swap];
  w = fw;
  h = fh;
  if (w_mm)
    w = div_u64((u64)w_mm * 1000 * fw, um[f->swap]);
  if (h_mm)
    h = div_u64((u64)h_mm * 1000 * fh, um[!f->swap]);
  if (w > (u64)fw || h > (u64)fh)
    return -1;
  f->w = w;
  f->h = h;
  f->fx = -flipx;
  f->fy = -flipy;
  f->cx = (flipx ? fw + 1 : 0) - area_start(x_mm, f->w, fw, um[f->swap]);
  f->cy = (flipy ? fh + 1 : 0) - area_start(y_mm, f->h, fh, um[!f->swap]);
  return 0;
}

static __always_inline int clampi(int v, int hi) {
  return v < 0 ? 0 : v > hi ? hi : v;
}

static __always_inline void xform(const struct xform *f, int px, int py, int *x,
                                  int *y) {
  *x = clampi(((f->swap ? py : px) ^ f->fx) + f->cx, f->w);
  *y = clampi(((f->swap ? px : py) ^ f->fy) + f->cy, f->h);
}

#ifdef __KERNEL__

struct lumenrx {
  struct input_dev *input;
  struct xform f;
  struct pen pen;
  int x, y;    /* last sent */
  u8 btn, fmt; /* btn: last sent; bit 0 tip, bits 1-2 pen buttons */
} ____cacheline_aligned;
static_assert(sizeof(struct lumenrx) == SMP_CACHE_BYTES);

static unsigned int area_w, area_h, area_x, area_y, rotation;
static int pressure __read_mostly;
module_param(area_w, uint, 0444);
MODULE_PARM_DESC(area_w, "area width in mm, 0 = whole tablet");
module_param(area_h, uint, 0444);
MODULE_PARM_DESC(area_h, "area height in mm, 0 = whole tablet");
module_param(area_x, uint, 0444);
MODULE_PARM_DESC(area_x, "area centre, 0 = tablet centre");
module_param(area_y, uint, 0444);
MODULE_PARM_DESC(area_y, "area centre, 0 = tablet centre");
module_param(rotation, uint, 0444);
MODULE_PARM_DESC(rotation, "tablet rotation in deg");
module_param(pressure, int, 0644);
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
  btn = (p.pressure > pressure) | p.buttons << 1;
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

static void lumenrx_free(void *lx) { kfree(lx); }

static int lumenrx_probe(struct hid_device *hdev,
                         const struct hid_device_id *id) {
  const struct tablet *t = (const void *)id->driver_data;
  int max[2] = {t->max_x, t->max_y}, um[2] = {t->width_um, t->height_um};
  struct input_dev *input;
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
  input = devm_input_allocate_device(&hdev->dev);
  if (!input)
    return -ENOMEM;
  if (xform_init(&lx->f, t, area_w, area_h, area_x, area_y, rotation)) {
    hid_err(hdev, "bad rotation, or area larger than the %dx%d mm tablet\n",
            t->width_um / 1000, t->height_um / 1000);
    return -EINVAL;
  }
  lx->fmt = t->fmt;
  lx->input = input;
  hid_set_drvdata(hdev, lx);

  input->name = "lumenrx pointer";
  input->phys = hdev->phys;
  input->id.bustype =
      BUS_VIRTUAL; /* with Wacom's ids userspace expects a tablet */
  input_set_capability(input, EV_KEY, BTN_LEFT);
  input_set_capability(input, EV_KEY, BTN_RIGHT);
  input_set_capability(input, EV_KEY, BTN_MIDDLE);
  input_set_abs_params(input, ABS_X, 0, lx->f.w, 0, 0);
  input_set_abs_params(input, ABS_Y, 0, lx->f.h, 0, 0);
  input_abs_set_res(input, ABS_X,
                    max[lx->f.swap] * 1000 / um[lx->f.swap]); /* units per mm */
  input_abs_set_res(input, ABS_Y, max[!lx->f.swap] * 1000 / um[!lx->f.swap]);

  ret = input_register_device(input);
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

  hid_info(hdev, "%s, area %dx%d units, rotation %u\n", t->name, lx->f.w,
           lx->f.h, rotation);
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

  assert(xform_init(&f, &t, 0, 0, 0, 0, 45));
  assert(xform_init(&f, &t, 153, 95, 0, 0, 0));
  assert(xform_init(&f, &t, 152, 95, 0, 0, 90));
  assert(xform_init(&f, &t, ~0u, ~0u, 0, 0, 0));

  assert(!xform_init(&f, &t, 100, 50, 0, 0, 0) && f.w == 10000 && f.h == 5000);
  assert(at(&f, 7600, 4750, 5000, 2500)); /* centre -> centre */
  assert(at(&f, 0, 9500, 0, 5000));       /* outside clamps */

  assert(!xform_init(&f, &t, 0, 0, 0, 0, 180));
  assert(at(&f, 0, 0, 15200, 9500) && at(&f, 15200, 9500, 0, 0));

  /* turned 90deg the tablet's bottom-left corner is the user's top-left */
  assert(!xform_init(&f, &t, 0, 0, 0, 0, 90) && f.w == 9500 && f.h == 15200);
  assert(at(&f, 0, 9500, 0, 0) && at(&f, 15200, 0, 9500, 15200));

  assert(!xform_init(&f, &t, 50, 100, 0, 0, 270) && f.w == 5000 &&
         f.h == 10000);
  assert(at(&f, 15200, 0, 0, 0)); /* top-right -> top-left clamped */
  assert(at(&f, 7600, 4750, 2500, 5000));

  assert(!xform_init(&f, &t, 100, 50, 60, 30, 0));
  assert(at(&f, 1000, 500, 0, 0) && at(&f, 6000, 3000, 5000, 2500));
  assert(!xform_init(&f, &t, 100, 50, 60, 30, 180));
  assert(at(&f, 14200, 9000, 0, 0) && at(&f, 9200, 6500, 5000, 2500));
  assert(!xform_init(&f, &t, 100, 50, 10, ~0u, 0));
  assert(at(&f, 0, 4500, 0, 0) && at(&f, 10000, 9500, 10000, 5000));
  assert(!xform_init(&f, &t, 50, 100, 20, 0, 90) && at(&f, 2600, 9500, 0, 0));

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
