# Veri kaynağını ayarlama

[English → sources.md](sources.md)

Her kişi için **bir** kaynak doldurun. İlk WiFi kurulumunda veya sonra cihazın web sayfasından girebilirsiniz (cihazda *Ayarlar → QR / Web* adresi ve QR'ı gösterir; web şifresi cihazın *Bilgi* ekranında yazar).

MeteMatik şeker verisini yalnızca **okur**. Hesabınıza hiçbir şey yazmaz.

---

## Nightscout

1. **URL:** site adresiniz, örneğin `https://adiniz.ornek.com` (sona `/` gerekmez).
2. **API Secret / erişim anahtarı:**
   - Siteniz herkese açıksa (giriş yapmadan grafiği görebiliyorsanız) **boş bırakın**.
   - Açık değilse bir erişim anahtarı oluşturun: Nightscout → *Admin Tools* → *Add new Subject* → rol **readable** → **access token**'ı (örneğin `metematik-1a2b3c4d5e6f7g8h`) kopyalayıp yapıştırın.
   - Lütfen ana `API_SECRET`'ı **girmeyin** — sadece okuma izinli anahtar daha güvenlidir.
3. Sitenin https sertifikası geçerli olmalıdır.

Ek olarak: IOB / COB / son bolus'lar (eklentiler açıksa), sensör ve set süresi (*SAGE* / *CAGE*).

### Medtronic (CareLink)
MeteMatik CareLink'e doğrudan bağlanmaz. CareLink verisini Nightscout'a aktarıp (örneğin Nightscout'un *connect* özelliği / *nightscout-connect* ile) Nightscout kaynağını kullanabilirsiniz. Bu araçların kendi kılavuzlarını izleyin; Medtronic kullanım şartlarıyla çelişebilirler.

---

## Dexcom (G6 / G7 / ONE+)

1. **Kendi Dexcom hesabınızı** kullanın — CGM kullanan kişinin telefonundaki Dexcom uygulamasına giriş yapılan hesap (Follow uygulamasının hesabı **değil**).
2. Dexcom uygulamasında **Paylaşım (Share)** açık olmalı ve **en az bir takipçi** ekli olmalı (takipçinin uygulamayı kullanması gerekmez).
3. **Kullanıcı adı:** kullanıcı adı, e-posta veya telefon. Hesap telefonla açıldıysa ülke koduyla ve boşluksuz yazın: `+905XXXXXXXXX`.
4. **Sunucu:** ABD dışı (Avrupa, Türkiye…) için `ous`, ABD için `us`.

*"24 saattir veri yok"* görürseniz: Paylaşım açık mı ve takipçi var mı kontrol edin.

---

## FreeStyle Libre (LibreLinkUp)

1. CGM kullanan kişinin telefonundaki **LibreLink / Libre** uygulamasından takipçi davet edin: *Bağlı uygulamalar → LibreLinkUp → Bağlantı ekle*.
2. Herhangi bir telefona **LibreLinkUp** kurun, davet edilen e-postayla hesap açın ve daveti **kabul edin**.
3. MeteMatik'e **LibreLinkUp e-posta ve şifresini** girin. Bölge: `EU` (cihaz sunucunun yönlendirdiği bölgeye kendisi geçer).

*"Yeni şartları kabul edin"* görürseniz: LibreLinkUp uygulamasını bir kez açıp yeni şartları kabul edin, sonra cihazda *Yenile*'ye basın.

Not: LibreLinkUp son 12 saatin grafiğini verir; cihaz zamanla kendi kaydıyla 24 saate tamamlar. LibreLinkUp çift ok bilgisi vermediği için hızlı düşüş / yükseliş alarmı 5 dakikalık değişime göre çalışır.

---

## Medtrum (EasyFollow)

1. **EasyView / Medtrum** uygulamasından takipçi davet edin (*Takip / Paylaş*).
2. O e-postayla **EasyFollow** hesabı açıp daveti kabul edin.
3. MeteMatik'e **EasyFollow e-posta ve şifresini** girin. Sunucu: Avrupa (`easyview.medtrum.eu`).

---

## Aile modu (2–3 kişi)

1. Cihazın web sayfası → **Aile modu** kartı (*Güvenlik*'in üstünde) → *Durum: Açık* → kişi sayısı → adlar → *Kaydet*.
2. Sayfanın üstünden bir kişi seçin → o kişinin kaynağını ve eşiklerini girin → *Kaydet*. Her kişi için tekrarlayın.
3. Her kişi farklı kaynak kullanabilir (örneğin biri Dexcom, biri Libre).

---

## Sorunlar

| Cihazdaki mesaj | Ne yapmalı |
|---|---|
| WiFi yok / *BOOT 2 sn* | Modem açık ve yakın mı? **BOOT** tuşunu 2 saniye basılı tutmak WiFi / kaynak kurulumunu açar. Kayıtlı ağlar silinmez. |
| Giriş hatası | Kullanıcı adı veya şifre yanlış olabilir. Web sayfasından yeniden girin. Hesabınız kilitlenmesin diye cihaz seyrek dener. |
| İnternet yok | WiFi bağlı ama modemin interneti yok. Veri kendiliğinden geri gelir. |
| Kaynak yok | Bir kaynak girin (kurulum veya web sayfası). |
| Nightscout'a ulaşılamadı | Adresi ve sitenin açık olduğunu kontrol edin; https sertifikası geçerli olmalı. |

Çözemediyseniz **metematik.t1d@gmail.com** adresine yazın: cihaz sürümü (*Bilgi* ekranı), kaynak türü, ülke ve ekranın fotoğrafı. **Şifrelerinizi asla göndermeyin.**
