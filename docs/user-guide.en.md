# MotionCalm motion sickness mask

[简体中文](user-guide.zh-CN.md) | [繁體中文](user-guide.zh-TW.md) | **English**

> This is the end-user guide. For the project overview, downloads and building from source, see the [project README](https://github.com/LChouRepo/MotionCalm#readme).

Do first-person games make you motion sick? Whenever you **turn the camera, walk around, or the picture shakes**, MotionCalm smoothly darkens the edges of the screen and keeps only the middle clear, while a small white dot stays fixed in the center of the screen as a steady reference. About half a second after you stop, the mask fades out on its own.

This technique has been shown again and again to reduce motion sickness in VR games (motion at the edge of your vision is what makes people sick the most). MotionCalm brings it to ordinary monitors and **works with any game**.

- It does not inject into games, read game memory or modify game files; it is just a transparent layer on top of the screen
- Low overhead: about 1–2% of one CPU core while you play, practically 0 when you are not in a game
- No installation needed: unzip and run

---

## 1. Before you start: fix your game settings first (biggest effect, no cost)

MotionCalm is a remedy; the game's own settings matter most. Check these in every game:

| Setting | Recommendation |
|---|---|
| Field of view (FOV) | Raise it; 90–100 or higher is usually more comfortable |
| Head bob / camera shake | **Off** |
| Motion blur | **Off** |
| Chromatic aberration, depth of field, film grain | Off |
| Frame rate | As stable and as high as possible; stutter causes more sickness than a low frame rate |
| Mouse sensitivity | Lower it a little; turning more slowly helps a lot |

Also: keep the room lit, sit a bit farther from the screen and take a break every 20–30 minutes. These really do help.

---

## 2. Installing and starting

1. Extract the zip file to any folder (for example `D:\MotionCalm`). Don't run it directly from inside the zip file.
2. Double-click `MotionCalm.exe`.
3. The first time, Windows may show a blue **"Windows protected your PC"** screen: click **"More info"** → **"Run anyway"**. This happens because the program has no paid code-signing certificate; it is not a virus.
4. After it starts, a small round icon appears in the system tray in the lower-right corner of the screen (it may be hidden behind `^`). **Double-click the icon to open Settings; right-click it to open the menu.**

> ⚠️ **Run the game in "borderless window" or "windowed fullscreen" mode.** If the game uses "exclusive fullscreen", it covers the mask and you won't see it. Most games let you change this in their display settings.

---

## 3. Recommended first steps

**Step 1: Start a game and turn the camera.**
MotionCalm recognizes the game automatically (it looks for the game "locking" the mouse to turn the camera). After that, the edges of the screen should darken when you turn quickly.

**Step 2: Calibrate (strongly recommended, once per game).**
Every game and every player uses a different mouse sensitivity. After calibration, MotionCalm knows how many degrees per second you are actually turning.

1. In the game, aim the crosshair at something easy to recognize (a corner, a pillar).
2. Press **Ctrl + Alt + F7**. A "Calibration 1/2" prompt appears near the top of the screen.
3. **Turn a full circle horizontally** back to the same spot and press **Ctrl + Alt + F7** again.
4. Press **Ctrl + Alt + F7** once more to start run 2, turn a full circle again, and press **Ctrl + Alt + F7** to finish.
5. When you see **"Calibration done: XX counts per degree"**, you're done. From now on it is applied automatically in this game.

If the two runs differ too much, MotionCalm asks you to try again. That's normal; turn a bit more slowly and precisely. **Calibrate again after you change the mouse sensitivity in the game.**

**Step 3: Pick a strength level.**

| Mask strength | When to use it |
|---|---|
| Light | Darkens slightly only on fast flicks |
| **Medium (default)** | Recommended. Darkens on normal turns |
| Strong | For strong motion sickness; the clear area in the middle is smaller |

Press **Ctrl + Alt + F10** to switch levels right in the game. If you get sick even when turning slowly, set **Trigger sensitivity** to "High" in Settings; if the mask appears too often, set it to "Low".

---

## 4. Hotkeys

You can change every hotkey in Settings → Hotkeys.

| Hotkey | What it does |
|---|---|
| Ctrl + Alt + F9 | Turn the mask on/off |
| Ctrl + Alt + F10 | Cycle mask strength (Light → Medium → Strong) |
| Ctrl + Alt + F11 | Turn the center dot on/off |
| Ctrl + Alt + F7 | Calibrate the current game |
| Ctrl + Alt + F6 | Turn screen motion detection on/off (see below) |
| Ctrl + Alt + F8 | Show debug info (for troubleshooting) |

---

## 5. What's in the Settings window

Double-click the tray icon to open it. **Every change applies immediately and is saved automatically; there is no OK button to click.**

- **Mask**: the strength level. The "Preview strength" slider at the top shows the mask on screen right away. Select "Show advanced options" to fine-tune the color, the size of the clear area, and the fade-in / fade-out times.
- **Reference dots**: turn the center dot and the 8 faint dots at the screen edges on or off, and set their size.
- **Trigger**: which input counts as turning the camera: mouse, keyboard (WASD / arrow keys), game controller, screen motion detection.
- **Calibration**: the list of calibrated games; you can delete entries.
- **Hotkeys**: click a box and press the new key combination.
- **General**: display language (简体中文 / 繁體中文 / English; by default it follows the Windows display language), Start with Windows, programs that never show the mask (exclusions), and programs that are always treated as games.

### What is "screen motion detection"?

Some shaking isn't caused by you turning the camera: explosions shaking the screen, camera sway while walking, cutscenes, riding vehicles… Screen motion detection analyzes whether the picture itself is moving, so the mask also appears in these situations. It is on by default.

While it is on, **the mask does not appear in recordings, screenshots or Discord streams** (this is intentional: MotionCalm needs to see a "clean" picture).

Starting with v1.1, screen motion detection also recognizes **rushing forward / backward** (the picture spreading out from the center or shrinking toward it) and **camera tilt** (the whole picture rotating), and the mask appears for these too. You can adjust how strongly forward motion counts in Settings → Trigger → Show advanced options → "Forward/back sensitivity". If you don't want tilt to count, clear "Detect camera tilt".

---

## 6. Still feeling sick? (swimming, flying and driving games)

In games like Subnautica, where you constantly move through 3D space, most of the sickness comes from the motion you cause yourself, and a mask alone isn't enough. From most to least effective:

**1. First remove the game's own camera motion.** Many games deliberately sway or tilt the camera while you move. Deal with this before anything else.
- **Subnautica (the first game)**: install the **MotionSickness** mod from Thunderstore (a mod manager such as r2modman makes this easiest). In its settings, turn on Disable Bobbing (no up-and-down sway while swimming), Disable Tilt (no tilt when moving sideways) and Disable Shake (no screen shake inside the Aurora), set Max FOV to about 100 (the author warns that above 160 it gets worse), and lower Wave Height (less sickness at the surface). It's a single-player game with no anti-cheat, so the mod is safe to install.
- In the game, turn off motion blur and depth of field, and set FOV to 90–100.
- If nothing else works: Nexus has mods that add a **third-person view** to Subnautica, and many people stop feeling sick in third person.
- **Subnautica 2**: there is no FOV slider in the menu, so you need to edit the config or install a mod; turn off motion blur and underwater blur.

**2. Adjust MotionCalm like this:**
- Settings → Mask: choose the "Strong" level.
- Settings → Trigger → Show advanced options: "Forward/back sensitivity" defaults to 1.0×, so normal swimming already darkens the screen edges; if swimming forward still makes you sick, raise it to 1.5–2×. You can also raise "Strength when only moving" (the mask strength while you hold WASD) from 35% to about 60%.

**3. Play habits:**
- **Use mouse and keyboard, not a controller.** With a mouse, the picture turns exactly as far as your hand moves, which your brain can predict; with a stick, the camera keeps turning as long as you push, starts slowly and stops late, so your hand and the picture don't match and you get sick more easily.
- **Lower the mouse sensitivity** so the same turn takes more mouse movement; the picture turns more slowly and predictably.
- **Stop before you look around.** When swimming, swim where you're looking; swinging the camera while you swim means you're turning and moving at the same time. Get into the habit of "stop → look around → pick a direction → swim".
- **Stable frame rate, low latency**: turn on NVIDIA Reflex, or cap the frame rate slightly below your monitor's refresh rate.

**4. Physical tips:**
- **Point a fan at your face**: several studies found that airflow noticeably reduces this kind of motion sickness. It's the cheapest fix there is.
- **Sit still and lean against the backrest**; avoid chairs that swivel or rock.
- **Build up gradually**: play 15–20 minutes at a time and stop as soon as you feel unwell; don't push through it (that makes your brain more sensitive). Increase the time slowly as you adapt; most people improve clearly within one or two weeks.
- Motion sickness medicine (such as dimenhydrinate) taken before playing can help, but it makes you drowsy. Ask a doctor or pharmacist first.

---

## 7. FAQ

**Q: The mask never appears?**
1. Is the game in "exclusive fullscreen"? Switch to "borderless window".
2. Press **Ctrl + Alt + F8** to show debug info and look at the line at the bottom of the screen:
   - It says "excluded": the program is on the exclusion list. Remove it in Settings → General.
   - It says "not a game" or stays "unlocked": right-click the tray icon → **"Treat current program as a game"** (switch back to the game first, then right-click).
3. Did you start the game with "Run as administrator"? Then MotionCalm must also be started with right-click → Run as administrator.

**Q: The mask pops up while I watch YouTube or a movie?**
Right-click the tray icon → "Add current program to exclusions". Common browsers and video players are already on the list by default.

**Q: Can anti-cheat ban me for this?**
MotionCalm doesn't inject into games, read game memory or modify game files; it works the same way as the Discord or OBS overlays. But note: **the dot in the center of the screen effectively acts as a crosshair overlay**, and some tournament or platform rules forbid that. Before official matches or ranked games, we recommend turning off the center dot with **Ctrl + Alt + F11**, or exiting MotionCalm. Check the latest rules for Valorant, FACEIT and similar platforms yourself.

**Q: The mask doesn't show up in screenshots or recordings, but I want to show someone what it looks like?**
Press **Ctrl + Alt + F6** to turn off screen motion detection first, then take the screenshot.

**Q: Small hand tremors make the picture jitter slightly when I stop, and that makes me sick too?**
MotionCalm doesn't modify mouse input (that would require installing a driver), so it can't remove the small jitter caused by hand tremors, but the center dot and edge dots give your eyes a fixed reference. A few things that really help:
- Lower the mouse DPI or the in-game sensitivity (the same tremor turns the camera much less);
- Use a heavier mouse or a mouse pad with more friction;
- Advanced: install the open-source mouse driver **Raw Accel** and use Jump mode with a gain of about 0.5 at low speeds. This cuts the tremor roughly in half without affecting normal turns (check yourself whether the games you play allow it).

**Q: How do I turn it off completely or uninstall it?**
Right-click the tray icon → "Exit". If you turned on "Start with Windows", clear it in Settings → General first. Then just delete the whole folder; MotionCalm doesn't leave files anywhere else.

**Q: How do I report a bug?**
Right-click the tray icon → "Open log", send `MotionCalm.log` to the author, and describe which game you were playing and what you were doing when the problem happened.

---

## 8. Files

| File | Description |
|---|---|
| `MotionCalm.exe` | The program |
| `MotionCalm.ini` | Configuration file (changes made in the Settings window are stored here; you can also edit it in Notepad and it applies automatically after you save; the comments inside the file are in Simplified Chinese) |
| `MotionCalm.log` | Log file (created automatically when the program runs) |

Requirements: Windows 10 (version 2004 or later) or Windows 11, 64-bit.
