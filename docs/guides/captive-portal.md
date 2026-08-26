---
title: Captive portal
parent: Guides
nav_order: 6
---

# Captive portal
{: .no_toc }

Why joining `BLLED_AP` pops up a setup page by itself, and what to do when it does not.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## When the access point appears

The controller opens an open access point called **`BLLED_AP`** whenever it has no usable WiFi
connection:

- on a fresh install, before you have configured anything,
- after a factory reset,
- when the stored credentials are wrong, or the network was unreachable at boot.

The strip turns **pink** while the AP is up, which is the quickest way to tell from across the
room.

{: .note }
> If credentials *are* stored, the controller keeps retrying your network in the background while
> the AP is open, and restarts by itself the moment the network comes back. You do not have to do
> anything after a power cut that outlasted your router.

## How the prompt appears

Modern operating systems test every network they join by fetching a known URL and checking the
answer. If the answer is not what they expect, they conclude there is a captive portal and show
you a **"Sign in to network"** notification.

BLLED plays along, deliberately:

1. In AP mode it runs a **wildcard DNS server** that answers *every* hostname with `192.168.4.1`.
   So whatever your phone tries to look up, it arrives at the controller.
2. The web server answers the probe URLs each platform uses with a redirect to the setup page:

   | Platform | Probe |
   |---|---|
   | Android | `/generate_204` |
   | iOS, macOS | `/hotspot-detect.html` |
   | Windows | `/connecttest.txt`, `/ncsi.txt` |
   | Firefox | `/canonical.html`, `/success.txt` |

3. Any other unknown URL redirects to `/wifi` as well.

The setup page is **fully self-contained** — one HTML file with no separate stylesheet, script or
image — because captive-portal mini-browsers are picky about sub-resources. It is served without
authentication while the AP is up, which is also why the whole device is unauthenticated in AP
mode.

<p>
<img src="../screenshots/wifisetup-375.png" alt="The captive-portal setup page on a phone (captured from the repository mock server — the /wifi route only exists in AP mode)" width="260">
</p>

## When it does not work

The portal is a convention, not a protocol, and several things can defeat it.

| Situation | What to do |
|---|---|
| No prompt appears at all | Open **`http://192.168.4.1`** in a browser while joined to `BLLED_AP`. This always works. |
| The browser insists on HTTPS | An HTTPS probe **cannot** be intercepted — that is the entire point of TLS. Type `http://192.168.4.1` with the `http://` prefix, in a normal browser tab. |
| The address bar autocompletes to a search | Same fix: type the full `http://192.168.4.1`. Some browsers treat a bare IP as a search term. |
| Android drops the network because it has no internet | Accept the "stay connected" / "use this network anyway" prompt. Some Android builds hide it under the network's detail screen. |
| The mini-browser opens but the page is blank or half-rendered | Close it and use a real browser at `http://192.168.4.1`. Captive mini-browsers are stripped down and occasionally just fail. |
| The page opens but *Discover* finds no printer | Discovery searches the network the controller is **about to join**, not the AP. Enter the printer's IP by hand. |
| You join `BLLED_AP` and nothing at all responds | Check the strip really is pink. If it is orange, the controller is still trying to join a stored network and has not opened the AP yet — give it about a minute. |

## After you save

The controller restarts, `BLLED_AP` disappears and your phone falls back to whatever it was on
before — sometimes silently, sometimes not. Re-join your own network before you go looking for
`http://blled.local`.

If the credentials were wrong, the strip goes pink again and `BLLED_AP` comes back.

## In normal operation

Once the controller is on your network there is no captive portal and no `/wifi` page — the route
does not exist outside AP mode. The screenshot above was taken against the repository's mock
server (`tools/mock_server.py`) for exactly that reason.

To get the setup page back, either fix the WiFi from the
[Connection](../using/connection.md) tab, or factory-reset from
[System](../using/system.md#the-buttons).
