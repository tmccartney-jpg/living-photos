# Provisioning — Why It Exists and How It Works

Each frame is not meant to be interacted with daily. However, it requires occasional configuration:
- Wi-Fi credentials
- Data source (GitHub manifest URL, shared across all 30 frames)
- Recovery/reset

Provisioning exists to support this **without turning the frame into an interactive device**.

---

## Provisioning Model

Each frame uses **device-hosted Wi-Fi and a web page** — same pattern as Praise Kier, reused deliberately so the setup experience is consistent across every device in the ecosystem.

### Entry Conditions

Provisioning is entered only when:
- No saved Wi-Fi credentials exist, OR
- BOOT button is intentionally held during startup

---

## Provisioning Flow

1. Device creates a Wi-Fi network, e.g. `Frame-Setup-##` (numbered per unit, so 30 devices don't collide during setup)
2. User connects via phone/computer
3. Navigate to `http://192.168.4.1`
4. Enter Wi-Fi credentials
5. Device saves and restarts

---

## After Provisioning

- Device connects automatically on future boots
- No ongoing interaction required
- Returns to normal presence-gated scene rotation

---

## Offline Behavior

If Wi-Fi becomes unavailable:
- Device continues operating normally using its local fallback (last cached manifest + video)
- No visible error or "offline" indicator shown to the viewer — unlike Praise Kier, this device has no directive text UI to display a status on, so failure should be invisible, not flagged
- Automatically retries connection in the background
- Does NOT require reprovisioning

---

## Scaling to 30 Units

- Each frame should have a distinct hostname/AP name (e.g. incorporating a short device ID) so provisioning 30 units doesn't create ambiguity about which physical frame you're currently configuring
- Credentials only need to be entered once per frame, at setup or if hardware is replaced/reset

---

## Philosophy

Provisioning is:
- Temporary
- Intentional
- Rare

A frame should feel like a photograph, not an app.

---

## One Sentence

Each frame is configured briefly, then left alone.
