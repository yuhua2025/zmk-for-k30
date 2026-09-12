#!/usr/bin/env python3
"""Patch ZMK v0.3.0 app/src/battery.c so the battery level is reported on
every timer tick instead of only when the level changes.

Without this patch the split central (dongle) only ever learns the battery
level once - whenever the level last changed - so a dongle that boots or
reconnects after that stays at 0% until the next real level change (which is
nearly never for a stable battery). See k30_dongle_status_screen.c comments.
"""

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = pathlib.Path(sys.argv[1])
    src = path.read_text()

    old = """    if (last_state_of_charge != state_of_charge.val1) {
        last_state_of_charge = state_of_charge.val1;

        rc = raise_zmk_battery_state_changed(
            (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

        if (rc != 0) {
            LOG_ERR("Failed to raise battery state changed event: %d", rc);
            return rc;
        }
    }
"""

    new = """    last_state_of_charge = state_of_charge.val1;

    // Patched (build.yml): always raise the event on every timer tick, even
    // when the level hasn't changed, so the split central + dongle display
    // keeps receiving periodic battery reports. 60s default interval is safe.
    rc = raise_zmk_battery_state_changed(
        (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

    if (rc != 0) {
        LOG_ERR("Failed to raise battery state changed event: %d", rc);
        return rc;
    }
"""

    if old not in src:
        print("ERROR: expected battery.c pattern not found; refusing to patch.",
              file=sys.stderr)
        return 1

    path.write_text(src.replace(old, new, 1))
    print("battery.c patched: report on every tick")
    return 0


if __name__ == "__main__":
    sys.exit(main())