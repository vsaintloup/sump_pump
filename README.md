# Sump Pump Pro — ESP32 + BMS BLE (Eco-Worthy/JBD) + LCD 16×2

Petit projet autonome basé sur ESP32 qui se connecte en Bluetooth Low Energy (BLE) au BMS Eco-Worthy et affiche en temps réel :

- Tension (V)  
- Courant (A)  
- État de charge (SOC, %)  
- Puissance (W)

Affichage sur un LCD I²C 16×2 (PCF8574).

---

## Fonctionnalités

- Connexion BLE sans PIN au BMS (profil propriétaire type JBD).  
- Découverte automatique des services FF00 (et fallback FFF0 / FFE0).  
- Envoi périodique de la requête JBD “basic info” `DD A5 03 00 FF FD 77`.  
- Parsing de la réponse pour extraire V, I, SOC et calcul W = V × I.  
- Affichage propre sur LCD 16×2 :  
  - Ligne 1 : `13.1V  0.00A`  
  - Ligne 2 : `SOC:  78%   150W`

---

## Matériel

- ESP32 (actuellement ESP-32-WROOM-32D DevKit).  
- Écran LCD 16×2 I²C (carte PCF8574, adresse 0x27 la plupart du temps, parfois 0x3F).  
- Batterie 12 V Eco-Worthy / BMS JBD annoncée en BLE (ex. nom `DP04S…`).  
- Câbles Dupont.

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

## Protocole BLE ciblé

- Publicité vue : Service 0xFF00 (souvent utilisé par les BMS JBD/Eco-Worthy).  
- Caractéristiques typiques :
  - Notify : `FF01` (fallback `FFF1` / `FFE1`)  
  - Write : `FF02` (fallback `FFF2` / `FFE2`)
- Requête cyclique : `DD A5 03 00 FF FD 77` (JBD “basic info”).  
- Échelles : la plupart des firmwares renvoient V et I en centi-unités (÷100), pas en milli (÷1000).

---

## Logiciels & bibliothèques

- Arduino IDE 1.8.x (je n’ai pas encore essayé avec 2.x).  
- ESP32 Arduino Core : 3.3.0.
- Aucune lib BLE externe à installer : on utilise la pile BLE intégrée (`BLEDevice.h`) **important** les autres lib BLE (comme ESP32_BLE_Arduino) ne fonctionnent pas bien.  
- LiquidCrystal_PCF8574 (Matthias Hertel) via *Library Manager*.

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
```

Si LCD est en 0x3F, remplacer `0x27` par `0x3F`.

---

## Compilation & flash (Windows pas à pas)

1. IDE Arduino → Outils → Type de carte → ESP32 → ESP32 Dev Module.  
2. Réglages conseillés :  
   - CPU Frequency : *240MHz (WiFi/BT)*  
   - Flash Size : *4MB (32Mb)*  
   - Upload Speed : *921600* (ou *115200* si câble limite)
3. Croquis → Gérer les bibliothèques…  
   - Installez LiquidCrystal_PCF8574.  
   - Vérifier qu’il n’y a pas `ESP32_BLE_Arduino` (Kolban) dans `Documents/Arduino/libraries`.
4. Collez le sketch.  
5. Téléverser et ouvrez le Moniteur Série (115200).

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

---

## Comment récupérer la MAC BLE de la batterie ?

Utiliser un scan BLE ou une app comme nRF Connect pour repérer :
- Adresse BLE (ex. `a5:c2:37:5d:85:67`)
- Nom (ex. `DP04S007L4S100A`)
- (Optionnel) Service annoncé (ex. `0xFF00`)

Sur Linux j’ai simplement utilisé **bluetoothctl** puis **scan on**.

---

## Ajustements utiles

Affichage (UI)

```cpp
// Ligne 1 : 1 décimale sur V, 2 sur I
snprintf(l1, sizeof(l1), "%5.1fV  %5.2fA", gV, gI);

// Ligne 2 : SOC + W (valeur absolue par défaut)
float P = gV * gI;
// Remplacer fabsf(P) par P si vous voulez garder le signe (charge/décharge)
snprintf(l2, sizeof(l2), "SOC:%3d%%  %4dW", gSOC, (int)lroundf(fabsf(P)));
```

Échelle V/I

Dans les parseurs, on utilise ÷100 (centi-unités) :
```cpp
V = mv / 100.0f;
I = ma / 100.0f;
```
Si votre firmware renvoie autre chose, adaptez (÷1000 pour milli-unités, etc.).

Offset SOC

Par défaut `SOC = payload[19]`. Certains firmwares déplacent l’octet (ex. `payload[21]`).  
Si le SOC semble faux, logguez la trame reçue en hex et ajustez l’offset.
