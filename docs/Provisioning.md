# Provisioning — Why It Exists and How It Works

Each frame is not meant to be interacted with daily. However, it requires occasional configuration:
- Wi-Fi credentials
- Recovery when the home network changes (new router, new password)

Provisioning exists to support this **without turning the frame into an interactive device**.

The data source (the `living-photos` GitHub repo) is built into the firmware and needs no setup.

---

## Provisioning Model

Setup happens **on the frame's own touchscreen**: a list of nearby networks and an on-screen keyboard. No phone, laptop or setup network is needed.

### Entry Conditions

The setup screen appears only when:
- No Wi-Fi network has been saved yet (first boot), OR
- The saved network has been missing for **1 hour**

It can also be opened by hand from the hidden diagnostics screen (**Wi-Fi** button).

---

## Provisioning Flow

1. The frame lists the networks it can see (strongest first) — tap one
2. Secured networks: type the password on the on-screen keyboard (a **Show** button reveals it), tap **Connect**
3. On success the network is saved and the frame returns to its scene
4. On failure the frame says why ("wrong password?", "network not found") and keeps the typed password for another try

The previously saved network stays saved until a new one connects successfully.

---

## Safeguards (calm object)

- **While the setup screen is up, the frame keeps retrying the saved network.** If it comes back, setup closes by itself and the scene returns.
- **"Not now"** returns to the scene; the frame keeps playing from its SD card and **asks again after 1 hour** — every hour until a network connects, because without Wi-Fi the frame gets no new scenes or daily rotation.
- **Untouched for 5 minutes**, the setup screen acts like "Not now", so it never sits on the wall.
- Router reboots and short outages never show setup (the 1-hour rule).

---

## After Provisioning

- Device connects automatically on future boots
- Sets its clock from internet time (SNTP), US Eastern with daylight saving
- No ongoing interaction required
- Returns to normal presence-gated scene rotation

---

## Offline Behavior

If Wi-Fi becomes unavailable:
- Device continues operating normally from its SD card
- No visible error or "offline" indicator shown to the viewer
- Automatically retries the connection in the background (5 s backoff, up to once a minute)
- Shows setup only under the 1-hour rule above

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
