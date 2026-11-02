// Who may control a demo: one client at a time holds control, everyone else watches. A client gets control with the
// code the demo shows on its own display (so: someone standing next to it), from the QR code on that display (a link
// to the demo's page with the code in it), or with the lab demos server's grant (an admin, or a server link).
//
// Messages, the same on a WebSocket and over Bluetooth (text, JSON); each gets a reply for that client:
//   {"access":"request"}                   show a code on the display    -> {"access":"shown","for":s} | {"access":"wait","for":s}
//   {"access":"code","code":"42"}          the code from the display     -> {"access":"granted","token":t} | {"access":"wrong"} |
//                                                                           {"access":"expired"} | {"access":"wait","for":s}
//   {"access":"token","token":t}           again after a reconnect       -> {"access":"granted","token":t} | {"access":"expired"}
//   {"access":"grant","grant":g[,"if_free":1]}  from the server           -> {"access":"granted","token":t} | {"access":"busy"} | {"access":"denied"}
//   {"access":"release"}                                                  -> {"access":"released"}
//
// A new holder takes over from the previous one (the person at the demo wins). Control ends after 10 minutes without a
// control command (closer_access_used), or 60 s after the holder's connection closed unless it comes back with its
// token. A code (6 digits, or as set with closer_access_set_digits) is valid for 60 s and 5 tries (2 for codes of up to
// 3 digits); after the last wrong try the next code comes 30 s later at the soonest, and code checks are at least 1 s
// apart. Guessing 6 digits takes weeks; 2 digits take ~15 minutes, while every code tried shows on the display.
//
// Client ids are the app's (a WebSocket fd, a Bluetooth handle + 1000, ...), not -1.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOSER_ACCESS_NONE (-1)

// Called (outside any lock) when control moves: from -> to (either may be CLOSER_ACCESS_NONE). Stop the demo there.
typedef void (*closer_access_change_fn)(int from, int to);

// page: the control page on the lab demos site, for the QR code, e.g. "q/besbot" (the site sends that on to
// the drive page); the link is <site>/<page>#123456.
void closer_access_init(const char *page, closer_access_change_fn on_change);
void closer_access_set_digits(int digits);     // 2..8, default 6 (fewer when the display is small or for children)

// A text message from a client: true if it was an access message (reply then holds the answer for that client).
bool closer_access_message(int client, const char *text, char *reply, size_t len);
bool closer_access_has(int client);            // holds control now
int closer_access_holder(void);                // CLOSER_ACCESS_NONE when nobody (or the holder is reconnecting)
bool closer_access_token_ok(const char *token); // a valid token of the current holder (for single HTTP requests)
void closer_access_used(int client);           // a control command from the holder: the idle time starts again
void closer_access_gone(int client);           // its connection closed
void closer_access_take(int client);           // the app checked something of its own (e.g. a fixed Bluetooth code)
const char *closer_access_state(int client);   // "you", "other" or "none"
bool closer_access_by_grant(int client);       // holds control, given by the server's grant (admin-only settings)

// For the display: the code being shown ("42") and its link (QR code), seconds left; 0 = no code now.
int closer_access_code(char *code, size_t code_len, char *link, size_t link_len);

// The server's grant secret (closer_demo passes it on from the heartbeat reply).
void closer_access_set_grant(const char *grant);

#ifdef __cplusplus
}
#endif
