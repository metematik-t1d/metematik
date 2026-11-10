# MeteMatik

**A free, open-source bedside / desk display for CGM glucose readings.**
It shows the latest glucose value, trend arrow and graph from Nightscout, Dexcom Share, LibreLinkUp or Medtrum EasyFollow on a small 2.8" touchscreen, with sound and visual alarms.

[Türkçe için tıklayın → README.tr.md](README.tr.md)

> Made for my son Mete. — Burak

---

## ⚠️ Important — please read

- **MeteMatik is not a medical device.** It is a secondary display for information only.
- **Do not make insulin or treatment decisions based on MeteMatik.** Always rely on your official CGM device or app.
- **Do not turn off the alarms on your CGM / official app.** MeteMatik's alarms are an extra, not a replacement. Internet, WiFi or server problems can delay or stop its data.
- MeteMatik is **not affiliated with** Dexcom, Abbott, Medtrum, Medtronic or Nightscout. All brands belong to their owners. Some sources are read through unofficial (third-party) endpoints that may change or stop working at any time.
- The glucagon video and the Guide mode steps were reviewed by a diabetes nurse; **they do not replace your doctor's instructions.**
- This software is provided **without any warranty** (see [LICENSE](LICENSE)). You use it at your own risk.

Short warnings in other languages: [see below](#warnings-in-other-languages).

---

## Features

- Large glucose value, trend arrow and delta (change per 5 minutes; `*` = estimated across a data gap, `--` = gap longer than 15 minutes)
- Graph (3 / 6 / 12 / 24 h), daily statistics (time in range), sensor / infusion-set age
- Alarms: critical low, low, high, very high, fast fall / rise, no data — with snooze, visual (blinking) alarm and per-alarm sound switch
- **Family mode:** follow up to **3 people** on one device, each with their own source, name and limits. Home screen *in turn* (15 s each) or *split* (all together). The person in alarm comes to the screen.
- **Guide mode** for teachers / carers: an *Alert* button appears on dangerous values and walks the helper through what to do; built-in glucagon video
- Night mode, school mode, do-not-disturb (screen off while in range, wakes on alarm or finger snap)
- Many home-screen themes, including kids' themes and a photo frame
- Carb notes calculator, meal timer
- Settings on the device **and** from a phone browser (same WiFi)
- **9 languages:** Türkçe, English, Español, Português, Français, Deutsch, Italiano, Русский, Bahasa Indonesia
- Color-friendly palette for color-blind users, mg/dL or mmol/L

## Data sources

| Source | What you need |
|---|---|
| **Nightscout** | Your site address (https://…) and, if the site is not public, a read-only access token |
| **Dexcom** (G6 / G7 / ONE+) | Your own Dexcom account with *Share* turned on and at least one follower |
| **FreeStyle Libre** (LibreLinkUp) | A LibreLinkUp *follower* account that accepted the invitation |
| **Medtrum** (EasyFollow) | An EasyFollow follower account (Europe server) |
| **Medtronic** | Via Nightscout (for example with nightscout-connect) |

Step-by-step: [docs/sources.md](docs/sources.md)

## Hardware

| Part | Notes |
|---|---|
| **Elecrow CrowPanel Advance 2.8" ESP32-S3** (HMI display, V1.2) | Touchscreen, WiFi, buzzer — no soldering needed |
| USB-C cable + 5 V USB charger | The device stays plugged in |
| 3D-printed case (optional) | Files in [crown/case](crown/case) — 4 × M3×16 screws (DIN 7985) |

A battery version (with a battery gauge, switch and soldering) is planned as a separate "advanced" guide.

## Installing the software

### Option A — from the browser (easiest)
Open the **[web installer](https://metematik-t1d.github.io/metematik/install/)** in Chrome or Edge on a computer, connect the board's **USB** port with a data cable and press *Install*. No programs to install. (Page source: [install/](install/).)

### Option B — Arduino IDE
1. Install **Arduino IDE 2** and, in *Boards Manager*, **esp32 by Espressif Systems** (3.3.x).
2. In *Library Manager* install: **LovyanGFX** (lovyan03), **WiFiManager** (tzapu), **ArduinoJson** (Benoit Blanchon).
   Tested with esp32 3.3.11, LovyanGFX 1.2.32, WiFiManager 2.0.17, ArduinoJson 7.4.3.
3. Open `crown/firmware/metematik-bt/metematik-bt.ino` (keep all files of that folder together).
4. *Tools* menu:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Disabled**
   - Flash Size: **16MB (128Mb)**
   - Partition Scheme: **Custom** (uses `partitions.csv` from the folder)
   - PSRAM: **OPI PSRAM**
   - Upload Speed: **921600**
5. Connect the board's **USB** port, choose the port and press *Upload*. The device restarts by itself; if it does not, unplug and replug the USB cable.

Later updates can be installed over WiFi from the device's web page (*Firmware update*).

## First start

1. Choose your **language**.
2. The **WiFi setup** screen shows two QR codes. Scan QR 1 with your phone to join the device's setup network, then (if no page opens) scan QR 2.
3. Choose your home WiFi and fill in **one** data source. Save.
4. The device restarts and shows your glucose. Everything else can be changed on the device (*Settings*) or from a phone browser: scan the QR in *Settings → QR / Web*.

## Family mode

On the device's web page, open the **Family mode** card (above *Security*), turn it **on**, choose 2 or 3 people and give them names. Then select each person at the top of the page and enter their source and limits. On the device: *Modes → Family mode*.

## Translations

Texts live in `crown/firmware/metematik-bt/lang/`. Corrections by native speakers are very welcome — especially for the medical texts. Open an issue or write to us.

## License

Copyright © 2026 Burak. **GNU General Public License v3.0** — see [LICENSE](LICENSE).
You may use, study, share and change MeteMatik. If you share a changed version, it must also be open source under GPL-3.0.

Third-party parts: Noto Sans font (SIL Open Font License 1.1), LovyanGFX (FreeBSD), WiFiManager (MIT), ArduinoJson (MIT), QRCode by Richard Moore (MIT), ESP32 Arduino core (LGPL 2.1 / Apache 2.0).

## Contact

- E-mail: **metematik.t1d@gmail.com**
- YouTube: **@metematik.t1d** (how to build, set up and use)

---

## Warnings in other languages

- **Türkçe:** MeteMatik tıbbi cihaz değildir. İnsülin ve tedavi kararları için resmi CGM cihazınızı/uygulamanızı esas alın. Asıl CGM alarmlarınızı kapatmayın. Dexcom, Abbott, Medtrum, Medtronic ve Nightscout ile bağlantılı değildir. Garanti verilmez.
- **Español:** MeteMatik no es un dispositivo médico. Para decisiones de insulina y tratamiento, use siempre su MCG o aplicación oficial. No desactive las alarmas de su MCG. No está afiliado a Dexcom, Abbott, Medtrum, Medtronic ni Nightscout. Sin ninguna garantía.
- **Português:** O MeteMatik não é um dispositivo médico. Para decisões de insulina e tratamento, use sempre o seu CGM ou aplicativo oficial. Não desative os alarmes do seu CGM. Não tem vínculo com Dexcom, Abbott, Medtrum, Medtronic ou Nightscout. Sem qualquer garantia.
- **Français :** MeteMatik n'est pas un dispositif médical. Pour les décisions d'insuline et de traitement, fiez-vous toujours à votre CGM ou à l'application officielle. Ne désactivez pas les alarmes de votre CGM. Non affilié à Dexcom, Abbott, Medtrum, Medtronic ou Nightscout. Aucune garantie.
- **Deutsch:** MeteMatik ist kein Medizinprodukt. Für Insulin- und Therapieentscheidungen gilt immer Ihr offizielles CGM bzw. die offizielle App. Schalten Sie die Alarme Ihres CGM nicht aus. Keine Verbindung zu Dexcom, Abbott, Medtrum, Medtronic oder Nightscout. Ohne jegliche Gewährleistung.
- **Italiano:** MeteMatik non è un dispositivo medico. Per le decisioni su insulina e terapia affidati sempre al tuo CGM o all'app ufficiale. Non disattivare gli allarmi del CGM. Non affiliato a Dexcom, Abbott, Medtrum, Medtronic o Nightscout. Nessuna garanzia.
- **Русский:** MeteMatik не является медицинским изделием. Решения об инсулине и лечении принимайте только по вашему официальному CGM или приложению. Не отключайте сигналы вашего CGM. Не связан с Dexcom, Abbott, Medtrum, Medtronic или Nightscout. Без каких-либо гарантий.
- **Bahasa Indonesia:** MeteMatik bukan alat medis. Untuk keputusan insulin dan pengobatan, selalu gunakan CGM atau aplikasi resmi Anda. Jangan matikan alarm CGM Anda. Tidak berafiliasi dengan Dexcom, Abbott, Medtrum, Medtronic, atau Nightscout. Tanpa jaminan apa pun.
