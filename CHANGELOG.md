# Neler yeni / What's new

[Türkçe](#türkçe) · [English](#english)

Yazılımı güncellemek için / To update the software: [browser installer](https://metematik-t1d.github.io/metematik/install/)

---

## Türkçe

### 4.1.1 — 21.11.2026
- mmol/L eşik girişine virgül tuşu eklendi (3,9 doğrudan yazılır).

### 4.1.0 — 21.11.2026

**Zamanlayıcı bolusla kendiliğinden başlayabilir (yalnız Nightscout)**
- Zamanlayıcı sayfasının sağ üstünde yeni bir **ayar (dişli)** butonu var. Bu buton yalnızca bir kişinin kaynağı Nightscout ise görünür, çünkü cihaz bolusları sadece Nightscout'tan görebiliyor.
- **Bolusla otomatik başlat** ayarı varsayılan olarak **kapalı** gelir. Açınca bir **bolus eşiği** girersiniz (örneğin 4). Nightscout'a **bu değerin üstünde** bir bolus girildiğinde zamanlayıcı kendiliğinden başlar.
- Eşiğin altındaki boluslar zamanlayıcıyı başlatmaz. Böylece şekeri düşürmek için girilen küçük boluslar zamanlayıcıyı tetiklemez.
- Süre yine **şeker / 10 dakikadır** ve **bolusun yapıldığı andan** sayılır. Bolus Nightscout'a birkaç dakika geç düşse bile süre doğru kalır.
- Şeker düşükse ya da zamanlayıcı zaten çalışıyorsa otomatik başlatma olmaz. Aynı bolus zamanlayıcıyı iki kez başlatmaz. 15 dakikadan eski boluslar dikkate alınmaz.
- Zamanlayıcı başlayınca kısa bir ses çalar ve ekranda 2 saniyelik bir bildirim çıkar. Bildirimde bolus miktarı ve kalan süre yazar.
- **Aile modunda** hangi kişinin boluslarına bakılacağı ayardan seçilir.
- Bu ayar yalnızca cihazdan yapılır.

**Geri sayım her sayfada**
- Zamanlayıcı çalışırken ana sayfanın ve diğer temaların üstünde, ortada küçük bir **⏱ 12:34** hapı görünür. Hapa dokununca zamanlayıcı sayfası açılır.

**Yeni bitiş ekranı**
- Süre bitince renkli konfetili, gülen tabaklı yeni bir **"Afiyet olsun!"** ekranı çıkar. Melodi ve ana sayfaya dönüş eskisi gibi çalışır.

**Yeni: saat dilimi ayarı**
- Cihazın saati eskiden her ülkede Türkiye saatine (UTC+3) göre çalışıyordu. Artık saat dilimi seçilebiliyor; yaz saati geçişleri kendiliğinden yapılıyor.
- Cihazda: **Ayarlar → Ekran / Dil → Saat dilimi**. Web sayfasında: **Cihaz** kartı.
- Saat, gece ve okul modu saatleri, grafik, bolus ve sensör saatleri ile günlük istatistik seçilen saat dilimine göre çalışır.
- Yeni kurulumda saat dilimi seçilen dile göre önceden ayarlanır (örneğin Español → Madrid). Gerekirse değiştirin. Güncelleme yapan cihazlarda Türkiye saati olarak kalır.

**Düzeltmeler**
- **WiFi çubukları:** Cihaz sinyal gücünü bir an okuyamayınca çubuklar yanlışlıkla "tam çekim" gösterebiliyordu. Artık son doğru değer gösteriliyor ve çubuklar 30 saniyede bir güncelleniyor.
- **mmol/L kullananlar:** Alarm eşikleri artık cihazda ve web sayfasında mmol/L olarak giriliyor (cihazın tuş takımına mmol/L için virgül tuşu eklendi). Web sayfasında birim yazıyor.
- **Rahatsız etme modu:** Ekran kapalıyken zamanlayıcı biterse ekran açılır ve bitiş ekranı görünür.
- **Açılış:** WiFi'ye bağlanamayınca ya da veri alınamayınca açılıştaki yeşil çubuk artık "Hazır" diye sonuna kadar dolmuyor.
- Rusça "ertelendi" yazısının sonunun bozuk görünmesi, Fransızca bir yazının taşması ve Modlar ekranındaki bir düğmenin yanlış rengi düzeltildi.

### 4.0.6
- GitHub'daki önceki sürüm.

---

## English

### 4.1.1 — 2026-11-21
- Added a decimal key for entering mmol/L limits (type 3.9 directly).

### 4.1.0 — 2026-11-21

**The timer can start by itself on a bolus (Nightscout only)**
- The timer page has a new **settings (gear)** button in the top right. It only appears if someone's source is Nightscout, because the device can only see boluses through Nightscout.
- **Auto-start on bolus** is **off** by default. When you turn it on, you set a **bolus threshold** (for example 4). When a bolus **above this value** reaches Nightscout, the timer starts by itself.
- Boluses below the threshold do not start the timer, so small boluses given to bring glucose down do not trigger it.
- The time is still **glucose / 10 minutes**, counted **from the moment of the bolus**. It stays correct even if the bolus reaches Nightscout a few minutes late.
- It does not start if glucose is low or if the timer is already running. The same bolus never starts it twice. Boluses older than 15 minutes are ignored.
- When the timer starts, a short sound plays and a notification shows for 2 seconds with the bolus amount and the time left.
- In **family mode** you choose in the settings whose boluses to watch.
- This setting is on the device only.

**Countdown on every page**
- While the timer runs, a small **⏱ 12:34** pill appears at the top center of the home screen and the other themes. Tap it to open the timer page.

**New finish screen**
- When the time is up, a new colorful **"Enjoy your meal!"** screen with confetti and a smiling plate appears. The melody and the return to the home screen work as before.

**New: time zone setting**
- The device clock used to run on Turkey time (UTC+3) in every country. Now you can choose the time zone; daylight saving time changes are automatic.
- On the device: **Settings → Display / Language → Time zone**. On the web page: the **Device** card.
- The clock, night and school mode hours, graph, bolus and sensor times and the daily statistics follow the chosen time zone.
- On a new setup the time zone is preset from the chosen language (for example Español → Madrid). Change it if needed. Updated devices stay on Turkey time.

**Fixes**
- **WiFi bars:** when the device briefly could not read the signal strength, the bars could wrongly show full signal. Now the last valid value is shown and the bars update every 30 seconds.
- **mmol/L users:** alarm limits are now entered in mmol/L on the device and on the web page (the device keypad gets a decimal key for mmol/L). The web page shows the unit.
- **Do not disturb:** if the timer ends while the screen is off, the screen turns on and shows the finish screen.
- **Start-up:** when WiFi or data is not available, the green start-up bar no longer fills up to "Ready".
- Fixed the end of the Russian "snoozed" text looking broken, an overflowing French text and the wrong color of a button on the Modes screen.

### 4.0.6
- Previous version on GitHub.
