# Setting up a data source

[Türkçe → kaynaklar.md](kaynaklar.md)

Fill in **one** source per person. You can enter it during the first WiFi setup, or later on the device's web page (*Settings → QR / Web* on the device shows the address and a QR code; the web password is on the device's *Info* screen).

MeteMatik only **reads** glucose data. It never writes anything to your account.

---

## Nightscout

1. **URL:** your site address, for example `https://yourname.example.com` (no `/` at the end needed).
2. **API secret / token:**
   - If your site is public (you can see the graph without logging in), leave it **empty**.
   - If it is not public, create a token: Nightscout → *Admin Tools* → *Add new Subject* → role **readable** → copy the **access token** (looks like `metematik-1a2b3c4d5e6f7g8h`) and paste it here.
   - Please do **not** use your main `API_SECRET` — a read-only token is safer.
3. The site's https certificate must be valid.

Works with: IOB / COB / last boluses (if the plugins are on), sensor and set age (*SAGE* / *CAGE*).

### Medtronic (CareLink)
MeteMatik does not connect to CareLink directly. You can bring CareLink data into Nightscout (for example with Nightscout's *connect* feature / *nightscout-connect*) and then use the Nightscout source. Follow those projects' own guides; such tools may conflict with Medtronic's terms of use.

---

## Dexcom (G6 / G7 / ONE+)

1. Use **your own Dexcom account** — the one you log in to in the Dexcom app on the CGM user's phone (**not** the Follow app account).
2. In the Dexcom app, **Share** must be on and there must be **at least one follower** (the follower does not need to use the app).
3. **User name:** user name, e-mail, or phone number. If the account was created with a phone number, write it with the country code and without spaces, e.g. `+905XXXXXXXXX`.
4. **Server:** `ous` for accounts outside the USA (Europe, Türkiye, …), `us` for the USA.

If you see *"No data in 24 h"*: check that Share is on and a follower exists.

---

## FreeStyle Libre (LibreLinkUp)

1. In the **LibreLink / Libre** app (the CGM user's phone), invite a person to follow: *Connected apps → LibreLinkUp → Add connection*.
2. Install **LibreLinkUp** on any phone, create an account with the invited e-mail and **accept** the invitation.
3. On MeteMatik enter the **LibreLinkUp e-mail and password**. Region: `EU` (the device follows the server's region redirect automatically).

If you see *"Accept the new terms"*: open the LibreLinkUp app once, accept the new terms, then tap *Retry* on the device.

Note: LibreLinkUp gives the last 12 hours of graph data; the device keeps its own log to fill up to 24 hours over time. LibreLinkUp does not report double arrows, so the fast fall / rise alarm uses the change per 5 minutes.

---

## Medtrum (EasyFollow)

1. In the **EasyView / Medtrum** app, invite a follower (*Follow / Share*).
2. Create an **EasyFollow** account with that e-mail and accept the invitation.
3. On MeteMatik enter the **EasyFollow e-mail and password**. Server: Europe (`easyview.medtrum.eu`).

---

## Family mode (2–3 people)

1. Device web page → **Family mode** card (above *Security*) → *Status: On* → number of people → names → *Save*.
2. At the top of the page, select a person → fill in that person's source and limits → *Save*. Repeat for each person.
3. Each person can use a different source (for example one Dexcom, one Libre).

---

## Problems

| Message on the device | What to do |
|---|---|
| No WiFi / *Hold BOOT 2 s* | Router on and nearby? Holding the **BOOT** button for 2 seconds opens WiFi / source setup. Saved networks are kept. |
| Login failed | User name or password may be wrong. Re-enter it on the web page. The device retries slowly so your account is not locked. |
| No internet | WiFi is connected but the router has no internet. Data returns by itself. |
| Source not configured | Enter a source (setup or web page). |
| Nightscout not reachable | Check the address and that the site is up; the https certificate must be valid. |

Still stuck? Write to **metematik.t1d@gmail.com** with: device version (*Info* screen), source type, country, and a photo of the screen. **Never send your passwords.**
