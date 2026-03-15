# 🐱 MiaUI - MinUI + Koriki Hybrid for RG35XX (Old Model)

MiaUI is the culmination of over 2 years of passionate development. This custom firmware was born from a simple goal: to bring the absolute best of [MinUI](https://github.com/shauninman/MinUI-Legacy-RG35XX) (its elegant, distraction-free interface) and [Koriki / Batocera](https://github.com/rg35xx-cfw/Koriki) (its raw hardware power) into a single distribution for the RG35XX legacy models.

This is a heavily optimized, highly opinionated OS designed to maximize performance and unlock new emulators with an uncompromising approach to SD card preservation.

<img width="147" alt="imagen" src="https://github.com/user-attachments/assets/4e3dba13-6a21-4555-8245-cd3739115e5b" /> <img width="320" alt="Support N64" src="https://github.com/user-attachments/assets/67651d41-926a-4601-a65b-350120395f29" /> <img width="320" alt="Emulation" src="https://github.com/user-attachments/assets/328ee5d0-57a5-46dc-b175-cddf92d70374" />
  

## 🛡️ SD Card Preservation
The most common point of failure in retro handhelds is SD card corruption. This OS is engineered from the ground up to reduce disk writes to absolute zero wherever possible.

* Read-Only System Architecture: The entire MinUI system and core binaries have been moved to /opt. The goal is to keep the OS partition strictly Read-Only, leaving the ROMs partition exclusively for your games, saves and user profile.
* Aggressive /tmp Utilization: Features that require constant background writing (like the "Recent Games" list) have been rerouted to volatile RAM (/tmp). This means your "Recents" will reset on reboot, but your SD card's lifespan is extended. (Tip: Use "Collections" for persistent favorites!)
* Safe Force-Shutdown: Keymon has been updated. Pressing [MENU]+[POWER] forces a hardware-safe shutdown at any time, even if any software you launched freezes, ensuring filesystem integrity.
* EXT4 Exclusive: There is no FAT32 support for the ROMs partition. FAT32 is prone to corruption and slows down boot times. We use ext4 strictly for maximum speed and reliability. (Windows users can [use third-party tools](https://www.diskinternals.com/linux-reader/) to access ext4 drives).

## 🚀 Unleash the RG35XX power!

Most RG35XX distributions have the internal GPU disabled. Not MiaUI 🐱!!!
* PowerVR SGX544MP Enabled: By activating the GPU and GLES (thx Koriki), we have unlocked massive performance gains.
* "Free" Hardware Scaling: Scaling calculations have been offloaded from the CPU to the GPU. This results in significantly lower battery consumption and frees up enough processing power to make heavy systems like N64 playable.
* Lightning Fast Boot: The original Koriki boot sequence was completely rewritten from scratch. From powering on to picking a game, the system now boots in just 3 to 6 seconds (depending on your SD card speed).

<img width="320" alt="n64" src="https://github.com/user-attachments/assets/08d79041-ef70-4def-8c2f-412d99b930f5" /> <img width="320" src="https://github.com/user-attachments/assets/a5a8c74f-135d-4335-9839-a1f108043c48" />


## 📺 GPU Display & Scaling Options
Scaling has been entirely rewritten from the original MinUI code to leverage the GPU, giving you pixel-perfect control over how games look.

### 4 Hardware Scaling Modes:
* Aspect: Maintains the original aspect ratio using floating-point calculations.
* Fullscreen: Ignores the original aspect ratio and stretches the image to fill the whole screen.
* Native: Maximizes the screen using perfect integer scaling.
* Cropped: Maximizes the screen using integer scaling, allowing the image to slightly overscan/crop past the screen edges for a larger picture.

<img alt="Scaling modes" src="https://github.com/user-attachments/assets/07251234-21f7-4806-9f2d-175f013ada6a" />

### ⎚ Overlay Support 😻

Added support for image overlays, [perfect](https://github.com/ourigen/perfect_overlays/) for handheld consoles (like Game Boy or Game Gear) so you don't have to stare at a black screen when playing.

<img width="320" alt="Overlay GB" src="https://github.com/user-attachments/assets/b96818e7-a154-480b-9a50-86e55739aa88" /> <img width="320" height="480" alt="Overlay NPC" src="https://github.com/user-attachments/assets/99659d30-ddc3-4ad9-8570-1395961efcaf" />

## ⌨️ Universal Keyboard Mapping & UI
Map any physical gamepad button to a keyboard key directly from the frontend menu. Added new `bind_key` tag to CFG for games and Core files.

<img height="240" alt="Keyboard Map Menu" src="https://github.com/user-attachments/assets/faa699bf-709f-4b1f-93ff-235f30c3ade6" /> <img height="240" alt="Bind Key" src="https://github.com/user-attachments/assets/445d57c6-2cba-4987-9610-d6a453cd7360" /> <img height="240" alt="Bind Key" src="https://github.com/user-attachments/assets/5cea1661-928f-4816-a952-77a552cb64f1" />

## 🎨 UI, UX & Navigation Features

While the system is more powerful, it retains the snappy, minimalist soul of MinUI.

* Threaded Boxart & Backgrounds: You can now add thumbnails and custom backgrounds! To keep the UI blazing fast, images are loaded on a separate background thread. You can scroll through your lists as fast as always without stuttering. (Don't want boxart? Ignore it, and the OS behaves exactly like vanilla MinUI).
* MAME Name Translation: No more scrolling through cryptic zip file names (e.g., ssrdrubc.zip). Arcade games now display their proper, readable titles.

<img width="320" alt="Boxart in MinUI!" src="https://github.com/user-attachments/assets/8cd9d17f-3ba3-4c44-8db8-3e74e1106cae" /> 

### Visual Directory Tags

Rename your folders or ROMs with specific tags to trigger UI changes:
* [DRK]: Darkens the ROM or directory name to visually organize and aid in list navigation.
* [SWAP]: Displays a small memory card icon next to the name (see Memory Management below).

<img width="320" alt="DRK" src="https://github.com/user-attachments/assets/14322c8f-415d-4740-8a46-ad5db6ec0427" /> <img width="320" alt="SWAP" src="https://github.com/user-attachments/assets/9cae5065-7a47-4812-b4c9-6bd22450ed10" />

### 🐱 The Battery Charge Cat
<img width="320" src="https://github.com/user-attachments/assets/4cbedc1c-4c02-4a26-8313-8cf70890cafd" /> <img width="320" src="https://github.com/user-attachments/assets/0c9d1438-4df5-437a-96f7-99b27dd9cac1" />

Because every great OS needs one. TOP FEATURE!

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
 * ADB Enabler: Connect your console via ADB to your computer for easy file transfers, debugging, or routing an INTERNET connection just using a USB cable. No wifi device need it!
 * Local Scraper: A lightweight Libretro scraper to download game screenshots directly on the device.
 * Video Player: A simple, highly functional VLC-based player for watching short series or videos.
 * Audio Player: Listen to your music with a dedicated battery-saving feature: you can turn the screen completely off while audio continues to play.

<img width="320" alt="MiniVideo Player" src="https://github.com/user-attachments/assets/959a16fe-b8fc-436b-8352-8f4f7532d958" /> <img width="320" alt="MiniMusic Player" src="https://github.com/user-attachments/assets/a2f0b022-8ad9-4113-a5b1-2618d16a225a" />

## Contributing & Feedback
This project is a personal hobby and a labor of love for a handheld I truly enjoy. 

I've spent over two years refining it so that it works exactly the way I want it to. So user support is not provided and the Issues tab is disabled. Clean Pull Requests are welcome, but for new features or major design changes, please fork the repository and build your own vision.

If you just want to say hi, show off your customized setup, or tell me you're enjoying MiaUI, I'd 🥰 to hear from you over on Bluesky: [@dskywalk.bsky.social](https://bsky.app/profile/dskywalk.bsky.social)

### Testers
* Thx to ^Morgoth^, Ricco, Eric Magnus, Wolfenprey, dfernandezd, ShinMetalSonic.
