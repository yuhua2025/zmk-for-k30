#!/usr/bin/env python3
"""Patch Zephyr's LVGL mono flush callback to rotate the panel 180 degrees.

Why: this SSD1312 module only produces a picture with the ssd1306 driver's
segment-remap/com-invdir hardware setting, and that combination renders the
framebuffer upside down. The other hardware combinations either also flip it
or output nothing at all (verified on the real panel). Since the panel cannot
be oriented in hardware, rotate the pixel block in the flush callback instead.

The panel is 128x32. Each flush covers a partial area (LVGL's renderer works in
8-row-aligned chunks), and the rotation is a per-pixel remap *within that chunk*
plus recomputing the destination origin, so partial refreshes stay correct.
Do NOT enable LV_Z_FULL_REFRESH: Zephyr sizes its draw buffer as
VDB_SIZE*W*H/100/8 bytes, far too small for a whole-screen render, and LVGL
would write out of bounds and hard-fault (verified: black screen).
"""

import sys

OLD = "\tdisplay_write(display_dev, area->x1, area->y1, &desc, (void *)color_p);\n"

NEW = """\t/* ---- 180 degree rotation patch (see tools/patch_lvgl_mono_rotate.py) ---- */
\tif ((data->cap.screen_info & SCREEN_INFO_MONO_VTILED) && (w * h / 8U) <= 128U * 32U / 8U) {
\t\tstatic uint8_t rot_buf[128 * 32 / 8];
\t\tuint8_t *src_buf = (uint8_t *)color_p;
\t\tuint16_t rx, ry, sx, sy;
\t\tuint16_t nx = data->cap.x_resolution - (area->x1 + w);
\t\tuint16_t ny = data->cap.y_resolution - (area->y1 + h);

\t\tmemset(rot_buf, 0, sizeof(rot_buf));
\t\tfor (ry = 0; ry < h; ry++) {
\t\t\tfor (rx = 0; rx < w; rx++) {
\t\t\t\tsx = w - 1 - rx;
\t\t\t\tsy = h - 1 - ry;
\t\t\t\tif (src_buf[sx + (sy / 8) * w] & BIT(7 - (sy % 8))) {
\t\t\t\t\trot_buf[rx + (ry / 8) * w] |= BIT(7 - (ry % 8));
\t\t\t\t}
\t\t\t}
\t\t}
\t\tdisplay_write(display_dev, nx, ny, &desc, rot_buf);
\t} else {
\t\tdisplay_write(display_dev, area->x1, area->y1, &desc, (void *)color_p);
\t}
"""


def main() -> int:
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    if "180 degree rotation patch" in src:
        print("already patched")
        return 0

    if OLD not in src:
        print("ERROR: anchor not found in %s" % path)
        return 1

    src = src.replace(OLD, NEW, 1)

    if "#include <string.h>" not in src:
        src = src.replace('#include "lvgl_display.h"',
                          '#include <string.h>\n#include "lvgl_display.h"', 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(src)

    print("patched %s: mono flush now rotates 180 degrees" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
