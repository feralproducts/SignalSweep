# Wi‑Fi hotspot, smoother hunting, and device pins

This build adds three things to SignalSweep on the **XIAO ESP32‑C5**:

1. **Any browser, any device.** The board can run its own Wi‑Fi hotspot and serve the app itself, so iPhones, iPads, Safari, and Firefox work. Before, only Chrome/Edge on a computer or Android could connect. Bluetooth and USB still work exactly as before.
2. **A smoother map and hunt meter.** Signal readings are filtered, map markers glide instead of flickering, and your own position updates live while the map or a hunt is open.
3. **Locate and pin.** While you hunt a device, the app estimates where it is from signal strength at the places you've walked. You can then pin it, either where you're standing ("Found it") or at the estimate, and share the pin as lat/long, degrees‑minutes‑seconds, or MGRS.

---

## Connecting

| Device | How |
|---|---|
| Android tablet/phone (APK or Chrome) | Bluetooth, as before, or the Wi‑Fi hotspot |
| Windows/Mac/Linux with Chrome or Edge | Bluetooth or USB, as before, or the Wi‑Fi hotspot |
| **iPhone / iPad (any browser)** | **Wi‑Fi hotspot** |
| Safari on a Mac, Firefox anywhere | Wi‑Fi hotspot |

### Turning the hotspot on

- **On the board:** double‑tap the **BOOT** button. The light flashes **blue** and the buzzer chirps. Double‑tap again to turn it off (amber flash). A single tap still opens the two‑minute Bluetooth window, as before. It just responds about half a second later, while the board waits to see whether a second tap is coming.
- **From the app** (a phone already connected by Bluetooth or USB): **Settings → Wi‑Fi hotspot → On.**

The hotspot is named **`SignalSweep‑XXXX`** (the last four characters are unique to each board). The default password is **`signalsweep`**. **Change it** under Settings → Wi‑Fi hotspot before you take the board out.

The hotspot is **off at every power‑up** and turns itself off after **10 minutes with nobody connected**.

### The trade‑off you accept while the hotspot is on

The board has one Wi‑Fi radio. Normally it hops across every Wi‑Fi channel looking for threats. While the hotspot is up, the radio must stay on the hotspot's channel (**channel 6**), so **Wi‑Fi detection only sees channel 6**. Channel 6 was chosen because Wi‑Fi Aware (NAN) Remote ID uses it in the 2.4 GHz band; drones that broadcast Remote ID as a Wi‑Fi beacon on other channels will be missed while the hotspot is on. **Bluetooth detection (trackers, AirTags, Bluetooth Remote ID) is not affected.** Turn the hotspot off for a full sweep.

Also: a Site Survey packet capture can't run while the hotspot is on, and the app says so.

### First‑time setup on each iPhone/iPad (once per phone, per board)

iOS only allows location and encryption on a secure (https) page, and both pinning and the PIN‑locked pin store need them. Each board makes its own security certificate, and each phone has to trust it once:

1. Join the board's Wi‑Fi (Settings → Wi‑Fi → `SignalSweep‑XXXX`).
2. In Safari, open **http://192.168.4.1** (note: **http**). Tap **Download certificate** → **Allow**.
3. Open **Settings → Profile Downloaded → Install** (enter your passcode).
4. Open **Settings → General → About → Certificate Trust Settings** and turn on **SignalSweep XXXX Local CA**.
5. Open **https://192.168.4.1**. The app loads and connects by itself. Use **Share → Add to Home Screen** for one‑tap access.

After that, every visit is just: join the Wi‑Fi, then open the home‑screen icon.

- **Android:** step 2 is the same. Then go to Settings → Security → Encryption & credentials → Install a certificate → CA certificate, and pick the downloaded file.
- **Factory reset** (holding BOOT for 5 s) makes the board generate a **new** certificate, and every phone has to repeat this setup.

### Troubleshooting: board status page

With a phone on the hotspot, open **http://192.168.4.1/status** (plain http). It shows how long the hotspot has been up, how many phones and app connections there are, free memory, and whether secure (TLS) connections are using the board's PSRAM. If the app page won't load, read these lines out; they say whether the board is short of memory or restarted.

### Will the map have street tiles over the hotspot?

Possibly not. The board's hotspot has no internet, and a phone joined to it may send its internet traffic (map tiles) to the hotspot rather than to cellular. iPhones sometimes fall back to cellular on their own ("Wi‑Fi Assist"); check this on your phones. If tiles don't load, the app says so. Your position, the walked path, the estimate, and pins all still work, and pinned coordinates open fine in Apple/Google Maps once you're off the board's Wi‑Fi.

(An earlier build tried to force phones to keep cellular by leaving the "router" out of the address the board hands out. In the field that left an iPhone holding the board's own address, so it was removed.)

---

## Hunting and pinning

1. On the Sweep list, tap **Hunt** on the device. The hunt panel opens at the top. The board's buzzer becomes a Geiger‑counter clicker for that device, as before.
2. **The big number is now the smoothed signal**, and the trace shows the smoothed line in bold over the raw readings. "warmer / colder / holding" is based on the smoothed level, so it stops flickering.
3. **Walk.** Allow location when asked. The panel tells you what it needs:
   - *"Walk around the area…"*: not enough spread yet. Around 25 ft of walking in more than one direction is the minimum.
   - *"Estimate: 180 ft NE of you · likely within 40 ft (fair)"*: where it probably is, and a circle it's likely inside.
   - Hints: *"looks further N than you've walked, head that way"*, *"walk a stretch at right angles"*, *"signal barely changes, get contrast"*.
4. Tap **Map** to see the hunt. Your path is drawn as dots colored from blue (weak) to red (strong), with the **amber circle** for the estimate and the **"Strongest"** dot where you got the best reading.
5. **Pin it:**
   - **Found it — pin here**: stand at the device and tap. The app takes the best GPS fix over a few seconds. **This is the precise pin.**
   - **Pin estimate**: saves the estimate's center and its "likely within" radius.
6. The pin sheet shows **Lat/Long, DMS, and MGRS**, each with a Copy button, plus **Share pin** (text message, AirDrop, etc.), **Copy all**, **Apple Maps**, **Google Maps**, and **Show on map**. Shared text looks like:

   ```
   SignalSweep pin — Apple AirTag (separated) (AA:BB:CC:11:22:33)
   Found at, 10/22/2026, 5:03 PM
   Lat/Long: 35.925345, -86.868700
   DMS: 35°55′31.2″N 86°52′07.3″W
   MGRS: 16S EE 11844 75676
   Accuracy: ±13 ft (GPS)
   https://www.google.com/maps/search/?api=1&query=35.925345,-86.868700
   ```

Pins go into the app's existing **PIN‑encrypted** store. The first pin asks you to set a PIN. Saved pins appear in Survey → Pins and on the map, and tapping one reopens its sheet.

### How good is the estimate? (honest version)

It's one antenna measuring signal strength. Bodies, walls, vehicles, and how you hold the board each shift readings by 10 dB or more, which is a factor of 2–3 in distance. In simulation with realistic noise:

| How you walked | Typical error | Inside the "likely within" circle |
|---|---|---|
| Back‑and‑forth sweep over the area | ~15–30 ft | ~65–80% of the time |
| Walking toward it | ~35–45 ft | ~85% |
| One straight pass alongside it | ~75–100 ft | ~65% (it can't tell which side) |
| Circling it at the same distance | poor (often 150+ ft) | marked **rough**: the signal barely changes |

Use the estimate to **know where to go**, then the beeps for the last few feet, then **Found it** for the pin your team will use. Indoors, GPS itself is vague (±30–100 ft), so the walk‑down with the clicker is the dependable method there.

### What the app keeps

While the map or a hunt is open, the app watches your position (upstream SignalSweep deliberately never did). Hunt readings live **only in memory** and are wiped when you stop hunting or disconnect. Nothing is written to the phone except pins you choose to drop, and those are encrypted.

---

## Bench checklist (verify on real hardware before field use)

None of this has been run on a physical board yet. The firmware compiles against the exact XIAO ESP32‑C5 core (arduino‑esp32 3.3.7 / ESP‑IDF 5.5), and the app was tested in a browser against a simulated board. These need your eyes:

- [ ] Double‑tap BOOT: blue flash, `SignalSweep‑XXXX` appears in the phone's Wi‑Fi list. Double‑tap again: amber flash, network gone.
- [ ] Single tap still opens the Bluetooth window (about half a second later than before).
- [ ] iPhone: certificate download and install from http://192.168.4.1 works. https://192.168.4.1 loads **without** a warning, and the header reads "SignalSweep · Wi‑Fi".
- [ ] iPhone: map **tiles** load over the hotspot (cellular stays active). Note the phone model if not.
- [ ] Live list updates about every second over Wi‑Fi. The hunt meter updates about 4×/second.
- [ ] Hotspot on: BLE trackers still detected. Wi‑Fi devices on channel 6 still detected.
- [ ] Two phones connected at once (one Bluetooth, one Wi‑Fi) both get updates.
- [ ] Hotspot turns itself off after 10 minutes with nobody connected.
- [ ] Location permission prompt appears on iPhone (https page), and "Found it" saves a pin with sensible ± feet.
- [ ] Power‑cycle: hotspot comes up **off**, and the certificate persists (no reinstall needed).

## For developers

- Firmware: `firmware/src/wifi_link.{h,cpp}` (hotspot, on‑device CA and server certificate, HTTPS + `wss://…/ws`, channel park). It's compiled only in the `c5` env (`-DSIGNALSWEEP_WIFI_LINK`). Every line the app receives goes out through `sendBleSerial()`, which now also queues it for Wi‑Fi clients.
- The app is compiled into the firmware: `firmware/tools/gen_web_bundle.py` runs before each C5 build and turns `app/dist` into `src/web_bundle.gen.cpp` (about 0.6 MB of flash). CI builds the app **before** the firmware. A firmware‑only local build gets a placeholder page.
- App: `app/public/locate.js` (Kalman RSSI filter, source estimator, DD/DMS/MGRS). MGRS was cross‑checked against proj4js/mgrs on 20,010 points with zero mismatches. Transport `'WIFI'` and the map/locator are in `app/public/app.js`. `node app/selftest.js` covers both.
