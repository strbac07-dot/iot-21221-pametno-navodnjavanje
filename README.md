# Pametno navodnjavanje s detekcijom vlage tla

Projekat za predmet **Dizajn i razvoj IoT projekata (IoT_21221)**.

ESP32 čita vlagu tla i temperaturu/vlažnost zraka, šalje podatke u oblak
preko MQTT-a i HTTP-a, a pumpa se automatski uključuje kad je tlo presuho.

---

## 1. Šta je u projektu

| Komponenta | Simulacija (Wokwi) | Stvarni hardver |
|---|---|---|
| ESP32 DevKit C v4 | `board-esp32-devkit-c-v4` | ESP32 DevKit |
| Senzor vlage tla | potenciometar (GPIO34) | kapacitivni senzor vlage |
| Temperatura/vlažnost | DHT22 (GPIO15) | DHT22 |
| Pumpa (indikator) | plava LED (GPIO26) | relej + pumpa |
| Alarm | crvena LED (GPIO27) | buzzer / LED |

Tok sistema:

```
[senzor vlage]──ADC──┐
[DHT22]────GPIO──────┤
                     ├─► [ESP32] ──WiFi──► [HiveMQ Cloud] ──► [dashboard / app]
[LED pumpa]◄──GPIO───┘                        │
[LED alarm]◄──GPIO────                        └──► [ThingSpeak] (grafovi)
```

## 2. Struktura projekta

```
iot-21221/
├── platformio.ini          # konfiguracija build-a i biblioteka
├── wokwi.toml              # putanja do firmware-a za Wokwi ekstenziju
├── diagram.json            # šema spajanja za Wokwi
├── libraries.txt           # lista biblioteka (za Wokwi web editor)
├── README.md               # ovaj fajl
├── scripts/
│   └── wokwi_extra.py
└── src/
    ├── main.cpp            # glavni program
    └── secrets.h           # <-- TVOJI podaci (WiFi, MQTT, ThingSpeak)
```

## 3. Pokretanje u Visual Studio Code

### 3.1. Instaliraj alate

1. **Visual Studio Code** — https://code.visualstudio.com/
2. U VS Code otvori *Extensions* i instaliraj:
   - **PlatformIO IDE** (autor: PlatformIO)
   - **Wokwi Simulator** (autor: Wokwi)
3. Sačekaj da PlatformIO završi instalaciju (prvi put traje par minuta).

### 3.2. Otvori projekat

1. Raspakuj ovaj zip.
2. VS Code → *File → Open Folder…* → izaberi folder `iot-21221`.
3. PlatformIO će sam prepoznati `platformio.ini` i ponuditi da instalira
   toolchain i biblioteke. Ako ne ponudi, klikni na ikonu PlatformIO u
   lijevoj traci → *Project Tasks*.

### 3.3. Popuni svoje podatke

Otvori `src/secrets.h` i zamijeni:

- `MQTT_HOST` — Cluster URL sa HiveMQ Cloud
- `MQTT_USER`, `MQTT_PASS` — kredencijali koje si napravila u HiveMQ
- `TS_API_KEY` — Write API Key iz ThingSpeak kanala
- (za pravi ESP32) `WIFI_SSID` i `WIFI_PASS`

Wokwi koristi svoju virtuelnu mrežu (`Wokwi-GUEST`), pa za simulaciju ne
trebaš mijenjati WiFi.

### 3.4. Build i simulacija

1. Otvori `src/main.cpp`.
2. Klikni **Build** (kvačica u PlatformIO statusnoj traci) da provjeriš da
   se kod kompajlira.
3. Pritisni **F1** → ukucaj `Wokwi: Start Simulator`.
4. Otvori *Serial Monitor* (115200 baud) i prati ispis.

Ako Wokwi traži licence: besplatna licenca se aktivira komandom
`Wokwi: Request a New License` (F1 meni) — dovoljna je za ovaj projekat.

## 4. Cloud postavke

### HiveMQ Cloud (MQTT broker)

1. Registracija: https://console.hivemq.cloud/
2. *Create New Cluster* → **Serverless** → izaberi regiju.
3. *Access Management* → *Add credentials* → upiši username i lozinku.
4. Kopiraj **Cluster URL** (npr. `abc123.s1.eu.hivemq.cloud`) u `secrets.h`.
5. Za provjeru: *Web Client* u konzoli → pretplati se na `irrigation/zone1/#`.

### ThingSpeak (grafovi i historija)

1. Registracija: https://thingspeak.com/
2. *Channels → New Channel* → uključi 4 polja:
   - Field 1: vlaga tla (%)
   - Field 2: temperatura (°C)
   - Field 3: vlažnost zraka (%)
   - Field 4: stanje pumpe (0/1)
3. *API Keys* → kopiraj **Write API Key** u `secrets.h`.

Napomena: besplatni ThingSpeak dozvoljava upis svakih **15 sekundi**, zato
je interval slanja u kodu postavljen na 20 s.

## 5. Dashboard

### Varijanta A — IoT MQTT Panel (Android, besplatno)

Instaliraj **IoT MQTT Panel**, pa dodaj konekciju prema HiveMQ brokeru
(host, port 8883, SSL, username, password). Zatim dodaj panele:

| Panel | Tip | Topic | JSON polje |
|---|---|---|---|
| Vlaga tla | Gauge | `irrigation/zone1/telemetry` | `soil` |
| Temperatura | Gauge | `irrigation/zone1/telemetry` | `temp` |
| Vlažnost zraka | Gauge | `irrigation/zone1/telemetry` | `hum` |
| Pumpa | LED / Switch | `irrigation/zone1/status` | `pump` |
| Ručno: upali | Button | `irrigation/zone1/cmd` | payload `PUMP_ON` |
| Ručno: ugasi | Button | `irrigation/zone1/cmd` | payload `PUMP_OFF` |
| Auto režim | Button | `irrigation/zone1/cmd` | payload `AUTO` |

### Varijanta B — HiveMQ Web Client

Najbrže za provjeru: u HiveMQ konzoli otvori *Web Client*, pretplati se na
`irrigation/zone1/#` i gledaj poruke kako stižu svakih 5 sekundi.

## 6. Kako logika radi

- Senzor vlage se čita 10× i usrednjava (smanjuje šum), pa se preslikava
  u 0–100 %.
- **Histereza:** pumpa se uključi kad vlaga padne **ispod 35 %**, a isključi
  kad poraste **iznad 60 %**. Između tih vrijednosti stanje se ne mijenja —
  tako pumpa ne "trepće" oko jednog praga.
- **Sigurnosni timeout:** pumpa se automatski gasi nakon 60 s rada.
- **Ručni režim:** komanda `PUMP_ON`/`PUMP_OFF` prebacuje u MANUAL i
  ignoriše automatiku; `AUTO` vraća automatski režim.
- **Alarm:** crvena LED svijetli ako temperatura pređe 38 °C.
- Pragovi se mogu mijenjati u hodu preko topica `irrigation/zone1/config`,
  npr. `{"dry":30,"wet":55}`.

## 7. Plan testiranja

**Test 1 — senzor.** U Wokwiju okreni potenciometar tako da vlaga padne
ispod 35 % → plava LED treba da se upali. Podigni iznad 60 % → LED se gasi.
U zoni 35–60 % stanje se ne mijenja (dokaz histereze).

**Test 2 — prijenos.** U HiveMQ Web Clientu pretplati se na
`irrigation/zone1/#` i provjeri da telemetrija stiže svakih ~5 s.

**Test 3 — dashboard.** Pošalji `PUMP_ON` sa telefona/paneela i provjeri da
se LED u simulaciji upali unutar sekunde, a statusna poruka promijeni.

**Test 4 — ThingSpeak.** Nakon ~20 s provjeri da kanal dobija nove zapise u
sva 4 polja.

**Test 5 — sigurnost.** Ostavi pumpu upaljenu duže od 60 s → mora se sama
ugasi razlogom "sigurnosni timeout".

**Test 6 — tačnost.** Zabilježi 20 parova (postavljena vrijednost na
potenciometru ↔ prikazani %) i izračunaj srednju apsolutnu grešku.

**Test 7 — stabilnost.** Pusti simulaciju 30 minuta, broji prekinute MQTT
konekcije i izgubljene poruke.

## 8. Česti problemi

| Simptom | Uzrok / rješenje |
|---|---|
| `MQTT greska rc=-2` | pogrešan host/port ili TLS; provjeri Cluster URL i 8883 |
| `rc=4` ili `rc=5` | pogrešan username/lozinka u HiveMQ |
| `rc=-4` | WiFi nije spojen (u Wokwiju mora biti `Wokwi-GUEST`) |
| DHT vraća `nan` | prvo čitanje je uvijek neispravno; kod preskače `nan` |
| ThingSpeak HTTP 0 | nema interneta ili pogrešan API ključ |
| LED ne svijetli | provjeri da je otpornik 220 Ω između katode i GND |
| Build puca na `secrets.h` | fajl mora biti u `src/`, ne u korijenu |

## 9. Linkovi

- Wokwi: https://wokwi.com/
- HiveMQ Cloud: https://console.hivemq.cloud/
- ThingSpeak: https://thingspeak.com/
- PlatformIO: https://platformio.org/
- IoT MQTT Panel: https://play.google.com/store/apps/details?id=com.ravendmaster.linearmqttdashboard
