// rg35xx-gles
// This file implements the platform-specific video, audio (dummy/SDL), and input drivers for the RG35XX.
// It uses SDL2 for window/context creation and input, and OpenGL ES 2.0 for rendering.

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <msettings.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

// -------------------------------------------------------------------------
// Global Contexts & Structs
// -------------------------------------------------------------------------

static struct OVL_Context {
	SDL_Surface* surface;
	GLuint tex;
	int enabled;
} ovl, ovl_game;

#define MODE_565F 1
#define MODE_444A 2
#define MODE_RGBA 3

// Forward declarations
void PLAT_initSurfaceOverlay(const char* filename, int mode);
void PLAT_quitGameOverlay(void);

// funciona regular por que en DOSBOX aparece mucha espera y no tiene sentido, pero es un comienzo.
//#define PROFILE_GPU 1

// -------------------------------------------------------------------------
// Shaders & OpenGL Helper Functions
// -------------------------------------------------------------------------

// Vertex Shader: Projects 2D coordinates to clip space using an orthographic projection matrix.
static const char* vertex_shader_source =
    "uniform mat4 u_projection;\n"
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = u_projection * a_position;\n"
    "   v_texCoord = a_texCoord;\n"
    "}\n";

// Fragment Shader: Samples a texture and outputs the color.
static const char* fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D s_texture;\n"
	"uniform float u_brightness;\n"
    "void main()\n"
    "{\n"
	"  vec4 color = texture2D(s_texture, v_texCoord);\n" 
	"  gl_FragColor = vec4(color.rgb * u_brightness, color.a);\n"
    "}\n";

/**
 * Compiles a shader from source code.
 * @param type GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
 * @param source The GLSL source code.
 * @return The compiled shader handle, or 0 on failure.
 */
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), NULL, infoLog);
        fprintf(stderr, "Shader compilation error: %s\n", infoLog);
        return 0;
    }
    return shader;
}

/**
 * Links vertex and fragment shaders into a program.
 * @param vertex_shader Handle to the compiled vertex shader.
 * @param fragment_shader Handle to the compiled fragment shader.
 * @return The linked program handle, or 0 on failure.
 */
static GLuint link_program(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), NULL, infoLog);
        fprintf(stderr, "Shader program linking error: %s\n", infoLog);
        return 0;
    }
    return program;
}

/**
 * Creates an orthographic projection matrix.
 * Maps (left, top) to (-1, 1) and (right, bottom) to (1, -1).
 */
static void mat4_ortho(float* mat, float left, float right, float bottom, float top, float near, float far) {
    mat[0] = 2.0f / (right - left); mat[1] = 0.0f; mat[2] = 0.0f; mat[3] = 0.0f;
    mat[4] = 0.0f; mat[5] = 2.0f / (top - bottom); mat[6] = 0.0f; mat[7] = 0.0f;
    mat[8] = 0.0f; mat[9] = 0.0f; mat[10] = -2.0f / (far - near); mat[11] = 0.0f;
    mat[12] = -(right + left) / (right - left); mat[13] = -(top + bottom) / (top - bottom); mat[14] = -(far + near) / (far - near); mat[15] = 1.0f;
}

// Helper to print GL version strings
void printGLString(const char *name, GLenum s) {
    const char *v = (const char *) glGetString(s);
    printf("GL %s = %s\n", name, v);
}

// -------------------------------------------------------------------------
// Video Context
// -------------------------------------------------------------------------

static struct VID_Context {
	SDL_Window* window;
	SDL_GLContext context;
	SDL_Surface* screen;    // Main surface (UI)

	GLuint program;
	GLuint vbo_screen;      // VBO for the UI/Screen quad
	GLuint vbo_game;        // VBO for the Game/Emulator quad
	GLuint vbo_game_overlay;// VBO for Game Overlay (e.g. bezels)
	GLuint vbo_overlay;     // VBO for UI Overlay (e.g. battery, pills)
	GLuint ibo;             // Shared Index Buffer
	GLuint screen_tex;      // Texture for UI
	GLuint game_tex;        // Texture for Emulator content
	GLint u_projection;
	GLint u_brightness_loc;

	void* pixels;           // Raw pixel buffer for the screen surface (double buffered)
	int page;               // Current buffer page index (0 or 1)
	int width;
	int height;
	int pitch;

	int clip_x;
	int clip_y;
	int clip_w;
	int clip_h;

	int nearest;            // Filtering mode: 1=Nearest, 0=Linear
	int cleared;            // Flag to indicate if screen was cleared
} vid;

// Struct to track game blit state for optimized texture updates
static struct {
	void* src;
	uint32_t sw, sh, sp; // Source width, height, pitch
	uint32_t dw, dh, dp; // Destination width, height, pitch
	int dx, dy;          // Destination x, y
	int bpp;
	int dirty;           // 1 = game geometry changed, VBO needs rebuild
	uint32_t last_sw;    // Previous source width (to detect resize)
	uint32_t last_sh;    // Previous source height
	uint32_t last_stride_w;
} gles_blit;

/**
 * Uploads vertex data for a quad (x, y, w, h) to a specific VBO.
 * Format: x, y, u, v (where u,v are texture coordinates).
 */
static void upload_quad_vbo_uv(GLuint vbo, float x, float y, float w, float h, float u_max, float v_max) {
	GLfloat vertices[] = {
		// x,  y,     z,      u,      v
		x,     y,     0.0f,   0.0f,   0.0f,   // Top-Left
		x,     y + h, 0.0f,   0.0f,   v_max,  // Bottom-Left
		x + w, y + h, 0.0f,   u_max,  v_max,  // Bottom-Right
		x + w, y,     0.0f,   u_max,  0.0f    // Top-Right
	};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
}

static void upload_quad_vbo(GLuint vbo, float x, float y, float w, float h) {
	upload_quad_vbo_uv(vbo, x, y, w, h, 1.0f, 1.0f);
}

/**
 * Draws a quad using the specified VBO and Texture.
 * Binds VBO, sets up attrib pointers, binds texture, and calls glDrawElements.
 */
static void draw_cached_quad(GLuint vbo, GLuint texture) {
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	// Attrib 0: Position (vec3)
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), 0);
	// Attrib 1: TexCoord (vec2)
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void*)(3 * sizeof(GLfloat)));
	glBindTexture(GL_TEXTURE_2D, texture);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
}

#define MASK_RGB565   0xF800, 0x07E0, 0x001F, 0x0000
#define MASK_RGBA4444 0xF000, 0x0F00, 0x00F0, 0x000F


// -------------------------------------------------------------------------
// Input Initialization (SDL Joystick)
// -------------------------------------------------------------------------

static SDL_Joystick *joystick;

void PLAT_initInput(void) {
    printf("[initInput] SDL_NumJoysticks: %u\n", SDL_NumJoysticks());

    if( SDL_NumJoysticks() < 1 )
    {
        printf( "[initInput] Warning: No joysticks connected!\n" );
    }
    else
    {
        //Load joystick
        printf("[initInput] JOY0: %s\n", SDL_JoystickNameForIndex(0));
        joystick = SDL_JoystickOpen( 0 );

        if( joystick == NULL )
        {
            printf( "[initInput] Warning: Unable to open game controller! SDL Error: %s\n", SDL_GetError() );
        }
    }
}


void PLAT_quitInput(void) {
    SDL_JoystickClose(joystick);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    joystick = NULL;
}

// -------------------------------------------------------------------------
// Video Initialization & Cleanup
// -------------------------------------------------------------------------

SDL_Surface* PLAT_initVideo(void) {
	printf("PLAT_initVideo start\n");
	SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK);
	SDL_ShowCursor(0);

	// Request OpenGL ES 2.0 context
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	// Setup framebuffer attributes
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 4);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 4);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 4);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 4);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

	printf("Creating window...\n");
	vid.window = SDL_CreateWindow("MiaUI", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, FIXED_WIDTH, FIXED_HEIGHT, SDL_WINDOW_OPENGL);
	printf("Creating context...\n");

	vid.context = SDL_GL_CreateContext(vid.window);
	
	// Debug info
	int r, g, b, a, depth;
	SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
	SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
	SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
	SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
	SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
	printf("GPU Backbuffer format: %d%d%d/%d Depth: %dbits\n", r, g, b, a, depth);
	printGLString("Version", GL_VERSION);
	printGLString("Vendor", GL_VENDOR);
	printGLString("Renderer", GL_RENDERER);

	SDL_GL_SetSwapInterval(1); // Enable VSync

	printf("Compiling shaders...\n");
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
	vid.program = link_program(vs, fs);
	glDeleteShader(vs);
	glDeleteShader(fs);

	vid.u_projection = glGetUniformLocation(vid.program, "u_projection");

	vid.u_brightness_loc = glGetUniformLocation(vid.program, "u_brightness");
	glUseProgram(vid.program);
	glUniform1f(vid.u_brightness_loc, 1.0f);

	// Create shared index buffer (never changes) for quads
	GLuint ibo;
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
	vid.ibo = ibo;

	// Create per-layer VBOs
	glGenBuffers(1, &vid.vbo_screen);
	glGenBuffers(1, &vid.vbo_game);
	glGenBuffers(1, &vid.vbo_overlay);
	glGenBuffers(1, &vid.vbo_game_overlay);

	// Setup Projection Matrix (Ortho 0..640, 480..0)
	float projection[16];
	mat4_ortho(projection, 0, FIXED_WIDTH, FIXED_HEIGHT, 0, -1, 1);
	glUseProgram(vid.program);
	glUniformMatrix4fv(vid.u_projection, 1, GL_FALSE, projection);

	// Enable vertex attribs once (persist for lifetime)
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	// Allocate CPU side buffer for the UI surface (double buffered pages)
	vid.width = FIXED_WIDTH;
	vid.height = FIXED_HEIGHT;
	vid.pitch = FIXED_PITCH;
	printf("Allocating pixels (PAGE_SIZE=%d, PAGE_COUNT=%d)...\n", PAGE_SIZE, PAGE_COUNT);
	vid.pixels = malloc(PAGE_SIZE * PAGE_COUNT);

	if (!vid.pixels) {
		printf("Failed to allocate pixels!\n");
		return NULL;
	}

	vid.page = 1;

	printf("Creating RGB surface...\n");
	// FIXED_DEPTH is 16 (FIXED_BPP=2), so we must use 16-bit masks. Using RGBA4444.
	vid.screen = SDL_CreateRGBSurfaceFrom(vid.pixels + PAGE_SIZE, vid.width, vid.height, FIXED_DEPTH, vid.pitch, MASK_RGBA4444);

	if (!vid.screen) {
		printf("Failed to create RGB surface: %s\n", SDL_GetError());
		return NULL;
	}

	printf("Generating textures...\n");
	// Screen (UI) Texture
	glGenTextures(1, &vid.screen_tex);
	glBindTexture(GL_TEXTURE_2D, vid.screen_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vid.width, vid.height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);

	// Pre-build screen quad VBO (0,0,640,480) — only changes on resizeVideo
	upload_quad_vbo(vid.vbo_screen, 0, 0, FIXED_WIDTH, FIXED_HEIGHT);

	// Game Texture
	glGenTextures(1, &vid.game_tex);
	glBindTexture(GL_TEXTURE_2D, vid.game_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	vid.clip_x = 0;
	vid.clip_y = 0;
	vid.clip_w = FIXED_WIDTH;
	vid.clip_h = FIXED_HEIGHT;

	// Initialize game blit state
	gles_blit.dirty = 0;
	gles_blit.last_sw = 0;
	gles_blit.last_sh = 0;
	gles_blit.last_stride_w = 0;

	// Global GL state
	glViewport(0, 0, FIXED_WIDTH, FIXED_HEIGHT);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	// disable global blend
	glDisable(GL_BLEND);

	printf("PLAT_initVideo done\n");

	return vid.screen;
}

void PLAT_fadeVideo(int type) { // 0 = fade out (a negro), 1 = fade in
	float start = (type == 0) ? 1.0f : 0.0f;
	float end   = (type == 0) ? 0.0f : 1.0f;
	int steps = 20; // Número de frames para el fade (aprox 300ms a 60fps)
	
	for (int i = 0; i <= steps; i++) {
		// Calcular brillo actual (interpolación lineal)
		float alpha = start + (end - start) * ((float)i / (float)steps);
		
		// 1. Limpiar pantalla
		glClear(GL_COLOR_BUFFER_BIT);
		
		// 2. Establecer el brillo global en el shader
		glUniform1f(vid.u_brightness_loc, alpha);

		// 3. REDIBUJAR LA ÚLTIMA ESCENA (Sin subir texturas nuevas)
		
		// A) Dibujar juego
		if (vid.game_tex) {
			 draw_cached_quad(vid.vbo_game, vid.game_tex);
		}

		// B) Dibujar Overlay de juego
		if(ovl_game.enabled && ovl_game.tex) {
			draw_cached_quad(vid.vbo_game_overlay, ovl_game.tex);
		}

		// C) Dibujar Overlay UI (pantalla principal)
		if (vid.screen_tex) {
			draw_cached_quad(vid.vbo_screen, vid.screen_tex);
		}

		// D) Dibujar Pill Overlay
		if (ovl.enabled && ovl.tex) {
			draw_cached_quad(vid.vbo_overlay, ovl.tex);
		}

		// 4. Mostrar frame
		SDL_GL_SwapWindow(vid.window);
		
		// Pequeña pausa para que el fade sea visible (10ms)
		usleep(5000);
	}
	
	// Asegurarse de dejar el brillo en el estado final
	glUniform1f(vid.u_brightness_loc, end);
}

void PLAT_quitVideo(void) {
	SDL_GL_SetSwapInterval(0);
	PLAT_fadeVideo(0);

	// Clear to black
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapWindow(vid.window);
	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapWindow(vid.window);

	if(ovl_game.enabled == 1) {
		PLAT_quitGameOverlay();
	}

	glDeleteTextures(1, &vid.game_tex);
	glDeleteTextures(1, &vid.screen_tex);
	glDeleteBuffers(1, &vid.vbo_screen);
	glDeleteBuffers(1, &vid.vbo_game);
	glDeleteBuffers(1, &vid.vbo_overlay);
	glDeleteBuffers(1, &vid.vbo_game_overlay);
	glDeleteBuffers(1, &vid.ibo);
	glDeleteProgram(vid.program);

	SDL_GL_DeleteContext(vid.context);
	SDL_DestroyWindow(vid.window);

	SDL_FreeSurface(vid.screen);
	free(vid.pixels);
	SDL_Quit();

	system("cat /dev/zero > /dev/fb0 2>/dev/null");
}

void PLAT_clearVideo(SDL_Surface* screen) {
	memset(screen->pixels, 0, vid.pitch * vid.height);
}
void PLAT_clearAll(void) {
	PLAT_clearVideo(vid.screen);
	vid.cleared = 1;
}

void PLAT_setVsync(int vsync) {
	SDL_GL_SetSwapInterval(vsync ? 1 : 0);
}

// -------------------------------------------------------------------------
// Video Resize & Scaling
// -------------------------------------------------------------------------

/**
 * Resizes the UI surface (not necessarily the window).
 * This is used when the application requests a different resolution for drawing.
 */
SDL_Surface* PLAT_resizeVideo(int w, int h, int pitch) {
	LOG_info("resizeVideo: %ix%i pitch: %i\n", w,h, pitch);

	vid.width = w;
	vid.height = h;
	vid.pitch = pitch;

	SDL_FreeSurface(vid.screen);
	// Re-wrap the pixels buffer with new dimensions
	vid.screen = SDL_CreateRGBSurfaceFrom(vid.pixels + vid.page * PAGE_SIZE, vid.width, vid.height, FIXED_DEPTH, vid.pitch, MASK_RGBA4444);

	glBindTexture(GL_TEXTURE_2D, vid.screen_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vid.width, vid.height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);

	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {
	vid.clip_x = x;
	vid.clip_y = y;
	vid.clip_w = width;
	vid.clip_h = height;
}

void PLAT_setNearestNeighbor(int enabled) {
	vid.nearest = enabled;
	GLint filter = enabled ? GL_NEAREST : GL_LINEAR;
	
	glBindTexture(GL_TEXTURE_2D, vid.screen_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

	glBindTexture(GL_TEXTURE_2D, vid.game_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

	printf("setNearestNeighbor: %i\n", enabled);
}

void PLAT_setSharpness(int sharpness) {
	GLint filter = sharpness ? GL_LINEAR : GL_NEAREST;

	glBindTexture(GL_TEXTURE_2D, vid.game_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

	printf("setSharpness: %i\n", sharpness);
}

void PLAT_setEffect(int _effect) {
}

void PLAT_vsync(int _remaining) {
	// handled by SDL_GL_SwapWindow in PLAT_flip
}

void PLAT_clearLayers(int layer) {
	PLAT_clearVideo(vid.screen);
}

// -------------------------------------------------------------------------
// Software Scaling to GL Upload Glue
// -------------------------------------------------------------------------

/**
 * Registers parameters for a scaling operation.
 * It doesn't perform the scaling itself but prepares the `gles_blit` state
 * so that `PLAT_flip` knows how to upload the texture.
 */
static void scale_gles(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp, int bpp) {
	gles_blit.src = src;
	// Flag VBO rebuild if destination geometry changed
	if (sw != gles_blit.sw || sh != gles_blit.sh || sp != gles_blit.sp ||
	    dw != gles_blit.dw || dh != gles_blit.dh) {
		gles_blit.dirty = 1;
	}
	gles_blit.sw = sw; gles_blit.sh = sh; gles_blit.sp = sp;
	gles_blit.dw = dw; gles_blit.dh = dh; gles_blit.dp = dp;
	gles_blit.bpp = bpp;
}

// Macros to define scaler wrappers for different scale factors
#define GLES_SCALER(N) \
void scale##N##x##N##_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp) { \
	scale_gles(src, dst, sw, sh, sp, dw, dh, dp, 2); \
} \

GLES_SCALER(1)
GLES_SCALER(2)
GLES_SCALER(3)
GLES_SCALER(4)
GLES_SCALER(5)
GLES_SCALER(6)

// Returns the appropriate scaler function (which just sets up state for GL upload)
scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	switch (renderer->scale) {
		case 6:  return scale6x6_n16;
		case 5:  return scale5x5_n16;
		case 4:  return scale4x4_n16;
		case 3:  return scale3x3_n16;
		case 2:  return scale2x2_n16;
		default: return scale1x1_n16;
	}
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	if (renderer->dst_x != gles_blit.dx || renderer->dst_y != gles_blit.dy) {
		gles_blit.dirty = 1;
	}
	gles_blit.dx = renderer->dst_x;
	gles_blit.dy = renderer->dst_y;

	int bpp = (renderer->true_w > 0) ? (renderer->src_p / renderer->true_w) : (renderer->src_p / renderer->src_w);
	void* src = (uint8_t*)renderer->src + (renderer->src_y * renderer->src_p) + (renderer->src_x * bpp);
	((scaler_t)renderer->blit)(src, NULL, renderer->src_w, renderer->src_h, renderer->src_p, renderer->dst_w, renderer->dst_h, renderer->dst_p);
}

// -------------------------------------------------------------------------
// Frame Flip (Render & Swap)
// -------------------------------------------------------------------------

void PLAT_flip(SDL_Surface* IGNORED, int sync) {
	glClear(GL_COLOR_BUFFER_BIT);

	// disable for plane textures
	glDisable(GL_BLEND);

	// 1. Draw Game Content (Emulator)
	if (gles_blit.src) {
		// 1. Calcular el ancho de la textura basado en el Pitch (memoria real)
		// Ejemplo: Si ancho=256 pero pitch=512 bytes (con bpp=2), el ancho real de textura es 256.
		// Si hay padding, pitch será mayor.
		int stride_w = gles_blit.sp / gles_blit.bpp; 
		
		// 2. Calcular hasta dónde llega la imagen válida (Coordenada U)
		// U = Ancho_Visible / Ancho_Total_Memoria
		float u_max = (float)gles_blit.sw / (float)stride_w;

		// Rebuild game VBO only when geometry changed
		// Ahora pasamos u_max para recortar la "basura" del padding a la derecha
		if (gles_blit.dirty) {
			upload_quad_vbo_uv(vid.vbo_game, 
							   (float)gles_blit.dx, (float)gles_blit.dy, 
							   (float)gles_blit.dw, (float)gles_blit.dh,
							   u_max, 1.0f);
			gles_blit.dirty = 0;
		}

		// Upload game texture pixels DIRECTAMENTE (Sin memcpy)
		glBindTexture(GL_TEXTURE_2D, vid.game_tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		// Verificamos si las dimensiones DE MEMORIA (stride) han cambiado, no solo las visibles
		if (gles_blit.last_stride_w == stride_w && gles_blit.last_sh == gles_blit.sh) {
			// Dimensions unchanged: cheaper sub-image update
			// Usamos stride_w como el ancho de la textura
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, stride_w, gles_blit.sh, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, gles_blit.src);
		} else {
			// Dimensions changed: reallocate texture
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, stride_w, gles_blit.sh, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, gles_blit.src);
			
			gles_blit.last_sw = gles_blit.sw; // Mantenemos esto por referencia
			gles_blit.last_stride_w = stride_w; // Actualizamos el stride trackeado
			gles_blit.last_sh = gles_blit.sh;
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		// Draw game quad (cached VBO)
		draw_cached_quad(vid.vbo_game, vid.game_tex);
		gles_blit.src = NULL;
	}

	// enable blending for ui/overlays
	glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// 2. Draw Game Overlay (e.g. bezels)
	if(ovl_game.enabled && ovl_game.surface) {
		draw_cached_quad(vid.vbo_game_overlay, ovl_game.tex);
	}

	// 3. Draw Screen Overlay (UI) — cached VBO, only pixels change
	glBindTexture(GL_TEXTURE_2D, vid.screen_tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid.width, vid.height, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, vid.screen->pixels);
	draw_cached_quad(vid.vbo_screen, vid.screen_tex);

	// 4. Draw Pill Overlay (Battery/Volume popup) if enabled
	if (ovl.enabled && ovl.surface) {
		glBindTexture(GL_TEXTURE_2D, ovl.tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ovl.surface->w, ovl.surface->h, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, ovl.surface->pixels);
		draw_cached_quad(vid.vbo_overlay, ovl.tex);
	}

	#ifndef PROFILE_GPU
	SDL_GL_SwapWindow(vid.window);
	#else
	PLAT_checkGPULoad();
	#endif

	// Swap pages for double buffering of the UI surface
	vid.page ^= 1;
	vid.screen->pixels = vid.pixels + vid.page * PAGE_SIZE;

	if (vid.cleared) {
		PLAT_clearVideo(vid.screen);
		vid.cleared = 0;
	}
}

// -------------------------------------------------------------------------
// Overlay Initialization
// -------------------------------------------------------------------------

#define OVERLAY_WIDTH PILL_SIZE 
#define OVERLAY_HEIGHT PILL_SIZE 
#define OVERLAY_BPP 2
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP) 
#define OVERLAY_RGBA_MASK MASK_RGBA4444

SDL_Surface* PLAT_initOverlay(void) {
	ovl.surface = SDL_CreateRGBSurface(0, SCALE2(OVERLAY_WIDTH, OVERLAY_HEIGHT), OVERLAY_DEPTH, OVERLAY_RGBA_MASK);
	memset(ovl.surface->pixels, 0, ovl.surface->pitch * ovl.surface->h);

	glGenTextures(1, &ovl.tex);
	glBindTexture(GL_TEXTURE_2D, ovl.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ovl.surface->w, ovl.surface->h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);

	// Pre-build overlay quad VBO (position is fixed)
	int ow = ovl.surface->w;
	int oh = ovl.surface->h;
	int ox = FIXED_WIDTH - SCALE1(PADDING) - ow;
	int oy = SCALE1(PADDING);
	upload_quad_vbo(vid.vbo_overlay, ox, oy, ow, oh);

	return ovl.surface;
}
void PLAT_quitOverlay(void) {
	if (ovl.surface) {
		SDL_FreeSurface(ovl.surface);
		ovl.surface = NULL;
	}
	glDeleteTextures(1, &ovl.tex);
}
void PLAT_enableOverlay(int enable) {
	ovl.enabled = enable;
}

void PLAT_initGameOverlay(const char* ovl_path) {
	PLAT_initSurfaceOverlay(ovl_path, MODE_444A);
}

void PLAT_initSurfaceOverlay(const char* filename, int mode) {
	printf(" >>> overlay loading: %s\n", filename);

	SDL_Surface* surface = IMG_Load(filename);
	if (!surface) {
		return;
	}

    GLenum format, ftype;
    if (surface->format->BytesPerPixel == 3) {
        format = GL_RGB;
        ftype = GL_UNSIGNED_SHORT_5_6_5;
        printf(">> loading %s as 565 buffer.\n", filename);
    } else if (surface->format->BytesPerPixel == 4) {
        SDL_Surface* converted;
        switch (mode) {
            case MODE_565F:
                // Convertir la superficie a RGB565
                converted = SDL_ConvertSurfaceFormat(
                    surface,
                    SDL_PIXELFORMAT_RGB565,
                    0
                );

                // liberamos al anterior
                SDL_FreeSurface(surface);

                if (!converted) {
                    fprintf(stderr, "Failed to convert surface to RGB565: %s\n", SDL_GetError());
                    return;
                }

                format = GL_RGB;
                ftype = GL_UNSIGNED_SHORT_5_6_5;
                surface = converted;
                printf(">> loading %s as 4BPP buffer converted to 565.\n", filename);
                break;
            case MODE_444A:
                // Convertir la superficie a RGB4444 with alpha
                converted = SDL_ConvertSurfaceFormat(
                    surface,
                    SDL_PIXELFORMAT_RGBA4444,
                    0
                );

                // liberamos al anterior
                SDL_FreeSurface(surface);

                if (!converted) {
                    fprintf(stderr, "Failed to convert surface to RGB565: %s\n", SDL_GetError());
                    return;
                }

                surface = converted;
                format = GL_RGBA;
                ftype = GL_UNSIGNED_SHORT_4_4_4_4;
                printf(">> loading %s as 4BPP buffer converted to 4444 (16bits alpha channel).\n", filename);
                break;
            case MODE_RGBA:
                format = GL_RGBA;
                ftype = GL_UNSIGNED_BYTE;
                printf(">> loading %s as 4BPP buffer as full RGBA.\n", filename);
                break;
        }
    } else {
        fprintf(stderr, "Unsupported image format\n");
        SDL_FreeSurface(surface);
        return;
    }

	ovl_game.surface = surface;

	glGenTextures(1, &ovl_game.tex);
	glBindTexture(GL_TEXTURE_2D, ovl_game.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, format, ovl_game.surface->w, ovl_game.surface->h, 0, format, ftype, ovl_game.surface->pixels);

	// Pre-build overlay quad VBO (position is fixed)
	int ow = ovl_game.surface->w;
	int oh = ovl_game.surface->h;
	int ox = 0;
	int oy = 0;
	upload_quad_vbo(vid.vbo_game_overlay, 0, 0, FIXED_WIDTH, FIXED_HEIGHT);

	ovl_game.enabled = 1;
}

void PLAT_quitGameOverlay(void) {
	if (ovl_game.surface) {
		SDL_FreeSurface(ovl_game.surface);
		ovl_game.surface = NULL;
	}
	glDeleteTextures(1, &ovl_game.tex);
}

// -------------------------------------------------------------------------
// Power / Battery / System
// -------------------------------------------------------------------------

void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	*is_charging = getInt("/sys/class/power_supply/battery/charger_online");
	
	int i = getInt("/sys/class/power_supply/battery/voltage_now") / 10000; 
	i -= 310; 	

	     if (i > 80) *charge = 100;
	else if (i > 60) *charge =  80;
	else if (i > 40) *charge =  60;
	else if (i > 20) *charge =  40;
	else if (i > 10) *charge =  20;
	else             *charge =  10;
}

void PLAT_enableBacklight(int enable) {
	putInt("/sys/class/backlight/backlight.2/bl_power", enable ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
}
void PLAT_powerOff(void) {
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	
	system("shutdown");
}

void PLAT_setCPUSpeed(int speed) {
	int freq = 0;
	switch (speed) {
		case CPU_SPEED_MENU: 		freq =  504000; break;
		case CPU_SPEED_POWERSAVE:	freq = 1104000; break;
		case CPU_SPEED_NORMAL: 		freq = 1296000; break;
		case CPU_SPEED_PERFORMANCE: freq = 1488000; break;
	}
	
	char cmd[32];
	sprintf(cmd, "overclock.elf %d\n", freq);
	system(cmd);
}

#define RUMBLE_PATH "/sys/class/power_supply/battery/moto"
void PLAT_setRumble(int strength) {
	int val = MAX(0, MIN((100 * strength) >> 16, 100));
	putInt(RUMBLE_PATH, val);
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	return "Anbernic RG35XX";
}

int PLAT_isOnline(void) {
	return 0;
}

// Variable para guardar el % de carga y mostrarlo donde quieras
float gpu_load_percent = 0.0f;

void PLAT_checkGPULoad(void) {
    static uint32_t frame_count = 0;
    static uint32_t accumulated_wait = 0;
    
    // Tomamos el tiempo ANTES de decirle a la GPU que nos muestre el frame
    // En este punto, la CPU ya acabó su trabajo de emulación.
    uint32_t t_start = SDL_GetTicks();
    
    // Esta función BLOQUEA la CPU hasta que la GPU termina de dibujar y llega el VSync
    SDL_GL_SwapWindow(vid.window);
    
    // Tomamos el tiempo DESPUÉS
    uint32_t t_end = SDL_GetTicks();
    
    // La diferencia es el tiempo que la GPU nos hizo esperar (o que tardó el VSync)
    uint32_t wait_time = t_end - t_start;
    
    // Acumulamos para hacer una media cada 60 frames
    accumulated_wait += wait_time;
    frame_count++;

    if (frame_count >= 60) {
        // En 60 frames a 60fps, han pasado 1000ms.
        // Si wait_time total es 1000ms, significa que la CPU no hizo nada, 
        // solo esperó -> GPU muy relajada (o Vsync haciendo su trabajo).
        
        // Pero, si la espera es CERO, significa que la CPU llegó tarde 
        // o justo a tiempo -> GPU al 100% o CPU cuello de botella.
        
        // HEURÍSTICA INVERSA PARA ESTAS CONSOLAS:
        // Si SwapWindow tarda 16ms -> GPU sobrada (esperó al VSync completo).
        // Si SwapWindow tarda 1ms  -> GPU saturada (terminó justo cuando la CPU se lo pidió).
        
        float avg_wait = (float)accumulated_wait / 60.0f;
        
        // 16.6ms es el tiempo ideal de espera si la CPU es instantánea.
        // Si esperamos 16ms, carga es 0%. Si esperamos 0ms, carga es 100%.
        float load = 100.0f - ( (avg_wait / 16.66f) * 100.0f );
        
        // Clamp (limitar valores)
        if (load < 0) load = 0;
        if (load > 100) load = 100;
        
        gpu_load_percent = load;
        
        printf("GPU Load Est: %.1f%% (Avg Wait: %.2f ms)\n", gpu_load_percent, avg_wait);
        
        frame_count = 0;
        accumulated_wait = 0;
    }
}