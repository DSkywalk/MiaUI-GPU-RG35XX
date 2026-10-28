#ifndef __msettings_h__
#define __msettings_h__

void InitSettings(void);
void QuitSettings(void);

int GetBrightness(void);
int GetVolume(void);

void SetRawBrightness(int value); // 0-1024
void SetRawVolume(int value); // 0-40

void SetBrightness(int value); // 0-10
void SetVolume(int value); // 0-20
void SetMasterVolumeKoriki(int value); //0-100
void SetRawBrightnessKoriki(int value); //0-100
void SaveSettings(void);

int GetJack(void);
void SetJack(int value); // 0-1

// Sleep after this long without input: 0 off, 1 1min, 2 2min, 3 5min, 4 10min
int GetAutoSleep(void);
void SetAutoSleep(int value);
// Asleep this long, power off: 0 5min, 1 10min, 2 15min
int GetSleepPowerOff(void);
void SetSleepPowerOff(int value);

// RetroAchievements, set in minui and read by minarch when a game starts
int GetRAEnabled(void); // 0-1
void SetRAEnabled(int value);
int GetRASound(void); // 0-1
void SetRASound(int value);
int GetRANotify(void); // RA_NOTIFY_*: 0 all, 1 unlocks, 2 general, 3 off
void SetRANotify(int value);

int GetHDMI(void);
void SetHDMI(int value); // 0-1

#endif  // __msettings_h__
