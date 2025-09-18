# Sump Pump Pro — ESP32 + BMS BLE (Eco-Worthy) + LCD 16×2

Petit projet autonome basé sur ESP32 qui se connecte en Bluetooth Low Energy (BLE) au BMS Eco-Worthy et affiche en temps réel :

- Tension (V)  
- Courant (A)  
- État de charge (SOC, %)  
- Puissance (W)

Affichage sur un LCD I²C 16×2 (PCF8574).  
**Indicateur lumineux** via un **pixel WS2812B** (NeoPixel) pour l’état du système.

---

## Fonctionnalités

- Connexion BLE sans PIN au BMS (profil propriétaire type JBD).  
- Découverte automatique des services **FF00** (fallback **FFF0 / FFE0**).  
- Envoi périodique de la requête JBD “basic info” `DD A5 03 00 FF FD 77`.  
- Parsing de la réponse pour extraire V, I, SOC et calcul **W = V × I**.  
- Affichage propre sur **LCD 16×2** :  
  - Ligne 1 : `13.1V  0.00A`  
  - Ligne 2 : `SOC:  78%   150W`
- **LED WS2812B (1 pixel) :**
  - **Bleu pulsé (lent)** = en connexion / pas encore connecté.
  - **Vert pulsé (rapide)** = **en charge** (W > 5).
  - **Vert fixe (~15 % de brightness)** = connecté, pas en charge.

---

## Matériel

- ESP32 (actuellement ESP-32-WROOM-32D DevKit).  
- Écran LCD 16×2 I²C (carte PCF8574, adresse 0x27 la plupart du temps, parfois 0x3F).  
- Batterie 12 V Eco-Worthy / BMS JBD annoncée en BLE (ex. nom `DP04S…`).  
- **1 × LED WS2812B** (NeoPixel) **5 V**.  
- Câbles Dupont, **résistance 330–470 Ω** (série DATA), **condensateur 1000 µF/≥6,3 V** (recommandés entre 5 V et GND).

> Important : fermer l’app mobile du BMS pendant l’utilisation (une seule connexion BLE à la fois).

---

## Câblage du LCD

LCD I²C → ESP32 :

| LCD | ESP32 |
|-----|-------|
| VCC | 5V    |
| GND | GND   |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

Adresse I²C par défaut : 0x27 (ou 0x3F sur certains modules).

---

## Câblage de la LED WS2812B

| WS2812B | ESP32 / Alim |
|---------|---------------|
| **DIN** | **GPIO 5** (via **330–470 Ω** en série) |
| **VCC** | **5V** |
| **GND** | **GND** (commun avec l’ESP32) |

> Recommandé : condensateur **1000 µF** entre 5 V et GND près de la LED.  
> Les WS2812B tolèrent souvent un DATA 3.3 V si les fils sont courts. Si instable, utiliser un **level shifter 3.3→5 V** (ex. 74AHCT125).

---

## Protocole BLE ciblé

- Publicité vue : Service **0xFF00** (souvent utilisé par les BMS JBD/Eco-Worthy).  
- Caractéristiques typiques :
  - Notify : `FF01` (fallback `FFF1` / `FFE1`)  
  - Write : `FF02` (fallback `FFF2` / `FFE2`)
- Requête cyclique : `DD A5 03 00 FF FD 77` (JBD “basic info”).  
- Échelles : la plupart des firmwares renvoient V et I en **centi-unités (÷100)**, pas en milli (÷1000).

---

## Logiciels & bibliothèques

- Arduino IDE 2.3.6.  
- ESP32 Arduino Core : **3.3.0**.  
- **Aucune lib BLE externe** : on utilise la pile BLE intégrée (`BLEDevice.h`).  
  - **Important** : ne pas installer `ESP32_BLE_Arduino` (Kolban), elle entre en conflit.  
- `LiquidCrystal_PCF8574` (Matthias Hertel) — *Library Manager*.  
- **`Adafruit NeoPixel`** — *Library Manager* (pour la LED WS2812B).

---

## Configuration dans le code

En haut du sketch :

```cpp
// MAC BLE trouvée au scan (insensible à la casse)
static const char* TARGET_BLE_MAC   = "a5:c2:37:5d:85:67";
// Filtre nom si on doit rescanner
static const char* TARGET_NAME_HINT = "DP04S";

// LCD I²C : 0x27 par défaut, 0x3F sur certains modules
LiquidCrystal_PCF8574 lcd(0x27);

// LED WS2812B (1 pixel)
#define LED_PIN   5
#define LED_COUNT 1
// Adafruit_NeoPixel px(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
```

Comportement LED implémenté dans le code :
- **Bleu pulsé lent** quand non connecté / en cours de connexion.
- **Vert pulsé rapide** si **P = V × I > 5 W** (en charge).
- **Vert fixe** (brightness ≈ 15 %) sinon.

---

## Compilation & flash (Windows pas à pas)

1. IDE Arduino → **Outils → Type de carte → ESP32 → ESP32 Dev Module**.  
2. Réglages conseillés :  
   - CPU Frequency : *240MHz (WiFi/BT)*  
   - Flash Size : *4MB (32Mb)*  
   - Upload Speed : *921600* (ou *115200* si câble limite)
3. **Croquis → Gérer les bibliothèques…**  
   - Installer **LiquidCrystal_PCF8574** et **Adafruit NeoPixel**.  
   - Vérifier qu’il **n’y a pas** `ESP32_BLE_Arduino` (Kolban) dans `Documents/Arduino/libraries`.
4. Coller le sketch, configurer la **MAC BLE** et vérifier l’**adresse I²C** du LCD.  
5. Téléverser et ouvrir le **Moniteur Série** (115200).

Attendu au boot :

```
[BLE] Connexion directe a a5:c2:37:5d:85:67 ...
[BLE] Connecte (direct).
[BLE] Notifs OK
TX
TX
...
```

LCD :

```
13.1V   0.00A
SOC:  78%   150W
```

LED :
- Bleu pulsé → en connexion.  
- Vert pulsé rapide → charge (W > 5).  
- Vert fixe → connecté (pas en charge).

---

## Ajustements utiles

**Affichage (UI)**

```cpp
// Ligne 1 : 1 décimale sur V, 2 sur I
snprintf(l1, sizeof(l1), "%5.1fV  %5.2fA", gV, gI);

// Ligne 2 : SOC + W
float P = gV * gI;
// Remplacer fabsf(P) par P si vous voulez garder le signe (charge/décharge)
snprintf(l2, sizeof(l2), "SOC:%3d%%  %4dW", gSOC, (int)lroundf(fabsf(P)));
```

**Échelle V/I**

```cpp
V = mv / 100.0f;
I = ma / 100.0f;
```

**Seuil “charge” pour la LED**  
Dans le code, `Praw > 5.0f` (W). Adaptez ce seuil selon votre bruit de mesure.

---

## Dépannage rapide

- **Scan BLE = 0 périphériques** : câble USB/port, module ESP32, ou lib Kolban présente → la supprimer.  
- **Pas de connexion** : fermer l’app mobile, vérifier que c’est bien la **MAC BLE**, rapprocher l’ESP32 (20–50 cm).  
- **LED instable** : ajouter la **résistance série** et le **condensateur** ; réduire la longueur des fils ; envisager un level shifter.
