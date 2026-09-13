#!/usr/bin/env python3
"""Patch ZMK v0.3.0 app/src/battery.c so the battery level is reported on
every timer tick instead of only when the level changes.

Why: for split keyboards over BLE, the peripheral->central battery event is
dropped by the BLE transport (bt-service.c comment: "The BLE transport uses
standard BAS service for propagation"). The ONLY paths that reach a central
dongle are:

  1. a GATT read of the Battery Service char at connect time (one-shot, can
     fail asynchronously), and
  2. BAS notifications, which battery.c only sent when the level CHANGED.

With a stable battery value neither path fires, so the dongle stays at the
"unknown" state forever. This patch changes battery.c to:

  a) raise zmk_battery_state_changed on every timer tick (helps wired split
     transports / host BAS), and
  b) call bt_bas_set_battery_level() on every timer tick so the GATT BAS
     notification reaches the connected central on a stable 60s cadence.

The 60s timer is started immediately at boot and while ACTIVE, so a dongle
that connects later still gets a fresh value within 60s.
"""

import pathlib
import sys


def _apply(path: pathlib.Path) -> int:
    src = path.read_text()

    # (a) Always raise the battery state changed event on every tick.
    old_event = """    if (last_state_of_charge != state_of_charge.val1) {
        last_state_of_charge = state_of_charge.val1;

        rc = raise_zmk_battery_state_changed(
            (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

        if (rc != 0) {
            LOG_ERR("Failed to raise battery state changed event: %d", rc);
            return rc;
        }
    }
"""

    new_event = """    last_state_of_charge = state_of_charge.val1;

    // Patched (build.yml): always raise the event on every timer tick, even
    // when the level hasn't changed, so split central + dongle display keeps
    // receiving periodic battery reports. 60s default interval is safe.
    rc = raise_zmk_battery_state_changed(
        (struct zmk_battery_state_changed){.state_of_charge = last_state_of_charge});

    if (rc != 0) {
        LOG_ERR("Failed to raise battery state changed event: %d", rc);
        return rc;
    }
"""

    # (b) Always push the value into the BAS GATT service on every tick. The
    #     change-only guard below means a stable battery level would never
    #     produce a BAS notification, so the central dongle never learns it.
    old_bas = """#if IS_ENABLED(CONFIG_BT_BAS)
    if (bt_bas_get_battery_level() != last_state_of_charge) {
        LOG_DBG("Setting BAS GATT battery level to %d.", last_state_of_charge);

        rc = bt_bas_set_battery_level(last_state_of_charge);

        if (rc != 0) {
            LOG_WRN("Failed to set BAS GATT battery level (err %d)", rc);
            return rc;
        }
    }
#endif
"""

    new_bas = """#if IS_ENABLED(CONFIG_BT_BAS)
    // Patched (build.yml): push BAS + notify every tick instead of only on
    // change. The BLE split transport drops battery events and relies on the
    // BAS service, so periodic notifications are what keep the dongle display
    // current. bt_bas_set_battery_level() tolerates not-yet-connected links.
    LOG_DBG("Setting BAS GATT battery level to %d.", last_state_of_charge);

    rc = bt_bas_set_battery_level(last_state_of_charge);

    if (rc != 0) {
        LOG_WRN("Failed to set BAS GATT battery level (err %d)", rc);
    }
#endif
"""

    replaced = False
    if old_event in src:
        src = src.replace(old_event, new_event, 1)
        replaced = True
    else:
        print("WARN: battery event pattern (a) not found; may already be patched.",
              file=sys.stderr)

    if old_bas in src:
        src = src.replace(old_bas, new_bas, 1)
        replaced = True
    else:
        print("WARN: BAS pattern (b) not found; may already be patched.", file=sys.stderr)

    if not replaced:
        # Neither pattern found: refuse to half-patch a changed source tree.
        print("ERROR: expected battery.c patterns not found; refusing to patch.",
              file=sys.stderr)
        return 1

    path.write_text(src)
    print("battery.c patched: event + BAS notification on every tick")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    return _apply(pathlib.Path(sys.argv[1]))


if __name__ == "__main__":
    sys.exit(main())
