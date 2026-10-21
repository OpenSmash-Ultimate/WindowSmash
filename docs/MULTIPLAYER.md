# Multiplayer

[← README](../README.md) · [Setup guide](BUILDING.md) · [Playing](PLAYING.md) · [Linux](LINUX.md) · [Technical guide](TECHNICAL.md)

WindowSmash plays the game's **Local Wireless** mode over the internet. To the game, the other players look like Switches nearby. The game's own **Online** mode works too, for Quickplay against WindowSmash's own game server ([Online mode](#online-mode-quickplay)). Nintendo's online service isn't used.

> [!NOTE]
> **Tested so far:** connecting and finding rooms, including the online lobby with several games on one PC, through the public lobby, and between a Windows and a Linux PC. Full matches between two PCs, every DLC fighter online, and every kind of home network haven't been fully verified yet. Online mode is newer still: see [what works](#online-mode-quickplay).

## Contents

- [Choose how to play](#choose-how-to-play)
- [Before you play](#before-you-play)
- [Online lobby](#online-lobby)
- [Online mode (Quickplay)](#online-mode-quickplay)
- [Direct connections and your internet address](#direct-connections-and-your-internet-address)
- [Private room](#private-room)
- [If a room doesn't appear](#if-a-room-doesnt-appear)
- [Run your own lobby](#run-your-own-lobby)
- [Logs and privacy](#logs-and-privacy)
- [Reference: environment variables](#reference-environment-variables)

## Choose how to play

| Way | Best for | Router setup | Start with |
| :--- | :--- | :--- | :--- |
| [**Online lobby**](#online-lobby) | Playing with anyone | None | `Play SSBU.bat` |
| [**Lobby code**](#friends-only-a-lobby-code) | Friends only, through the lobby | None | `Play SSBU.bat` |
| [**Online mode**](#online-mode-quickplay) | Quickplay with anyone searching (new) | None | `Play SSBU.bat` |
| [**Private room**](#private-room) | A group with one host | The host may need to open a port | `launchers\Host SSBU Netplay.bat` and `launchers\Join SSBU Netplay.bat` |
| [**Your own lobby**](#run-your-own-lobby) | Communities with a server | The server's port | `Play SSBU.bat` |

The online lobby is the easiest: nobody hosts anything outside the game, and nobody touches their router.

On Linux ([Linux guide](LINUX.md)), `run.sh`, `run-offline.sh`, `host-netplay.sh` and `join-netplay.sh` do what `Play SSBU.bat`, `launchers\Play SSBU Offline.bat`, `launchers\Host SSBU Netplay.bat` and `launchers\Join SSBU Netplay.bat` do. Windows and Linux players play together.

## Before you play

Whichever way you choose:

1. **Everyone uses the same WindowSmash, with the game at version 13.0.5.** WindowSmash builds no other version, so every player can play with every other. The online lobby turns away a game an older WindowSmash built from another version: its overlay says `Netplay: WindowSmash needs game 13.0.5: run the installer again` ([what to do](UPDATES.md#a-folder-built-from-another-version)).
2. **Every PC is set up with its own game data**, following the [setup guide](BUILDING.md). Match the DLC between players too.
3. **Every PC keeps its own save and its own `netplay_id.txt`.** Never copy `netplay_id.txt` to another PC: two PCs with the same one look like one player.
4. **Check the game works offline first** with `launchers\Play SSBU Offline.bat`, then close it.

Each WindowSmash window is one player. A private room holds up to eight members, and the game's own room rules decide how many can play a match.

## Online lobby

This is what `Play SSBU.bat` does by default. Every Local Wireless room that anyone creates shows up for every player in the lobby.

1. Double-click **`Play SSBU.bat`**. The first time, the game picks the name other players see, such as `Player1234`, and shows it at the bottom of the screen. To change it, press <kbd>Esc</kbd> in the game, go to **Online**, choose **Your name**, then **Game → Restart the game** ([settings](PLAYING.md#settings)).
2. If Windows Security asks about `ssbu.exe`, click **Allow**. No port forwarding is needed: your game only sends to the lobby.
3. In the game's main menu, choose **Local Wireless**.
   - **To host:** choose **Create a Room**. When a challenger appears, accept them.
   - **To join:** open **Rooms List** and pick a room. Open rooms come first, fuller ones before emptier ones.
4. Press <kbd>F1</kbd> to see the connection, for example `Online: 37 players, 4 rooms - you are Alpha, ping 48 ms`. A second line lists the other players in your room and how each one is reached ([see below](#direct-connections-and-your-internet-address)).

`online_server.txt`, in the WindowSmash folder, names the lobby. Its first line that doesn't start with `#` is the lobby's address, `smash.recomps.wtf:39421` (the public lobby) unless you change it. Port 39421 is the lobby with the game server for [Online mode](#online-mode-quickplay); port 39420 is the older lobby without it, which older WindowSmash versions use. Rooms on one don't show up on the other, so everyone in a group should use the same one.

### Friends only: a lobby code

A lobby code hides your rooms from everyone who doesn't use the same code.

1. Press <kbd>Esc</kbd> in the game and go to **Online**.
2. Set **Play online in** to **private lobby**, choose **Lobby code**, and type your code.
3. Choose **Restart now**. Everyone in your group does the same, with the same code.

The window `Play SSBU.bat` opens before the game does the same: set **Lobby** to **Private lobby (a code)** and type your code.

Capital letters count: `OurCode` and `ourcode` are two different lobbies.

Or, with the game closed, add a space and your code after the address in `online_server.txt`, for example:

```text
smash.recomps.wtf:39421 ourcode
```

The overlay shows the code after `Online`. A lobby code is not a password: anyone who knows it can join.

### Playing offline

- `launchers\Play SSBU Offline.bat` never goes online.
- To make `Play SSBU.bat` start offline too, set **Play online in** to **offline** in the settings (**Online** page), or write `off` in `online_server.txt` in place of the address.
- If the lobby doesn't answer for five seconds, the game plays offline by itself and keeps trying in the background.

## Online mode (Quickplay)

The game's own **Online** menu works with WindowSmash's game server, which runs inside the online lobby. **Quickplay** matches you with other WindowSmash players who are searching at the same time.

> [!WARNING]
> Online mode is new. Logging in, the Online menus, Quickplay's search, and two players being matched into one room have been tested, on one PC. The matches themselves, more than two players, and results and GSP afterwards haven't been tested yet. The Linux version has Online mode built in, but it hasn't been tried there.

1. Start the game with **`Play SSBU.bat`**. `online_server.txt` must name a lobby with the game server: `smash.recomps.wtf:39421`, the default, has one.
2. In the main menu, choose **Online**, then **Smash**, then **Quickplay**.
3. Choose your rules and fighter, then **Ready to fight**. The game waits in its practice room until it finds another player.

- **Who you meet:** players with the same lobby code (or none) who are searching too.
- **Your account:** the game server knows you by your `netplay_id.txt` and keeps your player name and ratings. Keep that file to keep your account, and don't give it to anyone: a copy of it plays as you.
- **Your connection:** the game reaches the game server through the lobby, over TCP and UDP on the lobby's port. Nobody needs to open a port. Once players are matched, the match's traffic goes through the lobby: [direct connections](#direct-connections-and-your-internet-address) are only for Local Wireless rooms so far.

| What you see | What to check |
| :--- | :--- |
| An error as soon as you choose **Online** | Start the game with `Play SSBU.bat`, not `launchers\Play SSBU Offline.bat`, and check that the overlay (<kbd>F1</kbd>) says `Online`. |
| An error code starting with **2306** | The game can't reach the game server. The lobby in `online_server.txt` has none (port 39420, or a lobby of your own run with `ssbu_relay.py`), or something blocks TCP to it. |
| Quickplay keeps waiting | Nobody with your lobby code is searching right now. Find players on the [Discord server](https://discord.gg/pkgyYGf3YB). |

## Direct connections and your internet address

Online, the games in a room send their traffic **straight to each other** whenever the players' routers allow it. The lobby introduces them, and the games open a path through their routers themselves (UDP hole punching), so nobody forwards a port for this either. Where that doesn't work, the traffic keeps going through the lobby. Some mobile networks, and providers that share one address between many customers (symmetric NAT), don't allow direct paths. A direct path that goes quiet falls back to the lobby within about a second and is tried again later.

Through the lobby, the delay between two players is roughly both players' pings to the lobby added together. Directly, it's just the delay between the two PCs.

The overlay's second line shows each player in your room, for example `Room: Bravo direct 12 ms, Kim relayed`:

| Shown | Meaning |
| :--- | :--- |
| `direct 12 ms` | Straight to that player, 12 ms there and back. |
| `relayed` | Through the lobby. |
| `relayed (trying direct)` | Still opening a direct path. |
| `relayed (direct off)` | That player turned direct connections off. |
| `relayed (direct off here)` | You turned them off. |
| `relayed (older game)` | That player's WindowSmash is too old for direct connections. |

Direct connections need a lobby running this version of `tools/ssbu_relay.py`. With an older lobby, or a player with an older WindowSmash, play goes through the lobby as before.

> [!NOTE]
> **Your internet address.** The players in a room with you learn your internet (IP) address, as they would playing online on a console. The lobby introduces only players whose games are already playing together in a room, and introduces each player in at most 20 rooms an hour.
>
> To keep your address to yourself, turn **Direct connections** off in the settings (**Online** page), then restart the game. Or create a file named `online_direct.txt` next to `Play SSBU.bat` (on Linux, next to `run.sh`) containing the word `off`. All your traffic then goes through the lobby, which gives your address to nobody.

Private rooms always go through the host.

## Private room

One player hosts a room on their own PC; the others join it with the host's address. Agree on a room name first, for example `smash`.

### The host

1. Double-click **`launchers\Host SSBU Netplay.bat`**.
2. It asks three questions:
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type the name you agreed on and press <kbd>Enter</kbd>.
   - **UDP port:** press <kbd>Enter</kbd> to keep **39420**.
3. If Windows Security asks about `ssbu.exe`, click **Allow**. If you set up your firewall by hand, it must let in **UDP** on that port.
4. The launcher asks your router to open the port (UPnP). The address to give your friends, for example `203.0.113.7:39420`, appears in the launcher's window or in `netplay_address.txt`, and in the game's overlay (<kbd>F2</kbd>). Send it to the others.
5. In the game's main menu, choose **Local Wireless** (the icon of a console with waves, in the right-hand column), then **Create a Room**. When a friend's challenge appears, accept it.

The address above is only an example. Use the one your own launcher shows, or a VPN or home-network address ([which address](#which-address-to-give)).

### Joining players

1. Double-click **`launchers\Join SSBU Netplay.bat`**.
2. It asks three questions:
   - **Host's address:** type the address the host sent you, including the `:` and port, and press <kbd>Enter</kbd>.
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type **exactly** the host's room name and press <kbd>Enter</kbd>.
3. In the game, press <kbd>F2</kbd> and check that the relay is reached and the player count looks right.
4. Choose **Local Wireless**, pick the host's room from **Rooms List**, and wait for the host to accept.
5. If the game asks for a Smash Tag, make your own.

Joining players don't need to change anything on their routers.

### Which address to give

| Where the players are | Address the others type | What the host sets up |
| :--- | :--- | :--- |
| Same home network | The host PC's local address and port | Let incoming UDP through the host's firewall. |
| Different homes, UPnP works | The public address the launcher shows | Nothing more: the launcher opened the port. |
| Different homes, port forwarded by hand | The host's public address and port | Forward **UDP 39420** on the router to the host PC; allow it in the firewall. |
| Everyone on one VPN or virtual LAN | The host's VPN address and port | Everyone joins the same virtual network and allows traffic between its members. |
| A relay on a server | The server's address and port | Everyone, the host included, uses `launchers\Join SSBU Netplay.bat`. |

If your internet provider uses carrier-grade NAT, or your router sits behind another router, forwarding a port at home may not be enough. A virtual LAN, a relay on a server, or the [online lobby](#online-lobby) is usually easier.

## If a room doesn't appear

| What you see | What to check |
| :--- | :--- |
| The overlay stays on `connecting` | The host's address and port, that the host's game is still running, the host's firewall rule for incoming UDP, the router's port mapping, and whether the relay can be reached. |
| `no answer from … - playing offline` | The lobby in `online_server.txt` hasn't answered for five seconds. Check the address with `python tools/ssbu_relay.py --probe <address>`. |
| `Netplay: WindowSmash needs game 13.0.5: run the installer again` | Your game was built from another version of the game, by an older WindowSmash. Put the game's 13.0.5 update in your game folder and run `Install WindowSmash.bat` again: it sets the folder up again as 13.0.5, keeping your save and settings ([how](UPDATES.md#a-folder-built-from-another-version)). |
| Online, a room is missing from the list | Both players need the same lobby code (or none), and both overlays must say `Online`. The list refreshes every few seconds. |
| Both are connected, but in different groups | Private-room names must match exactly. |
| The relay count is right, but no Local Wireless room | The host must create a room in the game: being connected doesn't create one. Wait for the list to refresh. |
| Two people show up as one player | Their installs share an identity. Close the game on one PC and **rename** its `netplay_id.txt`, so a new one is made. Keep both saves. |
| *Communication error* after joining | Packet loss, whether the relay can be reached, matching DLC, and whether the host or relay closed or changed networks. |
| Works at home, not over the internet | The host's firewall, UDP port forwarding, double NAT or carrier-grade NAT. Or use a virtual LAN, a relay on a server, or the online lobby. |
| Online, a player is always `relayed` | Their router or yours doesn't allow direct paths (symmetric NAT, common on mobile networks), a firewall drops the game's packets, or the lobby is older than direct connections. Play still works through the lobby. If Windows Firewall was told to block `ssbu.exe`, allow it in **Windows Security → Firewall & network protection → Allow an app through firewall**. |
| It works, but feels delayed | Traffic through a relay or the lobby adds both players' pings to it. Pick a host or lobby close to the players, and check the overlay for players who aren't `direct`. |

In a private room, the host has no relay hop to itself; each joining player's traffic goes through the host's relay, including traffic between two joining players. Local Wireless was designed for players in the same room, so internet delay can change how the game behaves.

## Run your own lobby

`tools/ssbu_relay.py` is the online lobby. It needs **Python 3.8 or newer**, a UDP port others can reach, and no game files:

```powershell
python tools/ssbu_relay.py --port 39420 --verbose
```

1. Let incoming UDP 39420 through that machine's firewall, and any network firewall in front of it.
2. Every player puts the lobby's address in `online_server.txt` and starts the game with `Play SSBU.bat`.
3. One player creates a Local Wireless room; the others pick it.

`launchers\Join SSBU Netplay.bat` with the lobby's address works too; the room name then acts as the lobby code.

| Task | How |
| :--- | :--- |
| Check that a lobby answers | `python tools/ssbu_relay.py --probe <address>` |
| Install it on a Linux server | `tools/deploy/deploy_lobby.sh <ssh host> install` sets it up as a sandboxed systemd service, `ssbu-lobby`. |
| Turn addresses away | `--banlist FILE` refuses the addresses or networks listed in that file, and re-reads it when it changes. |
| Watch it | It logs a line of statistics every minute. |

To offer [Online mode](#online-mode-quickplay) too, run `tools/ssbu_online_lobby.py` in place of `ssbu_relay.py`. It takes the same options, plus `--state <folder>` for the players' accounts and ratings, and it needs **TCP as well as UDP** on its port:

```powershell
python tools/ssbu_online_lobby.py --port 39421 --state online-state --verbose
```

`tools/deploy/deploy_online_lobby.sh <ssh host> install` sets it up on a Linux server as the service `ssbu-online`, next to `ssbu-lobby`. Its game server lets each player act only on their own account and on matches they're in, and limits how much each connection can ask of it.

The lobby checks where messages come from, limits how fast each player can send, and carries each room's traffic only between that room's players. A game accepts direct packets only from addresses the lobby gave it, carrying that pair's secret, and only from players in its own room. Even so, it's a transport for players you trust: lobby codes and room names aren't passwords, and the traffic isn't encrypted or authenticated.

## Logs and privacy

The launchers write `build\play.log`. For more network detail, set `SSBU_NET_LOG=1` before starting the game (`2` logs every call). Logs and the overlay can show player names, folder names and internet addresses, so read any excerpt before you share it. With Online mode, the lobby's game server also keeps your player name and ratings.

## Reference: environment variables

| Variable | Meaning |
| :--- | :--- |
| `SSBU_NET_HOST=1` | Start a relay inside the game and join it (hosting a private room). |
| `SSBU_NET_RELAY=address[:port]` | Join the online lobby or the relay at this address. |
| `SSBU_NET_ROOM` | The lobby code online, or the room name in a private room. Default `smash`, the public lobby. |
| `SSBU_NET_NAME` | The player name others see. |
| `SSBU_NET_PORT` | The relay's port. Default `39420`. |
| `SSBU_NET_UPNP=0` | Don't ask the router to open the port. |
| `SSBU_NET_BIND` | Limit which address the built-in relay listens on; used by local tests. |
| `SSBU_NET_DIRECT=0` | Online, send all traffic through the lobby. `Play SSBU.bat` sets it when `online_direct.txt` says `off`. |
| `SSBU_NET_TEST_NAT=1`, `SSBU_NET_TEST_BLOCK=<seconds>` | Local tests: filter incoming packets as a router would, or cut direct paths that many seconds after the start. |
| `SSBU_NET_LOG=1` / `2` | Network logging: a summary, or more detail. |
| `SSBU_ONLINE_COUNTRY` | The country Online mode reports, as two letters such as `GB`. Default `US`. |
| `SSBU_ONLINE_SERVER` | `off` turns Online mode off. Otherwise the game server's address inside the lobby, `10.13.0.1:60000` by default; only for tests. |

Without a host or relay setting, the game stays offline. `launchers\Play SSBU Offline.bat` clears both; `Play SSBU.bat` takes the relay from `online_server.txt`; the host and join launchers set their own.
