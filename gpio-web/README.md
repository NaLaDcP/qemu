# GPIO Web Interface

Interface web pour observer et piloter les GPIO du contrôleur MPC8xxx émulé par QEMU,
via des message queues POSIX partagées entre containers Docker.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Container qemu  (IPC namespace : shareable)                    │
│                                                                 │
│   firmware aarch64                                              │
│       │ écrit registres GPIO (dir, odr, dat)                    │
│       ▼                                                         │
│   hw/gpio/mpc8xxx.c                                             │
│       │ mq_send → /from_qemu   (O_WRONLY | O_NONBLOCK)         │
│       │ mq_recv ← /to_qemu     (O_RDONLY)                      │
│       │                                                         │
│   [remote_gpio_thread]  ←──── reçoit PIN_UPDATE / QUERY        │
│                                                                 │
└──────────────────┬──────────────────────────────────────────────┘
                   │  POSIX mqueue  (IPC namespace partagé)
┌──────────────────┴──────────────────────────────────────────────┐
│  Container gpio-web  (IPC namespace : service:qemu)             │
│                                                                 │
│   gpio_server.js                                                │
│       │ mq_recv ← /from_qemu   (O_RDONLY | O_NONBLOCK, poll)   │
│       │ mq_send → /to_qemu     (O_WRONLY)                       │
│       │                                                         │
│       │ HTTP :8080  →  gpio_ui.html                             │
│       │ WebSocket   ←→  navigateur                              │
│                                                                 │
└──────────────────┬──────────────────────────────────────────────┘
                   │  WebSocket ws://localhost:8080
              Navigateur
              gpio_ui.html
```

**Flux de données :**

| Direction | Chemin |
|-----------|--------|
| QEMU → UI | `mpc8xxx.c` → `/from_qemu` → `gpio_server.js` → WebSocket → navigateur |
| UI → QEMU | navigateur → WebSocket → `gpio_server.js` → `/to_qemu` → `mpc8xxx.c` |

---

## Protocole mqueue

Les deux queues échangent des messages binaires de **12 octets** :

```
offset  taille  champ    description
──────  ──────  ───────  ──────────────────────────────────────────
0       uint16  magic    Toujours 0xABCD (little-endian)
2       uint8   type     Type de message (voir tableau ci-dessous)
3       uint8   pad      Padding, toujours 0
4       uint32  pin      PIN_UPDATE : numéro de pin (0–31)
                         REG_DUMP   : offset du registre
8       uint32  state    PIN_UPDATE : état 0 ou 1
                         REG_DUMP   : valeur du registre
```

**Types de messages :**

| type | valeur | émetteur | description |
|------|--------|----------|-------------|
| `PIN_UPDATE` | 0 | QEMU ou serveur | Changement d'état d'une pin |
| `REG_DUMP`   | 1 | QEMU            | Valeur d'un registre GPIO |
| `QUERY`      | 2 | serveur         | Demande un dump de tous les registres |

**Registres GPIO exposés via REG_DUMP :**

| offset | nom | rôle |
|--------|-----|------|
| 0x00 | `dir` | Direction : bit=1 → sortie, bit=0 → entrée |
| 0x04 | `odr` | Open-drain : bit=1 → pin en open-drain |
| 0x08 | `dat` | Données courantes |
| 0x0C | `ier` | Interrupt Event Register |
| 0x10 | `imr` | Interrupt Mask Register |
| 0x14 | `icr` | Interrupt Control Register |

> Le bit de poids fort (bit 31) correspond à la pin 0, le bit 0 à la pin 31.

---

## Fichiers

### `hw/gpio/mpc8xxx.c` (QEMU)

Pilote du contrôleur GPIO MPC8xxx, modifié pour la communication IPC.

**Points clés :**
- `/from_qemu` ouvert en `O_WRONLY | O_NONBLOCK` — un `mq_send` sur une queue pleine
  échoue silencieusement plutôt que de bloquer le thread I/O de QEMU.
- `/to_qemu` ouvert en `O_RDONLY` bloquant — `remote_gpio_thread` attend un message.
- `remote_gpio_thread` tourne en parallèle : reçoit les `PIN_UPDATE` du serveur
  (appelle `mpc8xxx_gpio_set_irq` sous le BQL) et répond aux `QUERY` avec `send_all_regs`.
- `mpc8xxx_gpio_write` envoie un `REG_DUMP` chaque fois que `dir` ou `odr` est écrit
  par le firmware.

### `gpio_server.js`

Pont Node.js entre les message queues POSIX et les clients WebSocket.

**Démarrage :**
1. Lecture optionnelle de `/config/pins.json` (noms des pins).
2. `tryConnect()` tente d'ouvrir `/from_qemu` (O_RDONLY|O_NONBLOCK) et `/to_qemu`
   (O_WRONLY). En cas d'échec, retry toutes les 3 secondes.
3. À la connexion : envoie un message `QUERY` pour obtenir l'état initial des registres,
   puis démarre `setInterval(poll, 20ms)`.
4. `poll()` vide la queue `/from_qemu` en boucle non-bloquante à chaque tick.

**Messages WebSocket émis vers le navigateur :**

| type | payload | déclencheur |
|------|---------|-------------|
| `pin_names` | `{ names: { "0": "LED", ... } }` | à chaque nouvelle connexion WS |
| `qemu_status` | `{ connected: bool }` | changement d'état de connexion |
| `pin_update` | `{ pin, state }` | réception d'un PIN_UPDATE de QEMU |
| `reg_update` | `{ offset, name, value }` | réception d'un REG_DUMP de QEMU |
| `error` | `{ msg }` | message invalide reçu du navigateur |

**Messages WebSocket reçus du navigateur :**

```json
{ "pin": 5, "state": 1 }
```

Transmis à QEMU comme `PIN_UPDATE` sur `/to_qemu`.

### `gpio_ui.html`

Interface web servie statiquement par `gpio_server.js`.

**Comportement des pins :**

| état de la pin | apparence |
|----------------|-----------|
| Direction inconnue | fond neutre, badge `?` |
| Entrée (IN) OFF | fond sombre |
| Entrée (IN) ON | fond jaune |
| Sortie (OUT) OFF | gris atténué (non cliquable) |
| Sortie (OUT) ON | orange vif (non cliquable) |

Cliquer sur une pin **entrée** envoie un `PIN_UPDATE` au serveur → QEMU reçoit
le changement de niveau et déclenche l'interruption correspondante.

---

## Configuration des pins

Créer un fichier JSON avec comme clés les numéros de pins (0–31) :

```json
{
  "0":  "LED_RED",
  "1":  "LED_GREEN",
  "5":  "BTN_RESET",
  "16": "UART_TX",
  "17": "UART_RX"
}
```

Voir `pins.json.example` pour un modèle complet.

---

## Démarrage avec Docker

### Prérequis

Copier `.env.example` vers `.env` et renseigner les variables :

```ini
# Image kernel à démarrer dans QEMU
IMAGE_DIR=/chemin/vers/le/build
IMAGE_FILENAME=loader.img

# Fichier de noms de pins (optionnel)
PINS_FILE=/chemin/vers/mon/pins.json
```

### Lancer

```bash
cd gpio-web
docker compose up --build
```

L'interface est disponible sur **http://localhost:8080**.

### Rebuild partiel

| Modification | Commande |
|---|---|
| `mpc8xxx.c` | `docker compose up --build qemu` |
| `gpio_server.js` ou `gpio_ui.html` | `docker compose up --build gpio-web` |
| `pins.json` | Aucun rebuild — redémarrer `docker compose restart gpio-web` |

### Partage IPC entre containers

Le partage des message queues POSIX repose sur le partage du namespace IPC Linux :

```yaml
qemu:
  ipc: shareable      # expose son namespace IPC

gpio-web:
  ipc: service:qemu   # rejoint le namespace IPC de qemu
```

Les queues `/from_qemu` et `/to_qemu` créées par QEMU sont ainsi visibles
dans le container `gpio-web`.

---

## Dépendances Node.js

| paquet | rôle |
|--------|------|
| `koffi` | FFI vers `librt`/`libc` pour appeler `mq_open`, `mq_send`, `mq_receive` |
| `ws` | Serveur WebSocket |
