/****************************************************************************
 *  MiaUI
 *
 *  Copyright David Colmenero - D_Skywalk (2024-2026)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

int main(int argc , char* argv[]) {
	if (argc<2) {
		puts("Usage: show.elf image.png delay");
		return 0;
	}
	
	char path[256];
	strncpy(path,argv[1],256);
	if (access(path, F_OK)!=0) return 0; // nothing to show :(
	
	PWR_setCPUSpeed(CPU_SPEED_MENU);

	int delay = argc>2 ? atoi(argv[2]) : 2;
	
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PWR_init();
	InitSettings();

	SDL_FillRect(screen, NULL, 0);
	 
	SDL_Surface* img = IMG_Load(path); 
	SDL_BlitSurface(img, NULL, screen, &(SDL_Rect){(screen->w-img->w)/2,(screen->h-img->h)/2});
	GFX_flip(screen);

	sleep(delay);

	SDL_FreeSurface(img);
	

	QuitSettings();
	PWR_quit();

	GFX_quit();
	
	return 0;
}