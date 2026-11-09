# Status

| Area | State |
|---|---|
| Display | Works: 3200x2000 at 120 Hz, scale 2 |
| KDE Plasma 6 | Works, as a normal user inside a Droidspaces container |
| Touch, keyboard cover, touchpad | Works, including detaching and reattaching the cover |
| Pen | Works through the touch screen, with pressure |
| Pen gestures (Bluetooth) | Works: double tap, swipe up, swipe down |
| Wi-Fi | Works: NetworkManager with Android's saved networks |
| Bluetooth | Works: pairings copied from Android, A2DP audio |
| GPU | Works: all apps through Zink and Turnip |
| USB-C / HDMI display | Works: tested at 2560x1440, can be plugged in during a session. Some direct USB-C monitors need their own power |
| Battery, brightness | Works |
| Screen off and lock | Works: power button, keyboard case, idle |
| Tablet mode | Works: follows the keyboard cover |
| Vietnamese input | Works: fcitx5 with Lotus |
| USB mouse, keyboard, gamepad | Should work; a gamepad was detected, others not tried |
| USB drives, USB sound | Written but not tested with real hardware |
| Built-in speaker and microphone | Not working |
| Bluetooth microphone | Not working |
| Real sleep (suspend) | Not working: refused by the kernel (alarm timer, wakelocks) |
| Auto-rotate, sensors | Not working: needs a bridge to Android's sensor HAL |
| Cameras | Not working |
| 4K or several external displays | Untested, likely needs more shim work |

Idle with the display off used about 1.5 W in one measurement, and the display on about 4.3 W.
While Linux runs, Android's apps, calls and notifications are stopped.
