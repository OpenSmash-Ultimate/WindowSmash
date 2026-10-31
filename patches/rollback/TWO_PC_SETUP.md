# Rollback netplay: 2-4 PCs

Two to four PCs play one match. Each PC plays one player with its own controller (or keyboard). One PC **hosts**;
the others **join** it. Every PC sends its player's moves on, and rolls back when another's arrive late. Every PC
checks, every 10 frames, that all the fights are still the same, and says so in its log.

This is the research build (`build\ssbu_rollback.exe`). The normal game (`Play SSBU.bat`) is untouched.

## 1. The game on each PC (once)

Every PC must run the very same build: two games built differently can drift apart.

- Each player builds WindowSmash with **their own** game files (version 13.0.5), from the same commit of this
  branch, then runs `patches\rollback\build_rollback.bat`. The host's ruleset is sent to everyone, so the saves'
  rulesets don't need to match.
- The first start: Windows may say it protected your PC (the game isn't signed): **More info → Run anyway**.
  Windows Firewall may ask about network access: allow it.

## 2. Host

On the host's PC double-click `launchers\Rollback Netplay.bat`. A window opens: click **Host a match**, pick **how
many players** (2-4, the host's own included) and click **Start hosting**. Once the game has started, the window shows
the addresses to give the others, each with a **Copy** button:

- **Friends on the same network:** for example `192.168.1.20`
- **Friends elsewhere (internet):** for example `203.0.113.7`

The game asks the host's router to open the port it needs (UDP 7701, UPnP); nothing to set up by hand. If the
window says the router has no UPnP or refused, forward UDP port 7701 to the host PC on the router by hand (port
forwarding / virtual server); if it says the router is behind the provider's NAT, let another PC host instead.

## 3. Join

On every other PC double-click `launchers\Rollback Netplay.bat`, click **Join a match**, paste the host's address (the
"same network" one at the same place, the "internet" one otherwise) and click **Join**; the window remembers the
address for next time. The host gives each joining game its player number by
itself (player 2, 3, 4 in the order they join), and the input delay is chosen from the connection (1 frame at
home, more over a long trip).

## 4. Set up the match

Plug in only your own controller (any kind; every controller plays as a Pro Controller here), or use the keyboard.

1. **Smash** → the same ruleset on every PC.
2. **Character select:** join with your controller and **pick your fighter**. That's all: no CPUs needed, the other
   slots are filled with the other PCs' players when the match starts. The host also picks the stage.
3. Start the fight on every PC, in any order: the first ones wait (up to 10 minutes) at the start of the match until
   every PC is there.

## 5. What to look for, and what to send back

- Every PC shows the same fight. Your own moves are instant; the others' arrive a little late and can make a
  fighter snap slightly: that is rollback.
- After the match, the logs are `build\play_rollback_net_host.log` (the host) and `build\play_rollback_net_join.log`
  (a joining PC; `join2`, `join3` if more games share one PC). Lines to look for: `a game joined: it is player N`,
  `took player 1's match seed`, `net mode (player N): ... 0 DIFFERENT (in sync)`, or `DESYNC with player N`.

## Known limits

- Two to four players. The rules come from each PC's own save: keep the rulesets the same.
- The host's PC uses UDP port **7701**; joining PCs any free port (they only call out).
- If a joining PC's game is closed and started again, it joins again by itself (it may get another number).
- For tests only: `launchers\Rollback Netplay (command prompt).bat` does the same in text. `set RB_DELAY=2` (on the
  host) fixes the delay; `set SSBU_RB_NET_LIVE=0` lets a bot play that PC's player. Type them in a Command Prompt and
  start either launcher from it.
