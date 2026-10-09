# Design Principles

Each frame is a calm, purpose-built object.

---

## Core Rules

- One scene displayed at a time, per frame
- No user interaction required in normal operation
- No modes exposed to the user
- No notifications
- Motion only when unobserved (presence-gated), never on demand

---

## Connectivity

### Wi-Fi
- Used to fetch the shared GitHub manifest and sync crossfade state
- Automatically retries if offline
- Never forces reprovisioning if credentials exist

### Provisioning
- Device-hosted Wi-Fi + web page (see `Provisioning.md`)
- Triggered only when needed (no credentials, or BOOT held)

---

## Behavior Priority

1. Presence event (near/far transition from mmWave sensor)
2. Freeze / animate state
3. Scene rotation (active-bag draw)
4. Local fallback (cached last-known scene)

---

## Failure Behavior

- Uses local fallback (last successfully fetched manifest + cached video) if GitHub is unavailable
- Continues normal presence-gated behavior even when offline
- No visible error state to the viewer — the frame should never look "broken," only quiet
- Automatically retries GitHub sync in the background

---

## Non-Goals

- No apps, dashboards, or menus
- No visible indication that the frame is "smart" or networked
- No continuous interaction or gamification
- No sound (hardware has no speaker; irrelevant even if content includes audio)

---

## Guiding Principle

If it adds friction, demands attention, or reveals the mechanism, it does not belong.

The frame should feel like a photograph that happens to be alive — not a device.
