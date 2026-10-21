/* Netplay: the game's own Local Wireless mode, on a virtual LAN carried through a relay.
 *
 * Each player's game gets an address in 10.13.0.0/16 from the relay; local wireless
 * (runtime/hle_net.c emulates nn::ldn) and the game's sockets live in that network, and their
 * datagrams - broadcasts included - travel to the other players through the relay
 * (runtime/net_relay.c), or straight to them once the online lobby has introduced the players
 * of a room to each other and their games have punched through their routers. Clients only
 * send to the relay (and those players), so no player needs port forwarding. The relay is
 * either the online lobby (tools/ssbu_relay.py on a server: it lists every player's Local
 * Wireless rooms to the others and carries each room's traffic between its players only) or a
 * private room's relay inside the host player's game (its port opened with UPnP).
 *
 * Configured by the launchers (Play SSBU.bat, launchers\Host/Join SSBU Netplay.bat) through the environment:
 *   SSBU_NET_HOST=1         run the relay here and join it (port SSBU_NET_PORT, 39420)
 *   SSBU_NET_RELAY=addr     join the lobby or relay at addr[:port]
 *   SSBU_NET_ROOM           the lobby code (a relay's room); smash, the default, is the public lobby
 *   SSBU_NET_NAME           player name
 *   SSBU_NET_DIRECT=0       no direct connections: all of a room's traffic through the lobby, and the
 *                           lobby gives this player's internet address to nobody
 *   SSBU_NET_LOG=1          log every socket/network call (debugging)
 * With neither set the game stays offline, as before. */
#pragma once
#include "recomp.h"

#define NET_SUBNET      0x0A0D0000u   /* 10.13.0.0 */
#define NET_MASK        0xFFFF0000u
#define NET_GATEWAY     0x0A0D0001u   /* 10.13.0.1: the relay's own address */
#define NET_BROADCAST   0x0A0DFFFFu
#define NET_DEFAULT_PORT 39420
#define NET_PROTOCOL    2
#define NET_LDN_PORT    65511         /* local wireless's own messages (hle_net.c): always through the relay */

int net_configured(void);            /* netplay asked for (host or join) */
int net_log(void);                   /* SSBU_NET_LOG */
u32 net_local_ip(void);              /* this player's virtual address (host order), 0 until the relay gives one */
const char *net_player_name(void);
/* The lobby or relay this game joins (host[:port]): SSBU_NET_RELAY's, else online_server.txt's
 * with --online; NULL when it hosts a private room or plays offline. */
const char *net_relay_address(void);
int net_unreachable(void);           /* asked for, but the relay has not answered for a while: offline */

/* The online lobby: rooms are listed by the lobby, not found by their broadcasts. */
int net_lobby(void);                 /* joined to a lobby (not a private room's relay) */
void net_lobby_list(u32 seq);        /* ask for the rooms; they come back through ldn_listing, then ldn_list_end */
/* (hle_net.c) One room of the list asked for as seq (owner: its host's address), and the list's end. */
void ldn_listing(u32 seq, u32 owner, const u8 *info);
void ldn_list_end(u32 seq);

/* A datagram from one of this player's sockets to another player (or the room: NET_BROADCAST). */
void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);
/* (hle_net.c) A datagram for this player's sockets, from the relay, another player directly, or looped back. */
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);
/* (hle_net.c) Whether this player is in a local wireless network now, the player at ip one of its
 * nodes: only then do that player's direct packets reach the game. */
int ldn_in_network(u32 ip);

/* One line about the connection for the overlay (empty when netplay is off). */
void net_status(char *out, size_t n);
