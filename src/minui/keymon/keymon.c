#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <signal.h>

#include <msettings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "defines.h"

//	for ev.value
#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define INPUT_COUNT 2
static int inputs[INPUT_COUNT];
static struct input_event ev;

static pthread_t ports_pt;
static volatile int keepRunning = 1;

#define JACK_STATE_PATH "/sys/class/switch/h2w/state"
#define BACKLIGHT_PATH "/sys/class/backlight/backlight.2/bl_power"

void intHandler(int dummy) {
    keepRunning = 0;
}

int getInt(char* path) {
    int i = 0;
    FILE *file = fopen(path, "r");
    if (file!=NULL) {
        fscanf(file, "%i", &i);
        fclose(file);
    }
    return i;
}

static void* watchPorts(void *arg) {
    int has_headphones,had_headphones;
    
    has_headphones = had_headphones = getInt(JACK_STATE_PATH);
    SetJack(has_headphones);
    
    while(1) {
        sleep(1);
        
        has_headphones = getInt(JACK_STATE_PATH);
        if (had_headphones!=has_headphones) {
            had_headphones = has_headphones;
            SetJack(has_headphones);
        }
    }
    
    return 0;
}

int main (int argc, char *argv[]) {
    SetMasterVolumeKoriki(100);
    InitSettings();
    pthread_create(&ports_pt, NULL, &watchPorts, NULL);
    
    char path[32];
    for (int i=0; i<INPUT_COUNT; i++) {
        sprintf(path, "/dev/input/event%i", i);
        inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    
    uint32_t input;
    uint32_t val;
    uint32_t menu_pressed = 0;
    uint32_t power_pressed = 0;
    
    uint32_t up_pressed = 0;
    uint32_t up_just_pressed = 0;
    uint32_t up_repeat_at = 0;
    
    uint32_t down_pressed = 0;
    uint32_t down_just_pressed = 0;
    uint32_t down_repeat_at = 0;
    uint32_t release_save_at = 0;
    
    uint8_t ignore;
    uint8_t dirty = 0;
    uint32_t then;
    uint32_t now;
    struct timeval tod;
    
    gettimeofday(&tod, NULL);
    then = tod.tv_sec * 1000 + tod.tv_usec / 1000; // essential SDL_GetTicks()
    ignore = 0;
    
    while (keepRunning) {
        gettimeofday(&tod, NULL);
        now = tod.tv_sec * 1000 + tod.tv_usec / 1000;
        if (now-then>100) ignore = 1; // ignore input that arrived during sleep
        
        for (int i=0; i<INPUT_COUNT; i++) {
            input = inputs[i];
            while(read(input, &ev, sizeof(ev))==sizeof(ev)) {
                if (ignore) continue;
                val = ev.value;
                //printf("keymon: t: %i v: %i\n", ev.type, ev.code);
                if (( ev.type != EV_KEY ) || ( val > REPEAT )) continue;
                switch (ev.code) {
                    case 316: // menu
                        menu_pressed = val;
                    break;
                    case 116: // power
                        power_pressed = val;
                    break;
                    case 317: // button up
                        up_pressed = up_just_pressed = val;
                        if (val) up_repeat_at = now + 300;
                    break;
                    case 318: // button down
                        down_pressed = down_just_pressed = val;
                        if (val) down_repeat_at = now + 300;
                    break;
                    default:
                    break;
                }
            }
        }

        // safety poweroff
        if (power_pressed && menu_pressed) {
            printf("keymon shutdown!!\n");
            system("shutdown");
        }
        
        if (up_just_pressed || (up_pressed && now>=up_repeat_at)) {
            if (menu_pressed) {
                val = GetBrightness();
                if (val<BRIGHTNESS_MAX) {
                    SetBrightness(++val);
                    dirty = 1;
                }
            }
            else {
                val = GetVolume();
                if (val<VOLUME_MAX) {
                    SetVolume(++val);
                    dirty = 1;
                }
            }
            
            if (up_just_pressed) up_just_pressed = 0;
            else up_repeat_at += 100;
        } else if (down_just_pressed || (down_pressed && now>=down_repeat_at)) {
            if (menu_pressed) {
                val = GetBrightness();
                if (val>BRIGHTNESS_MIN){
                    SetBrightness(--val);
                    dirty = 1;
                }
            }
            else {
                val = GetVolume();
                if (val>VOLUME_MIN) {
                    SetVolume(--val);
                    dirty = 1;
                }
            }
            
            if (down_just_pressed) down_just_pressed = 0;
            else down_repeat_at += 100;
        } else if (dirty && ((!down_pressed) || (!up_pressed))){
            if (!release_save_at) release_save_at = now + 6000;
            if (now>=release_save_at) {
                SaveSettings();
                dirty = 0;
                release_save_at = 0;
            }
        }
        
        then = now;
        ignore = 0;
        
        usleep(16666); // 60fps
    }
}
