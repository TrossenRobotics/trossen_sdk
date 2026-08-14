# Leader link loss on the Rivets

Leaders drop out while followers keep working. Nothing is fixed yet — this file
records what was measured on 2026-08-13 ~19:20-19:25 so the diagnosis doesn't
have to be redone.

Both rigs were **up and recording** the whole time this was captured. There was
no outage.

---

## 1. Leaders and followers are on different networks

- [ ] Nothing to fix here — this is the context for everything below

| | path | address |
|---|---|---|
| `follower_left` / `follower_right` | wired `mgbe1` | `192.168.1.4` / `.5` |
| `glide_left` / `glide_right` | Wi-Fi `wlP1p1s0f0` | rivet-01 `192.168.5.3` / `.2`, rivet-02 `.13` / `.12` |

The wired side is clean on both rigs — carrier up, ~2.9M packets each way,
zero errors. So anything that degrades Wi-Fi presents as "leaders are dead,
followers are fine", which is exactly the reported symptom.

---

## 2. The leader UDP stream loses 60-70% in bursts

- [ ] Move the video off the leaders' link, or the leaders off the video's link

**Problem.** The arm driver logs severe loss on the leader UDP telemetry:

```
19:22:20  rivet-01  glide_right@192.168.5.2   3371 / 5000 lost   (67%)
19:22:25  rivet-01  glide_left @192.168.5.3   3472 / 5000 lost   (69%)
19:20:36  rivet-02  glide_right@192.168.5.12  1512 / 5000 lost   (30%)
19:22:12  rivet-02  glide_left @192.168.5.13  3222 / 5000 lost   (64%)
```

Bursts, not steady state — 2 events per rig in 6 minutes. They land on both
rigs inside the same two-minute window, which is a shared-medium signature
rather than two independent faults.

**It is not the Jetson-to-AP hop.** That link was pristine on both rigs at the
same moment:

```
signal -52 / -46 dBm      tx bitrate 960 / 1200 Mbit/s (80MHz)
tx retries 19 / 10        tx failed 0 / 2      beacon loss 0
0% ICMP loss to the AP    association continuous since boot
```

**The contention is past the AP**, and two things stack up there:

*Both rigs share one AP on one channel.* Same BSSID `9a:41:b2:7c:b5:92`, SSID
`Rivet and Glide`, 5600 MHz, 80 MHz wide. All four leader arms and all the
video compete for the same airtime.

*Each Glide's leaders sit behind a Wi-Fi bridge that also carries a viewing
screen.* ARP resolves three IPs to one MAC per rig:

```
rivet-01  192.168.5.2, .3, .149  -> 5c:67:83:02:cd:fd
rivet-02  192.168.5.12, .13, .177 -> 5c:67:83:03:f7:7d
```

`.149` and `.177` are viewing screens, and they were pulling video over that
same bridge while the leaders were dropping:

```
rivet-01  TX 45 Mbit/s   5 MJPEG streams (:9877) to .149 and .111
rivet-02  TX 13 Mbit/s   3 MJPEG streams (:9877) to .177
```

Bulk TCP video and a 1000 Hz control loop share one bridge link. The video
wins the queue; the small high-rate UDP packets are dropped. Jitter to `.149`
was already elevated — 8.2 ms avg vs 3.3 ms to the AP, peaking at 35 ms —
while ICMP loss was still 0%.

Same root cause as the camera feed freezing on the monitor screens.

---

## 3. Both rigs reboot together

- [ ] Explain it — nothing here points at software yet

rivet-01 came up 18:08:36, rivet-02 at 18:07:19, and the pairing repeats
through the day (17:54/17:55, 17:27/17:12). Two independently powered rigs
rebooting within 80 seconds of each other is a shared power event, not a
network one. Unrelated to the leader loss above.

---

## 4. A dead viewing screen still holds its streams open

- [ ] Drop MJPEG clients that stop reading

`192.168.5.111` was at 100% ICMP loss from rivet-01 while still holding three
established TCP sockets (`:8000`, and two on `:9877`). The rig was pushing
frames at a client that was gone, spending airtime on the same link the
leaders need.

---

## 5. The AP is on a DFS channel

- [ ] Move the AP off channel 120 if simultaneous drops recur

5600 MHz is channel 120, in the weather-radar band. On radar detection the AP
must vacate within 10 seconds and stay off for 30 minutes, dropping every
client on both rigs at once. It had not happened as of this capture (beacon
loss 0, association continuous since boot), but it is a live mechanism for the
exact "both rivets disconnected simultaneously" symptom.
