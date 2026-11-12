# 🐱 MiaUI - MinUI + Koriki Hybrid for RG35XX (Old Model / OG)

MiaUI is the culmination of over 2 years of (interrupted) passionate development.

This custom firmware was born from a simple goal: to bring the absolute best of [MinUI](https://github.com/shauninman/MinUI-Legacy-RG35XX) (its elegant, distraction-free interface) and [Koriki / Batocera](https://github.com/rg35xx-cfw/Koriki) (its raw hardware power) into a single distribution for the RG35XX legacy models.

<img width="147" alt="imagen" src="https://github.com/user-attachments/assets/4e3dba13-6a21-4555-8245-cd3739115e5b" /> <img width="320" alt="Support N64" src="https://github.com/user-attachments/assets/67651d41-926a-4601-a65b-350120395f29" /> <img width="320" alt="Emulation" src="https://github.com/user-attachments/assets/328ee5d0-57a5-46dc-b175-cddf92d70374" />

This is a heavily optimized, highly opinionated OS designed to maximize performance and unlock new emulators with an uncompromising approach to SD card preservation.

## 📋 Table of Contents
- [🛡️ SD Card Preservation](#️-sd-card-preservation)
- [🚀 Unleash the RG35XX Power!](#-unleash-the-rg35xx-power)
- [🏆 RetroAchievements (Offline Mode!)](#-retroachievements-offline-mode)
- [🐱 Meet NekoBit](#-meet-nekobit)
- [📺 GPU Display, Scaling & Overlays](#-gpu-display-scaling--overlays)
- [⌨️ Universal Keyboard Mapping & UI](#️-universal-keyboard-mapping--ui)
- [🎨 UI, UX & Navigation Features](#-ui-ux--navigation-features)
- [🌐 USB Networking & WEB Manager](#-usb-networking--web-manager)
- [🧠 Smart Memory Management (On-Demand SWAP)](#-smart-memory-management-on-demand-swap)
- [🧰 Built-in Helper Apps](#-built-in-helper-apps)
- [🤝 Contributing & Feedback](#contributing--feedback)
- [❓ Frequently Asked Questions](#-frequently-asked-questions-faq)
 

## 🛡️ SD Card Preservation
The most common point of failure in retro handhelds is SD card corruption. This OS is engineered from the ground up to reduce disk writes to absolute zero wherever possible.

* **Read-Only System Architecture**: The entire MinUI system and core binaries have been moved to /opt. The goal is to keep the OS partition strictly Read-Only, leaving the ROMs partition exclusively for your games, saves and user profile.
* **Aggressive /tmp Utilization**: Features that require constant background writing (like the "Recent Games" list) have been rerouted to volatile RAM (/tmp). This means your "Recents" will reset on reboot, but your SD card's lifespan is extended. (Tip: Use "Collections" for persistent favorites!)
* **Safe Force-Shutdown**: Keymon has been updated. Pressing [MENU]+[POWER] forces a hardware-safe shutdown at any time, even if any software you launched freezes, ensuring filesystem integrity.
* **EXT4 Exclusive:** There is no FAT32 support for the ROMs partition. FAT32 is prone to corruption and slows down boot times. We use ext4 strictly for maximum speed and reliability. **For Windows users, our #1 recommendation is to transfer games using the built-in WEB Manager via USB** ([more info](#-usb-networking--web-manager)). Based on our experience, it is the most reliable, problem-free method. But you *can* use third-party tools to [read](https://www.diskinternals.com/linux-reader/) or [write](https://www.diskinternals.com/linux-writer/) the ext4 SD card directly.

## 🚀 Unleash the RG35XX power!

Most RG35XX distributions have the internal GPU disabled. Not MiaUI 🐱!!! We have unlocked massive performance gains by utilizing the PowerVR SGX544MP GPU.
* **GPU (GL) Rendering in Minarch**: The emulator frontend now runs 3D cores natively on the GPU. Play Dreamcast (Flycast), PSP (PPSSPP), and Nintendo 64 (parallel-n64).
* **"Free" Hardware Scaling**: Scaling calculations have been offloaded from the CPU to the GPU. This results in significantly lower battery consumption and frees up enough processing power to make heavy
* **A Faster UI, Drawn Entirely by the GPU**: Everything on the screen (main menu, in-game menus, system tools) is GPU-rendered. And now when you are navigating on the menus the usage dropped to about 5% and Big game folders now open almost instantly!
* **Lightning Fast Boot**: The original Koriki boot sequence was completely rewritten from scratch. From powering on to picking a game, the system now boots in just 3 to 6 seconds (depending on your SD card speed).

<img width="320" alt="n64" src="https://github.com/user-attachments/assets/08d79041-ef70-4def-8c2f-412d99b930f5" /> <img width="320" src="https://github.com/user-attachments/assets/a5a8c74f-135d-4335-9839-a1f108043c48" />

## 🏆 RetroAchievements (Offline Mode!)
Earn achievements directly on your console with no internet connection required. Inspired by the amazing work from [NextUI](https://github.com/LoveRetro/NextUI/tree/main/workspace/all/minarch).

<img width="320" alt="captura-ra2" src="https://github.com/user-attachments/assets/b8f44da3-7856-4542-a1cc-b82e76580620" /> <img width="320" alt="captura-ra3" src="https://github.com/user-attachments/assets/08ac2c66-f215-4c53-8aa4-a6c7d1737771" /> <img height="240" alt="ra-gui-dw" src="https://github.com/user-attachments/assets/291aa32c-8f95-49cf-9e74-3ae5a167b40a" />

- **Download Once, Play Anywhere:** Download achievement sets right on the console using the **RetroAchievements.pak** (via USB network) or use our new [Desktop Companion App (`ra_fetch_gui`)](https://github.com/miaui-cfw/rs_fetch_gui/releases) for Windows/macOS/Linux. It uses your PC's internet to safely inject achievements straight to your console!
- **Rich Notifications:** Unlock pop-ups feature a trophy icon, points earned, and a short chime. Pop-ups queue up and slide in smoothly.
- **In-Game Tracking:** Check your achievement list, progress, and points directly from the in-game menu.
- **Hardcore Mode:** When achievements are on, save states are disabled (battery saves still work). You can toggle this off per-game to bring save states back.

## 🐱 Meet NekoBit

A virtual companion living in your main menu, inspired by [PakUI BitPal](https://github.com/tenlevels/PakUI/tree/main) but deeply integrated, zero-cost C port running purely on the GPU (GLES). It uses only 4.8% idle CPU, leaving emulation totally untouched. 

<img width="320" alt="captura-ra0" src="https://github.com/user-attachments/assets/bf3850b3-0fd0-41b3-b5fc-74ea60bf9272" /> <img width="320" alt="captura-ra1" src="https://github.com/user-attachments/assets/c7b3f4e7-15c8-4329-8763-263940752bb0" />

- **Interactive Pet:** Press **SELECT** on the main menu to wake NekoBit up. 
- **Missions & Progression:** Accept missions to play specific systems or unlock achievements. Earn XP, level up, and change moods based on your gameplay. (Mastery rewards included!)
- **XP Curve:** _NekoBit_ is designed to be a **long-term** companion. To ensure it stays fun for years, I carefully crafted an XP curve that scales dynamically as you reach higher levels.
- **Zero SD Wear:** This system lives entirely in volatile RAM (`tmpfs`) while you navigate or play. Your pet's progress is only written to the SD card safely during the system power-off sequence!

### Got a high-level NekoBit? Prove it! 🏆
 👉 Drop a screenshot of your pet's level and mood on your favorite social network using `#MiaUI` and `#NekoBit`. We can't wait to see how far you and your digital companion have come.
 
## 📺 GPU Display, Scaling & Overlays
Scaling has been completely rewritten to leverage the GPU, giving you pixel-perfect control with zero CPU cost. Every scaling and sharpness setting works in GL mode too!
* **4 Hardware Scaling Modes:** Aspect, Fullscreen, Native (perfect integer scaling), and Cropped.
* **32-Bit Color & Overlays:** Added support for image overlays, [perfect](https://github.com/ourigen/perfect_overlays/) for handheld consoles like Game Boy. In-game overlays are now 32-bit, so gradients show no banding.
 * Easy! Just rename (or remove) the overlay.png in their /Emus/GBx.pak folder :) 

<img alt="Scaling modes" src="https://github.com/user-attachments/assets/07251234-21f7-4806-9f2d-175f013ada6a" />

👉 Do you prefer other overlays, or want to change/remove them? Just rename, add, or remove the `overlay.png` in the respective /Emus/xxx.pak folder :)


## ⌨️ Universal Keyboard Mapping & UI
Map any physical gamepad button to a keyboard key directly from the frontend menu. Added new `bind_key` tag to CFG for games and Core files.

<img height="240" alt="Keyboard Map Menu" src="https://github.com/user-attachments/assets/faa699bf-709f-4b1f-93ff-235f30c3ade6" /> <img height="240" alt="Bind Key" src="https://github.com/user-attachments/assets/445d57c6-2cba-4987-9610-d6a453cd7360" /> <img height="240" alt="Bind Key" src="https://github.com/user-attachments/assets/5cea1661-928f-4816-a952-77a552cb64f1" />

## 🎨 UI, UX & Navigation Features

- **New Quick Menu:** Press **MENU** on the main screen to quickly access Settings (Auto-sleep, Kids Mode), RetroAchievements toggles, Network Modes, and Clock.
- **MAME Name Translation:** Arcade games display their proper, readable titles instead of cryptic zip names.
- **Threaded Boxart & Backgrounds:** Scroll through lists blazing fast! Images load on a separate background thread to prevent stuttering. 

<img width="320" alt="imagen" src="https://github.com/user-attachments/assets/acf58e29-8b0c-49bb-976c-0475675f36c7" /> <img width="320" alt="Boxart in MinUI!" src="https://github.com/user-attachments/assets/8cd9d17f-3ba3-4c44-8db8-3e74e1106cae" /> 

### 📂 Custom Sorting & Collections
Take full control of your library using these classic MinUI features:
* **Custom Folder Order:** Want your favorite systems at the top of the list? Prefix your ROM folders with a number and a closing parenthesis (e.g., `1) SFC`, `2) MD`, `3) PS`). MiaUI will sort the folders in that exact order but **magically hide the numbers** on the screen!
* **Collections (.txt files):** Create your own custom lists (like "Mario Games" or "RPGs") without duplicating ROMs on your SD card. Just create a plain `.txt` file (e.g., `Zelda.txt`) inside your `Collections` folder and paste the exact file path of each game on a new line. It will appear as a virtual folder in the UI!

### 🏷️ Visual Directory Tags

Rename your folders or ROMs with specific tags to trigger UI changes:
* [DRK]: Darkens the ROM or directory name to visually organize and aid in list navigation.
* [SWAP]: Displays a small memory card icon next to the name (see Memory Management below).

<img width="320" alt="DRK" src="https://github.com/user-attachments/assets/14322c8f-415d-4740-8a46-ad5db6ec0427" /> <img width="320" alt="SWAP" src="https://github.com/user-attachments/assets/9cae5065-7a47-4812-b4c9-6bd22450ed10" />

### 🐱 The Battery Charge Cat
<img width="320" src="https://github.com/user-attachments/assets/4cbedc1c-4c02-4a26-8313-8cf70890cafd" /> <img width="320" src="https://github.com/user-attachments/assets/0c9d1438-4df5-437a-96f7-99b27dd9cac1" />

Because every great OS needs one. TOP FEATURE!

## 🌐 USB Networking & WEB Manager

<img width="320" alt="captura-usb" src="https://github.com/user-attachments/assets/403a59c3-2b40-4ef9-a577-270c0a704bdb" /> 
<img width="320" alt="manager" src="https://github.com/user-attachments/assets/fd8c577c-5890-4fea-8507-f495573542c9" />

- **Overhauled USB Script:** Switch live between ADB, ADB+Network, Network Only, or Charge Only without restarting.
- **Built-in DHCP:** Your PC gets an address automatically (192.168.137.x).
- **Built-in WEB Manager:** Manage your console from any web browser! Easily navigate `/userdata`, queue bulk game uploads, fix permissions, and use the BIOS tab to validate and rename your system files.


## 🧠 Smart Memory Management (On-Demand SWAP)

The RG35XX a limited RAM 256 MEGS, which previously made some heavy ports impossible to run. While other OSes force a Swap file all the time (killing the SD card), I took a another approach:
 * Opt-in SWAP: You can use the SD Tool to create a Swap partition on TF2. However, the system will not use it by default to protect the card.
 * The [SWAP] Tag: If a specific port or emulator needs extra RAM, simply add [SWAP] to its filename. The OS will dynamically mount the Swap memory only when launching that specific game, and a UI icon will let you know it's active.

Also I created a...

### SD Tool App
A brand new built-in app allows you to format, manage and create a SWAP in Slot 2 (TF2).

<img width="320" alt="imagen" src="https://github.com/user-attachments/assets/e65b775e-6ccb-41ee-94a7-935d2a37c6a1" />

## 🧰 Built-in Helper Apps

To make life easier, 4 standalone utilities are included:
 * Local Scraper: A lightweight Libretro scraper to download game screenshots directly on the device.
 * Video Player: A simple, highly functional VLC-based player for watching short series or videos.
 * Audio Player: Listen to your music with a dedicated battery-saving feature: you can turn the screen completely off while audio continues to play.

<img width="320" alt="MiniVideo Player" src="https://github.com/user-attachments/assets/959a16fe-b8fc-436b-8352-8f4f7532d958" /> <img width="320" alt="MiniMusic Player" src="https://github.com/user-attachments/assets/a2f0b022-8ad9-4113-a5b1-2618d16a225a" />

## Contributing & Feedback
This project is a personal hobby and a labor of love for a handheld I truly enjoy. 

I've spent over two years refining it so that it works exactly the way I want it to. So user support is not provided. Clean Pull Requests are welcome, but for new features or major design changes, please fork the repository and build your own vision.

If you just want to say hi, show off your customized setup, or tell me you're enjoying MiaUI, I'd 🥰 to hear from you over on: [@dskywalk.eurosky.social](https://bsky.app/profile/dskywalk.bsky.social), [@dskywalk@masto.es](https://masto.es/@dskywalk) or [@D_Skywalk on reddit](https://www.reddit.com/user/D_Skywalk/).

## ❓ Frequently Asked Questions (FAQ)

_To push this tiny handheld to do such amazing things, we had to make some strict design decisions. We know this might require a little extra effort on your end to adapt your setup, but the performance we could get is worth it! ;)_

**Q: Can I use SD2 for ROMs? Can the OS merge ROMs from SD1 and SD2? Can I do X on SDx?**  
**A:** No. MiaUI expects all your ROMs to be on SD1. The second slot (SD2) is exclusively reserved for the On-Demand SWAP feature. Scanning and merging directories across two different SD cards adds overhead and slows down boot times. This OS is built to be fast, simple, and direct, so multi-SD ROM reading is not supported.

**Q: I have a large SD card in slot 2. Can I make a 2GB partition for SWAP and use the rest of the space for ROMs?**  
**A:** No. Because the system does not scan SD2 for games, any extra space there will remain unused by MiaUI. The SWAP size is fixed at 2GB (allocating more is actually counterproductive for performance). We highly recommend using an old, cheap, low-capacity SD card (like a 2GB or 4GB card) in slot 2, and saving your large, high-quality cards for slot 1.

**Q: Is HDMI out working?**  
**A:** No, but is not intentionally removed, is not something we use or it could be tested for me. However, HDMI introduces external displays with varying resolutions and known stability issues inherited from the base OS. A core goal of MiaUI is to avoid unnecessary CPU/GPU scaling calculations to maintain top performance. To achieve this, the entire UI and emulation architecture is strictly optimized to run at the handheld's native 640x480 screen.

### Testers
* Thx to ^Morgoth^, Ricco, Eric Magnus, Wolfenprey, dfernandezd, SamyJankins, ShinMetalSonic.
