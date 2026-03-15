#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>

#include "msettings.h"

///////////////////////////////////////

#define SETTINGS_VERSION 2
typedef struct Settings {
    int version; // future proofing
    int brightness;
    int headphones;
    int speaker;
    int unused[2]; // for future use
    // NOTE: doesn't really need to be persisted but still needs to be shared
    int jack; 
} Settings;
static Settings DefaultSettings = {
    .version = SETTINGS_VERSION,
    .brightness = 2,
    .headphones = 4,
    .speaker = 8,
    .jack = 0,
};
static Settings* settings;

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];
static int shm_fd = -1;
static int is_host = 0;
static int shm_size = sizeof(Settings);

#define BACKLIGHT_PATH "/sys/class/backlight/backlight.2/bl_power"

#define BRIGHTNESS_PATH "/sys/class/backlight/backlight.2/brightness"
#define BRIGHTNESS_SCRIPT "/usr/bin/batocera-brightness"
#define BRIGHTNESS_STORE "/usr/bin/store-brightness"

#define VOLUME_BIN "/usr/bin/amixer"
#define VOLUME_MIXER "Master"

#define VOLUME_PATH "/sys/class/volume/value"

void InitSettings(void) {	
    sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));
    
    shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
    if (shm_fd==-1 && errno==EEXIST) { // already exists
        //printf("Settings SHM:%s EEXIST\n", SHM_KEY);
        puts("Settings client");
        shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
        settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    }
    else { // host
        //printf("Settings SHM:%s host\n", SHM_KEY);
        puts("Settings host"); // keymon
        is_host = 1;
        // we created it so set initial size and populate
        ftruncate(shm_fd, shm_size);
        settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        
        int fd = open(SettingsPath, O_RDONLY);
        if (fd>=0) {
            //printf("Loading settings from: %s\n", SettingsPath);
            read(fd, settings, shm_size);
            // TODO: use settings->version for future proofing?
            close(fd);
        }
        else {
            // load defaults
            //printf("Loading defaults (ign: %s)\n", SettingsPath);
            memcpy(settings, &DefaultSettings, shm_size);
        }
        
        // these shouldn't be persisted
        // settings->jack = 0;
    }
    //printf("brightness: %i\nspeaker: %i \n", settings->brightness, settings->speaker);
    
    SetVolume(GetVolume());
    SetBrightness(GetBrightness());
}
void QuitSettings(void) {
    munmap(settings, shm_size);
    if (is_host) shm_unlink(SHM_KEY);
}
void SaveSettings(void) {
    //printf("[saving] brightness: %i\nspeaker: %i (%s)\n", settings->brightness, settings->speaker, SettingsPath);
    int fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
    if (fd>=0) {
        write(fd, settings, shm_size);
        close(fd);
        sync();
    }
}

int GetBrightness(void) { // 0-10
    return settings->brightness;
}
void SetBrightness(int value) {
    int raw;
    switch (value) {
        case 0: raw=16; break; 		//   0
        case 1: raw=24; break; 		//   8
        case 2: raw=40; break; 		//  16
        case 3: raw=64; break; 		//  24
        case 4: raw=128; break;		//	64
        case 5: raw=192; break;		//  64
        case 6: raw=256; break;		//  64
        case 7: raw=384; break;		// 128
        case 8: raw=512; break;		// 128
        case 9: raw=768; break;		// 256
        case 10: raw=1024; break;	// 256
    }
    SetRawBrightness(raw);
    settings->brightness = value;
}

int GetVolume(void) { // 0-20
    return settings->jack ? settings->headphones : settings->speaker;
}
void SetVolume(int value) {
    if (settings->jack) settings->headphones = value;
    else settings->speaker = value;
    
    int raw = value * 2;
    SetRawVolume(raw);
}

void SetRawBrightnessKoriki(int val) {
    char cmd[128];
    sprintf(cmd, "%s %i && %s\n", BRIGHTNESS_SCRIPT, val, BRIGHTNESS_STORE);
    system(cmd);
}

void SetMasterVolumeKoriki(int val) {
    char cmd[128];
    sprintf(cmd, "%s -q sset '%s' %i\n", VOLUME_BIN, VOLUME_MIXER, val);
    system(cmd);
}

void SetRawBrightness(int val) { // 0 - 1024
    // printf("SetRawBrightness(%i)\n", val); fflush(stdout);
    int fd = open(BRIGHTNESS_PATH, O_WRONLY);
    if (fd>=0) {
        dprintf(fd,"%d",val);
        close(fd);
    }
}
void SetRawVolume(int val) { // 0 - 40
    int fd = open(VOLUME_PATH, O_WRONLY);
    if (fd>=0) {
        dprintf(fd,"%d",val);
        close(fd);
    }
}


// monitored and set by thread in keymon
int GetJack(void) {
    return settings->jack;
}
void SetJack(int value) {
    // printf("SetJack(%i)\n", value); fflush(stdout);
    
    settings->jack = value;
    SetVolume(GetVolume());
}

int GetHDMI(void) {
    return 0;
}
void SetHDMI(int value) {
    // buh
}