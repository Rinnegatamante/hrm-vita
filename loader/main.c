/* main.c -- Human Resource Machine .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2022 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#define _GNU_SOURCE
#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

#ifdef DEBUG
#define dlog printf
#else
#define dlog
#endif

void sincos(double x, double *sin, double *cos) {
	float s, c;
	sincosf(x, &s, &c);
	*sin = s; *cos = c;
}

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

uint8_t force_30fps = 1;

static char fake_vm[0x1000];
static char fake_env[0x1000];

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module cpp_mod, hrm_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, DATA_PATH);
	return buf;
}

int debugPrintf(char *text, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/hrm_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}
int pthread_mutexattr_init_fake(pthread_mutexattr_t **uid) {
	pthread_mutexattr_t *m = calloc(1, sizeof(pthread_mutexattr_t));
	if (!m)
		return -1;
	
	int ret = pthread_mutexattr_init(m);
	if (ret < 0) {
		free(m);
		return -1;
	}

	*uid = m;

	return 0;
}

int pthread_mutexattr_destroy_fake(pthread_mutexattr_t **m) {
	if (m && *m) {
		pthread_mutexattr_destroy(*m);
		free(*m);
		*m = NULL;
	}
	return 0;
}

int pthread_mutexattr_settype_fake(pthread_mutexattr_t **m, int type) {
	pthread_mutexattr_settype(*m, type);
	return 0;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const pthread_mutexattr_t **mutexattr) {
	pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
	if (!m)
		return -1;
	
	int ret = pthread_mutex_init(m, mutexattr ? *mutexattr : NULL);
	if (ret < 0) {
		free(m);
		return -1;
	}

	*uid = m;

	return 0;
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_trylock(*uid);
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
	if (uid && *uid && (uintptr_t)*uid > 0x8000) {
		pthread_mutex_destroy(*uid);
		free(*uid);
		*uid = NULL;
	}
	return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_lock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init_fake(&attr);
		pthread_mutexattr_settype_fake(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy_fake(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
	pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
	if (!c)
		return -1;

	*c = PTHREAD_COND_INITIALIZER;

	int ret = pthread_cond_init(c, NULL);
	if (ret < 0) {
		free(c);
		return -1;
	}

	*cnd = c;

	return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
	if (cnd && *cnd) {
		pthread_cond_destroy(*cnd);
		free(*cnd);
		*cnd = NULL;
	}
	return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_timedwait(*cnd, *mtx, t);
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}

int pthread_cond_timedwait_relative_np_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, struct timespec *ts) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	
	if (ts != NULL) {
		struct timespec ct;
		clock_gettime_hook(0, &ct);
		ts->tv_sec += ct.tv_sec;
		ts->tv_nsec += ct.tv_nsec;
	}
	
	pthread_cond_timedwait(*cnd, *mtx, ts); // FIXME
	return 0;
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
	return pthread_create(thread, NULL, entry, arg);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
	if (!once_control || !init_routine)
		return -1;
	if (__sync_lock_test_and_set(once_control, 1) == 0)
		(*init_routine)();
	return 0;
}

int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

extern void *__aeabi_atexit;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

int stat_hook(const char *pathname, void *statbuf) {
	dlog("stat(%s)\n", pathname);
	struct stat st;
	int res = stat(pathname, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	printf("fstat\n");
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint32_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

char *basename(char *path) {
	char *p = path;
	if (strlen(path) == 1)
		return path;
	char *slash = strstr(p, "/");
	while (slash) {
		p = slash + 1;
		slash = strstr(p, "/");
	}
	return p;
}

void *sceClibMemclr(void *dst, SceSize len) {
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath() {
	return DATA_PATH;
}

char *SDL_AndroidGetInternalStoragePath() {
	return DATA_PATH;
}

char *SDL_GetBasePath_hook() {
	char *r = (char *)SDL_malloc(512);
	sprintf(r, "%s/assets/", DATA_PATH);
	return r;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook() {
	//dlog("ABORT CALLED!!!\n");
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

static so_default_dynlib gl_hook[] = {
	{"glDetachShader", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *SDL_GL_GetProcAddress_fake(const char *symbol) {
	for (size_t i = 0; i < gl_numhook; ++i) {
		if (!strcmp(symbol, gl_hook[i].symbol)) {
			return (void *)gl_hook[i].func;
		}
	}
	
	void *r = vglGetProcAddress(symbol);
	if (!r) {
		dlog("Cannot find symbol %s\n", symbol);
	}
	return r;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int __errno_hook() {
	printf("errno called\n");
	return __errno;
}

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	dlog("opendir(%s)\n", dirname);
	SceUID uid = sceIoDopen(dirname);

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

SDL_Surface *IMG_Load_hook(const char *file) {
	char real_fname[256];
	printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_Texture * IMG_LoadTexture_hook(SDL_Renderer *renderer, const char *file) {
	char real_fname[256];
	printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, file);
		return IMG_LoadTexture(renderer, real_fname);
	}
	return IMG_LoadTexture(renderer, file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
	SDL_RWops *f;
	char real_fname[256];
	//printf("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, fname);
		//printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	} else {
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
	printf("fopen(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", DATA_PATH, fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

SDL_GLContext SDL_GL_CreateContext_fake(SDL_Window * window) {
	eglSwapInterval(0, force_30fps ? 2 : 1);
	return SDL_GL_CreateContext(window);
}

SDL_Window * SDL_CreateWindow_fake(const char *title, int x, int y, int w, int h, Uint32 flags) {
	return SDL_CreateWindow(title, 0, 0, SCREEN_W, SCREEN_H, flags);
}

SDL_Renderer *SDL_CreateRenderer_hook(SDL_Window *window, int index, Uint32 flags) {
	SDL_Renderer *r = SDL_CreateRenderer(window, index, flags);
	SDL_RenderSetLogicalSize(r, SCREEN_W, SCREEN_H);
	return r;
}

extern void *_Znaj;
extern void *_ZdlPv;
extern void *_Znwj;

int usleep_hook(useconds_t usec) {
	//printf("usleep %u\n", usec);
	if (usec > 0)
		usleep(usec);
	return 0;
}

int rename_hook(const char *old_filename, const char *new_filename) {
	sceIoRemove(new_filename);
	return rename(old_filename, new_filename);
}

static so_default_dynlib default_dynlib[] = {
	{ "SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook },
	{ "SDL_AndroidGetActivityClass", (uintptr_t)&ret0 },
	{ "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
	{ "SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState },
	{ "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
	{ "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
	{ "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
	{ "SDL_Android_Init", (uintptr_t)&ret1 },
	{ "SDL_AddTimer", (uintptr_t)&SDL_AddTimer },
	{ "SDL_CondSignal", (uintptr_t)&SDL_CondSignal },
	{ "SDL_CondWait", (uintptr_t)&SDL_CondWait },
	{ "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
	{ "SDL_CreateCond", (uintptr_t)&SDL_CreateCond },
	{ "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
	{ "SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer },
	{ "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
	{ "SDL_iconv_string", (uintptr_t)&SDL_iconv_string},
	{ "SDL_OpenURL", (uintptr_t)&SDL_OpenURL},
	{ "SDL_SetWindowIcon", (uintptr_t)&SDL_SetWindowIcon},
	{ "SDL_RegisterEvents", (uintptr_t)&SDL_RegisterEvents},
	{ "SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform},
	{ "SDL_SetClipboardText", (uintptr_t)&SDL_SetClipboardText},
	{ "SDL_GetClipboardText", (uintptr_t)&SDL_GetClipboardText},
	{ "SDL_EnableScreenSaver", (uintptr_t)&SDL_EnableScreenSaver},
	{ "SDL_DisableScreenSaver", (uintptr_t)&SDL_DisableScreenSaver},
	{ "SDL_memset", (uintptr_t)&SDL_memset},
	{ "SDL_RWseek", (uintptr_t)&SDL_RWseek},
	{ "SDL_CreateSemaphore", (uintptr_t)&SDL_CreateSemaphore},
	{ "SDL_SemWait", (uintptr_t)&SDL_SemWait},
	{ "SDL_SemWaitTimeout", (uintptr_t)&SDL_SemWaitTimeout},
	{ "SDL_SemPost", (uintptr_t)&SDL_SemPost},
	{ "SDL_DestroySemaphore", (uintptr_t)&SDL_DestroySemaphore},
	{ "SDL_TLSCreate", (uintptr_t)&SDL_TLSCreate},
	{ "SDL_TLSGet", (uintptr_t)&SDL_TLSGet},
	{ "SDL_TLSSet", (uintptr_t)&SDL_TLSSet},
	{ "sincos", (uintptr_t)&sincos},
	{ "SDL_CreateRGBSurfaceFrom", (uintptr_t)&SDL_CreateRGBSurfaceFrom},
	{ "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture },
	{ "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface },
	{ "SDL_CreateThread", (uintptr_t)&SDL_CreateThread },
	{ "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_fake },
	{ "SDL_Delay", (uintptr_t)&SDL_Delay },
	{ "SDL_strlen", (uintptr_t)&SDL_strlen },
	{ "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
	{ "SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer },
	{ "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture },
	{ "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
	{ "SDL_FillRect", (uintptr_t)&SDL_FillRect },
	{ "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
	{ "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode },
	{ "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
	{ "SDL_GetError", (uintptr_t)&SDL_GetError },
	{ "SDL_GetModState", (uintptr_t)&SDL_GetModState },
	{ "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
	{ "SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA },
	{ "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
	{ "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
	{ "SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo },
	{ "SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode },
	{ "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath },
	{ "SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod },
	{ "SDL_GetTicks", (uintptr_t)&SDL_GetTicks },
	{ "SDL_GetVersion", (uintptr_t)&SDL_GetVersion },
	{ "SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture },
	{ "SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext },
	{ "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
	{ "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
	{ "SDL_Init", (uintptr_t)&SDL_Init },
	{ "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
	{ "SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect },
	{ "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
	{ "SDL_LockSurface", (uintptr_t)&SDL_LockSurface },
	{ "SDL_Log", (uintptr_t)&ret0 },
	{ "SDL_LogError", (uintptr_t)&ret0 },
	{ "SDL_LogSetPriority", (uintptr_t)&ret0 },
	{ "SDL_MapRGB", (uintptr_t)&SDL_MapRGB },
	{ "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
	{ "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
	{ "SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow },
	{ "SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents },
	{ "SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents },
	{ "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
	{ "SDL_PollEvent", (uintptr_t)&SDL_PollEvent },
	{ "SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture },
	{ "SDL_Quit", (uintptr_t)&SDL_Quit },
	{ "SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer },
	{ "SDL_RenderClear", (uintptr_t)&SDL_RenderClear },
	{ "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy },
	{ "SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect },
	{ "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent },
	{ "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook },
	{ "SDL_RWread", (uintptr_t)&SDL_RWread },
	{ "SDL_RWwrite", (uintptr_t)&SDL_RWwrite },
	{ "SDL_RWclose", (uintptr_t)&SDL_RWclose },
	{ "SDL_RWsize", (uintptr_t)&SDL_RWsize },
	{ "SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem },
	{ "SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey },
	{ "SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter },
	{ "SDL_SetHint", (uintptr_t)&SDL_SetHint },
	{ "SDL_SetHintWithPriority", (uintptr_t)&SDL_SetHintWithPriority },
	{ "SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode },
	{ "SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor },
	{ "SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget },
	{ "SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode },
	{ "SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod },
	{ "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
	{ "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
	{ "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput },
	{ "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
	{ "SDL_strdup", (uintptr_t)&SDL_strdup },
	{ "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
	{ "SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface },
	{ "SDL_UpdateTexture", (uintptr_t)&SDL_UpdateTexture },
	{ "SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit },
	{ "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
	{ "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
	{ "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
	{ "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds },
	{ "SDL_UnionRect", (uintptr_t)&SDL_UnionRect },
	{ "SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus },
	{ "SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode },
	{ "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
	{ "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
	{ "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
	{ "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
	{ "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
	{ "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
	{ "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
	{ "SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus },
	{ "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
	{ "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
	{ "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
	{ "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext },
	{ "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake },
	{ "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
	{ "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
	{ "SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData },
	{ "SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags },
	{ "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
	{ "SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex },
	{ "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen },
	{ "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize },
	{ "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
	{ "SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow },
	{ "SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData },
	{ "SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle },
	{ "SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard },
	{ "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
	{ "SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition },
	{ "SDL_GL_SetSwapInterval", (uintptr_t)&ret0 },
	{ "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
	{ "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
	{ "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
	{ "SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID },
	{ "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow },
	{ "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_NumAccelerometers", (uintptr_t)&ret0 },
	{ "SDL_AndroidGetJNIEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem },
	{ "SDL_ConvertSurface", (uintptr_t)&SDL_ConvertSurface },
	{ "SDL_SetError", (uintptr_t)&SDL_SetError },
	{ "SDL_MapRGBA", (uintptr_t)&SDL_MapRGBA },
	{ "SDL_EventState", (uintptr_t)&SDL_EventState },
	{ "SDL_SetSurfaceBlendMode", (uintptr_t)&SDL_SetSurfaceBlendMode },
	{ "SDL_UpperBlitScaled", (uintptr_t)&SDL_UpperBlitScaled },
	{ "SDL_FreeRW", (uintptr_t)&SDL_FreeRW },
	{ "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState },
	{ "SDL_JoystickNumAxes", (uintptr_t)&SDL_JoystickNumAxes },
	{ "SDL_JoystickUpdate", (uintptr_t)&SDL_JoystickUpdate },
	{ "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
	{ "SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton },
	{ "SDL_GetScancodeFromKey", (uintptr_t)&SDL_GetScancodeFromKey },
	{ "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
	{ "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
	{ "SDL_JoystickGetHat", (uintptr_t)&SDL_JoystickGetHat },
	{ "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
	{ "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
	{ "SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState },
	{ "SDL_LogSetAllPriority", (uintptr_t)&SDL_LogSetAllPriority },
	{ "SDL_LogMessageV", (uintptr_t)&SDL_LogMessageV },
	{ "SDL_RWtell", (uintptr_t)&SDL_RWtell },
	{ "SDL_AndroidGetActivity", (uintptr_t)&ret0 },
	{ "SDL_free", (uintptr_t)&SDL_free },
	{ "SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd },
	{ "SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet },
	{ "SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor },
	{ "SDL_OpenAudio", (uintptr_t)&SDL_OpenAudio },
	{ "SDL_CloseAudio", (uintptr_t)&SDL_CloseAudio },
	{ "SDL_PauseAudio", (uintptr_t)&SDL_PauseAudio },
	{ "SDL_CreateCursor", (uintptr_t)&SDL_CreateCursor },
	{ "SDL_SetCursor", (uintptr_t)&SDL_SetCursor },
	{ "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
	{ "SDL_FreeCursor", (uintptr_t)&SDL_FreeCursor },
	{ "SDL_CreateColorCursor", (uintptr_t)&SDL_CreateColorCursor },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
	{ "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },
	{ "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
	{ "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
	{ "SL_IID_EFFECTSEND", (uintptr_t)&SL_IID_EFFECTSEND },
	{ "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB },
	{ "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
	{ "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
	{ "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
	{ "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
	{ "slCreateEngine", (uintptr_t)&slCreateEngine },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memmove8", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno_hook },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	// { "__google_potentially_blocking_region_begin", (uintptr_t)&__google_potentially_blocking_region_begin },
	// { "__google_potentially_blocking_region_end", (uintptr_t)&__google_potentially_blocking_region_end },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&ret0 },
	// { "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgetpos", (uintptr_t)&fgetpos },
	{ "fsetpos", (uintptr_t)&fsetpos },
	{ "floor", (uintptr_t)&floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	// { "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	// { "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat_hook },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&sceClibMemmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	// { "open", (uintptr_t)&open },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&ret0 },
	{ "pthread_attr_init", (uintptr_t)&ret0 },
	{ "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
	{ "pthread_attr_setstacksize", (uintptr_t)&ret0 },
	{ "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
	{ "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
	{ "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
	{ "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
	{ "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
	{ "pthread_cond_timedwait_relative_np", (uintptr_t)&pthread_cond_timedwait_relative_np_fake}, // FIXME
	{ "pthread_create", (uintptr_t)&pthread_create_fake },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
	{ "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
	{ "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
	{ "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
	{ "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
	{ "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},
	{ "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},
	{ "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},
	{ "pthread_once", (uintptr_t)&pthread_once_fake },
	{ "pthread_self", (uintptr_t)&pthread_self },
	{ "pthread_setname_np", (uintptr_t)&ret0 },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_setschedparam", (uintptr_t)&pthread_setschedparam },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	{ "rename", (uintptr_t)&rename_hook },
	{ "remove", (uintptr_t)&remove },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	//{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strlcat", (uintptr_t)&strlcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep_hook },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	// { "writev", (uintptr_t)&writev },
	{ "glClearColor", (uintptr_t)&glClearColor },
	{ "glClearDepthf", (uintptr_t)&glClearDepthf },
	{ "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
	{ "glTexImage2D", (uintptr_t)&glTexImage2D },
	{ "glDeleteTextures", (uintptr_t)&glDeleteTextures },
	{ "glDepthFunc", (uintptr_t)&glDepthFunc },
	{ "glGenTextures", (uintptr_t)&glGenTextures },
	{ "glBindTexture", (uintptr_t)&glBindTexture },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri },
	{ "glGetError", (uintptr_t)&glGetError },
	{ "glMatrixMode", (uintptr_t)&glMatrixMode },
	{ "glLoadIdentity", (uintptr_t)&glLoadIdentity },
	{ "glScalef", (uintptr_t)&glScalef },
	{ "glClear", (uintptr_t)&glClear },
	{ "glOrthof", (uintptr_t)&glOrthof },
	{ "glViewport", (uintptr_t)&glViewport },
	{ "glScissor", (uintptr_t)&glScissor },
	{ "glEnable", (uintptr_t)&glEnable },
	{ "glDisable", (uintptr_t)&glDisable },
	{ "glUniform3fv", (uintptr_t)&glUniform3fv},
	{ "glUniform2f", (uintptr_t)&glUniform2f},
	{ "glUniform1f", (uintptr_t)&glUniform1f},
	{ "glUniform4f", (uintptr_t)&glUniform4f},
	{ "glUniform4fv", (uintptr_t)&glUniform4fv},
	{ "glIsTexture", (uintptr_t)&glIsTexture},
	{ "glIsRenderbuffer", (uintptr_t)&ret0},
	{ "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
	{ "glGetRenderbufferParameteriv", (uintptr_t)&ret0},
	{ "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
	{ "glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
	{ "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
	{ "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
	{ "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
	{ "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
	{ "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
	{ "glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
	{ "glGenBuffers", (uintptr_t)&glGenBuffers},
	{ "glBufferSubData", (uintptr_t)&glBufferSubData},
	{ "glBufferData", (uintptr_t)&glBufferData},
	{ "glLineWidth", (uintptr_t)&glLineWidth},
	{ "_Znwj", (uintptr_t)&_Znwj},
	{ "glCreateProgram", (uintptr_t)&glCreateProgram},
	{ "glAttachShader", (uintptr_t)&glAttachShader},
	{ "glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
	{ "glLinkProgram", (uintptr_t)&glLinkProgram},
	{ "glGetProgramiv", (uintptr_t)&glGetProgramiv},
	{ "glGetFramebufferAttachmentParameteriv", (uintptr_t)&glGetFramebufferAttachmentParameteriv},
	{ "glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
	{ "glUseProgram", (uintptr_t)&glUseProgram},
	{ "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
	{ "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
	{ "glDeleteProgram", (uintptr_t)&glDeleteProgram},
	{ "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
	{ "glDepthMask", (uintptr_t)&glDepthMask},
	{ "glGetIntegerv", (uintptr_t)&glGetIntegerv},
	{ "glUniform1i", (uintptr_t)&glUniform1i},
	{ "glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
	{ "glActiveTexture", (uintptr_t)&glActiveTexture},
	{ "glTexParameterf", (uintptr_t)&glTexParameterf},
	{ "glPixelStorei", (uintptr_t)&ret0},
	{ "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
	{ "glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
	{ "_ZdlPv", (uintptr_t)&_ZdlPv},
	{ "glGetString", (uintptr_t)&glGetString},
	{ "glGetFloatv", (uintptr_t)&glGetFloatv},
	{ "glBindBuffer", (uintptr_t)&glBindBuffer},
	{ "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
	{ "glDrawArrays", (uintptr_t)&glDrawArrays},
	{ "glCreateShader", (uintptr_t)&glCreateShader},
	{ "glShaderSource", (uintptr_t)&glShaderSource},
	{ "glCompileShader", (uintptr_t)&glCompileShader},
	{ "glGetShaderiv", (uintptr_t)&glGetShaderiv},
	{ "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
	{ "glDeleteShader", (uintptr_t)&glDeleteShader},
	{ "eglGetProcAddress", (uintptr_t)&eglGetProcAddress},
	{ "_Znaj", (uintptr_t)&_Znaj},
	{ "glEnableClientState", (uintptr_t)&glEnableClientState },
	{ "glDisableClientState", (uintptr_t)&glDisableClientState },
	{ "glBlendFunc", (uintptr_t)&glBlendFunc },
	{ "glColorPointer", (uintptr_t)&glColorPointer },
	{ "glVertexPointer", (uintptr_t)&glVertexPointer },
	{ "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
	{ "glDrawElements", (uintptr_t)&glDrawElements },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "IMG_Load", (uintptr_t)&IMG_Load_hook },
	{ "IMG_LoadTexture", (uintptr_t)&IMG_LoadTexture_hook },
	{ "IMG_LoadTexture_RW", (uintptr_t)&IMG_LoadTexture_RW },
	{ "raise", (uintptr_t)&raise },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	//printf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 0;
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return NULL;
	}
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetSlowTrulyRandomValue() {
	return rand();
}

int PfmGetSystemLanguageId() {
	int lang = -1;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	switch (lang) {
	case SCE_SYSTEM_PARAM_LANG_DUTCH:
		return 'n' | ('l' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_KOREAN:
		return 'k' | ('o' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_FINNISH:
		return 'f' | ('i' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_SWEDISH:
		return 's' | ('v' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_DANISH:
		return 'd' | ('a' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_NORWEGIAN:
		return 'n' | ('n' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_POLISH:
		return 'p' | ('l' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_TURKISH:
		return 't' | ('r' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_JAPANESE:
		return 0x706A;
	case SCE_SYSTEM_PARAM_LANG_CHINESE_S:
	case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
		return 0x746863;
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR:
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
		return 0x7062;
	case SCE_SYSTEM_PARAM_LANG_FRENCH:
		return 'f' | ('r' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_SPANISH:
		return 'e' | ('s' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_GERMAN:
		return 'd' | ('e' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_RUSSIAN:
		return 'r' | ('u' << 8) | 0x2020;
	case SCE_SYSTEM_PARAM_LANG_ITALIAN:
		return 'i' | ('t' << 8) | 0x2020;
	default:
		return 'e' | ('n' << 8) | 0x2020;
	}
}

int SetContextVersion(int version) {
	return version;
}

int GetCurrentPlatformClass() {
	return 2;
}

so_hook ogl_hook;

int ogl_LoadFunctions() {
	int r = SO_CONTINUE(int, ogl_hook);
	int *mapbuffer_ext = (int *)so_symbol(&hrm_mod, "ogl_ext_OES_mapbuffer");
	//printf("MapBufferExt: %d\n", *mapbuffer_ext);
	*mapbuffer_ext = 1;
	return r;
}

void patch_game(void) {
	hook_addr(so_symbol(&hrm_mod, "_Z23GetCurrentPlatformClassv"), GetCurrentPlatformClass);
	hook_addr(so_symbol(&hrm_mod, "_Z21SDL2SetContextVersioni"), SetContextVersion);
	hook_addr(so_symbol(&hrm_mod, "_Z23GetSlowTrulyRandomValuev"), GetSlowTrulyRandomValue);
	hook_addr(so_symbol(&hrm_mod, "_Z22PfmGetSystemLanguageIdv"), PfmGetSystemLanguageId);
	ogl_hook = hook_addr(so_symbol(&hrm_mod, "ogl_LoadFunctions"), ogl_LoadFunctions);
	
	// openAL
	hook_addr(so_symbol(&hrm_mod, "alAuxiliaryEffectSlotf"), (uintptr_t)alAuxiliaryEffectSlotf);
	hook_addr(so_symbol(&hrm_mod, "alAuxiliaryEffectSlotfv"), (uintptr_t)alAuxiliaryEffectSlotfv);
	hook_addr(so_symbol(&hrm_mod, "alAuxiliaryEffectSloti"), (uintptr_t)alAuxiliaryEffectSloti);
	hook_addr(so_symbol(&hrm_mod, "alAuxiliaryEffectSlotiv"), (uintptr_t)alAuxiliaryEffectSlotiv);
	hook_addr(so_symbol(&hrm_mod, "alBuffer3f"), (uintptr_t)alBuffer3f);
	hook_addr(so_symbol(&hrm_mod, "alBuffer3i"), (uintptr_t)alBuffer3i);
	hook_addr(so_symbol(&hrm_mod, "alBufferData"), (uintptr_t)alBufferData);
	hook_addr(so_symbol(&hrm_mod, "alBufferSamplesSOFT"), (uintptr_t)alBufferSamplesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alBufferSubDataSOFT"), (uintptr_t)alBufferSubDataSOFT);
	hook_addr(so_symbol(&hrm_mod, "alBufferSubSamplesSOFT"), (uintptr_t)alBufferSubSamplesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alBufferf"), (uintptr_t)alBufferf);
	hook_addr(so_symbol(&hrm_mod, "alBufferfv"), (uintptr_t)alBufferfv);
	hook_addr(so_symbol(&hrm_mod, "alBufferi"), (uintptr_t)alBufferi);
	hook_addr(so_symbol(&hrm_mod, "alBufferiv"), (uintptr_t)alBufferiv);
	hook_addr(so_symbol(&hrm_mod, "alDeferUpdatesSOFT"), (uintptr_t)alDeferUpdatesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alDeleteAuxiliaryEffectSlots"), (uintptr_t)alDeleteAuxiliaryEffectSlots);
	hook_addr(so_symbol(&hrm_mod, "alDeleteBuffers"), (uintptr_t)alDeleteBuffers);
	hook_addr(so_symbol(&hrm_mod, "alDeleteEffects"), (uintptr_t)alDeleteEffects);
	hook_addr(so_symbol(&hrm_mod, "alDeleteFilters"), (uintptr_t)alDeleteFilters);
	hook_addr(so_symbol(&hrm_mod, "alDeleteSources"), (uintptr_t)alDeleteSources);
	hook_addr(so_symbol(&hrm_mod, "alDisable"), (uintptr_t)alDisable);
	hook_addr(so_symbol(&hrm_mod, "alDistanceModel"), (uintptr_t)alDistanceModel);
	hook_addr(so_symbol(&hrm_mod, "alDopplerFactor"), (uintptr_t)alDopplerFactor);
	hook_addr(so_symbol(&hrm_mod, "alDopplerVelocity"), (uintptr_t)alDopplerVelocity);
	hook_addr(so_symbol(&hrm_mod, "alEffectf"), (uintptr_t)alEffectf);
	hook_addr(so_symbol(&hrm_mod, "alEffectfv"), (uintptr_t)alEffectfv);
	hook_addr(so_symbol(&hrm_mod, "alEffecti"), (uintptr_t)alEffecti);
	hook_addr(so_symbol(&hrm_mod, "alEffectiv"), (uintptr_t)alEffectiv);
	hook_addr(so_symbol(&hrm_mod, "alEnable"), (uintptr_t)alEnable);
	hook_addr(so_symbol(&hrm_mod, "alFilterf"), (uintptr_t)alFilterf);
	hook_addr(so_symbol(&hrm_mod, "alFilterfv"), (uintptr_t)alFilterfv);
	hook_addr(so_symbol(&hrm_mod, "alFilteri"), (uintptr_t)alFilteri);
	hook_addr(so_symbol(&hrm_mod, "alFilteriv"), (uintptr_t)alFilteriv);
	hook_addr(so_symbol(&hrm_mod, "alGenBuffers"), (uintptr_t)alGenBuffers);
	hook_addr(so_symbol(&hrm_mod, "alGenEffects"), (uintptr_t)alGenEffects);
	hook_addr(so_symbol(&hrm_mod, "alGenFilters"), (uintptr_t)alGenFilters);
	hook_addr(so_symbol(&hrm_mod, "alGenSources"), (uintptr_t)alGenSources);
	hook_addr(so_symbol(&hrm_mod, "alGetAuxiliaryEffectSlotf"), (uintptr_t)alGetAuxiliaryEffectSlotf);
	hook_addr(so_symbol(&hrm_mod, "alGetAuxiliaryEffectSlotfv"), (uintptr_t)alGetAuxiliaryEffectSlotfv);
	hook_addr(so_symbol(&hrm_mod, "alGetAuxiliaryEffectSloti"), (uintptr_t)alGetAuxiliaryEffectSloti);
	hook_addr(so_symbol(&hrm_mod, "alGetAuxiliaryEffectSlotiv"), (uintptr_t)alGetAuxiliaryEffectSlotiv);
	hook_addr(so_symbol(&hrm_mod, "alGetBoolean"), (uintptr_t)alGetBoolean);
	hook_addr(so_symbol(&hrm_mod, "alGetBooleanv"), (uintptr_t)alGetBooleanv);
	hook_addr(so_symbol(&hrm_mod, "alGetBuffer3f"), (uintptr_t)alGetBuffer3f);
	hook_addr(so_symbol(&hrm_mod, "alGetBuffer3i"), (uintptr_t)alGetBuffer3i);
	hook_addr(so_symbol(&hrm_mod, "alGetBufferSamplesSOFT"), (uintptr_t)alGetBufferSamplesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetBufferf"), (uintptr_t)alGetBufferf);
	hook_addr(so_symbol(&hrm_mod, "alGetBufferfv"), (uintptr_t)alGetBufferfv);
	hook_addr(so_symbol(&hrm_mod, "alGetBufferi"), (uintptr_t)alGetBufferi);
	hook_addr(so_symbol(&hrm_mod, "alGetBufferiv"), (uintptr_t)alGetBufferiv);
	hook_addr(so_symbol(&hrm_mod, "alGetDouble"), (uintptr_t)alGetDouble);
	hook_addr(so_symbol(&hrm_mod, "alGetDoublev"), (uintptr_t)alGetDoublev);
	hook_addr(so_symbol(&hrm_mod, "alGetEffectf"), (uintptr_t)alGetEffectf);
	hook_addr(so_symbol(&hrm_mod, "alGetEffectfv"), (uintptr_t)alGetEffectfv);
	hook_addr(so_symbol(&hrm_mod, "alGetEffecti"), (uintptr_t)alGetEffecti);
	hook_addr(so_symbol(&hrm_mod, "alGetEffectiv"), (uintptr_t)alGetEffectiv);
	hook_addr(so_symbol(&hrm_mod, "alGetEnumValue"), (uintptr_t)alGetEnumValue);
	hook_addr(so_symbol(&hrm_mod, "alGetError"), (uintptr_t)alGetError);
	hook_addr(so_symbol(&hrm_mod, "alGetFilterf"), (uintptr_t)alGetFilterf);
	hook_addr(so_symbol(&hrm_mod, "alGetFilterfv"), (uintptr_t)alGetFilterfv);
	hook_addr(so_symbol(&hrm_mod, "alGetFilteri"), (uintptr_t)alGetFilteri);
	hook_addr(so_symbol(&hrm_mod, "alGetFilteriv"), (uintptr_t)alGetFilteriv);
	hook_addr(so_symbol(&hrm_mod, "alGetFloat"), (uintptr_t)alGetFloat);
	hook_addr(so_symbol(&hrm_mod, "alGetFloatv"), (uintptr_t)alGetFloatv);
	hook_addr(so_symbol(&hrm_mod, "alGetInteger"), (uintptr_t)alGetInteger);
	hook_addr(so_symbol(&hrm_mod, "alGetIntegerv"), (uintptr_t)alGetIntegerv);
	hook_addr(so_symbol(&hrm_mod, "alGetListener3f"), (uintptr_t)alGetListener3f);
	hook_addr(so_symbol(&hrm_mod, "alGetListener3i"), (uintptr_t)alGetListener3i);
	hook_addr(so_symbol(&hrm_mod, "alGetListenerf"), (uintptr_t)alGetListenerf);
	hook_addr(so_symbol(&hrm_mod, "alGetListenerfv"), (uintptr_t)alGetListenerfv);
	hook_addr(so_symbol(&hrm_mod, "alGetListeneri"), (uintptr_t)alGetListeneri);
	hook_addr(so_symbol(&hrm_mod, "alGetListeneriv"), (uintptr_t)alGetListeneriv);
	hook_addr(so_symbol(&hrm_mod, "alGetProcAddress"), (uintptr_t)alGetProcAddress);
	hook_addr(so_symbol(&hrm_mod, "alGetSource3dSOFT"), (uintptr_t)alGetSource3dSOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSource3f"), (uintptr_t)alGetSource3f);
	hook_addr(so_symbol(&hrm_mod, "alGetSource3i"), (uintptr_t)alGetSource3i);
	hook_addr(so_symbol(&hrm_mod, "alGetSource3i64SOFT"), (uintptr_t)alGetSource3i64SOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcedSOFT"), (uintptr_t)alGetSourcedSOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcedvSOFT"), (uintptr_t)alGetSourcedvSOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcef"), (uintptr_t)alGetSourcef);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcefv"), (uintptr_t)alGetSourcefv);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcei"), (uintptr_t)alGetSourcei);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcei64SOFT"), (uintptr_t)alGetSourcei64SOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSourcei64vSOFT"), (uintptr_t)alGetSourcei64vSOFT);
	hook_addr(so_symbol(&hrm_mod, "alGetSourceiv"), (uintptr_t)alGetSourceiv);
	hook_addr(so_symbol(&hrm_mod, "alGetString"), (uintptr_t)alGetString);
	hook_addr(so_symbol(&hrm_mod, "alIsAuxiliaryEffectSlot"), (uintptr_t)alIsAuxiliaryEffectSlot);
	hook_addr(so_symbol(&hrm_mod, "alIsBuffer"), (uintptr_t)alIsBuffer);
	hook_addr(so_symbol(&hrm_mod, "alIsBufferFormatSupportedSOFT"), (uintptr_t)alIsBufferFormatSupportedSOFT);
	hook_addr(so_symbol(&hrm_mod, "alIsEffect"), (uintptr_t)alIsEffect);
	hook_addr(so_symbol(&hrm_mod, "alIsEnabled"), (uintptr_t)alIsEnabled);
	hook_addr(so_symbol(&hrm_mod, "alIsExtensionPresent"), (uintptr_t)alIsExtensionPresent);
	hook_addr(so_symbol(&hrm_mod, "alIsFilter"), (uintptr_t)alIsFilter);
	hook_addr(so_symbol(&hrm_mod, "alIsSource"), (uintptr_t)alIsSource);
	hook_addr(so_symbol(&hrm_mod, "alListener3f"), (uintptr_t)alListener3f);
	hook_addr(so_symbol(&hrm_mod, "alListener3i"), (uintptr_t)alListener3i);
	hook_addr(so_symbol(&hrm_mod, "alListenerf"), (uintptr_t)alListenerf);
	hook_addr(so_symbol(&hrm_mod, "alListenerfv"), (uintptr_t)alListenerfv);
	hook_addr(so_symbol(&hrm_mod, "alListeneri"), (uintptr_t)alListeneri);
	hook_addr(so_symbol(&hrm_mod, "alListeneriv"), (uintptr_t)alListeneriv);
	hook_addr(so_symbol(&hrm_mod, "alProcessUpdatesSOFT"), (uintptr_t)alProcessUpdatesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alSetConfigMOB"), (uintptr_t)ret0);
	hook_addr(so_symbol(&hrm_mod, "alSource3dSOFT"), (uintptr_t)alSource3dSOFT);
	hook_addr(so_symbol(&hrm_mod, "alSource3f"), (uintptr_t)alSource3f);
	hook_addr(so_symbol(&hrm_mod, "alSource3i"), (uintptr_t)alSource3i);
	hook_addr(so_symbol(&hrm_mod, "alSource3i64SOFT"), (uintptr_t)alSource3i64SOFT);
	hook_addr(so_symbol(&hrm_mod, "alSourcePause"), (uintptr_t)alSourcePause);
	hook_addr(so_symbol(&hrm_mod, "alSourcePausev"), (uintptr_t)alSourcePausev);
	hook_addr(so_symbol(&hrm_mod, "alSourcePlay"), (uintptr_t)alSourcePlay);
	hook_addr(so_symbol(&hrm_mod, "alSourcePlayv"), (uintptr_t)alSourcePlayv);
	hook_addr(so_symbol(&hrm_mod, "alSourceQueueBuffers"), (uintptr_t)alSourceQueueBuffers);
	hook_addr(so_symbol(&hrm_mod, "alSourceRewind"), (uintptr_t)alSourceRewind);
	hook_addr(so_symbol(&hrm_mod, "alSourceRewindv"), (uintptr_t)alSourceRewindv);
	hook_addr(so_symbol(&hrm_mod, "alSourceStop"), (uintptr_t)alSourceStop);
	hook_addr(so_symbol(&hrm_mod, "alSourceStopv"), (uintptr_t)alSourceStopv);
	hook_addr(so_symbol(&hrm_mod, "alSourceUnqueueBuffers"), (uintptr_t)alSourceUnqueueBuffers);
	hook_addr(so_symbol(&hrm_mod, "alSourcedSOFT"), (uintptr_t)alSourcedSOFT);
	hook_addr(so_symbol(&hrm_mod, "alSourcedvSOFT"), (uintptr_t)alSourcedvSOFT);
	hook_addr(so_symbol(&hrm_mod, "alSourcef"), (uintptr_t)alSourcef);
	hook_addr(so_symbol(&hrm_mod, "alSourcefv"), (uintptr_t)alSourcefv);
	hook_addr(so_symbol(&hrm_mod, "alSourcei"), (uintptr_t)alSourcei);
	hook_addr(so_symbol(&hrm_mod, "alSourcei64SOFT"), (uintptr_t)alSourcei64SOFT);
	hook_addr(so_symbol(&hrm_mod, "alSourcei64vSOFT"), (uintptr_t)alSourcei64vSOFT);
	hook_addr(so_symbol(&hrm_mod, "alSourceiv"), (uintptr_t)alSourceiv);
	hook_addr(so_symbol(&hrm_mod, "alSpeedOfSound"), (uintptr_t)alSpeedOfSound);
	hook_addr(so_symbol(&hrm_mod, "alcCaptureCloseDevice"), (uintptr_t)alcCaptureCloseDevice);
	hook_addr(so_symbol(&hrm_mod, "alcCaptureOpenDevice"), (uintptr_t)alcCaptureOpenDevice);
	hook_addr(so_symbol(&hrm_mod, "alcCaptureSamples"), (uintptr_t)alcCaptureSamples);
	hook_addr(so_symbol(&hrm_mod, "alcCaptureStart"), (uintptr_t)alcCaptureStart);
	hook_addr(so_symbol(&hrm_mod, "alcCaptureStop"), (uintptr_t)alcCaptureStop);
	hook_addr(so_symbol(&hrm_mod, "alcCloseDevice"), (uintptr_t)alcCloseDevice);
	hook_addr(so_symbol(&hrm_mod, "alcCreateContext"), (uintptr_t)alcCreateContext);
	hook_addr(so_symbol(&hrm_mod, "alcDestroyContext"), (uintptr_t)alcDestroyContext);
	hook_addr(so_symbol(&hrm_mod, "alcDeviceEnableHrtfMOB"), (uintptr_t)ret0);
	hook_addr(so_symbol(&hrm_mod, "alcGetContextsDevice"), (uintptr_t)alcGetContextsDevice);
	hook_addr(so_symbol(&hrm_mod, "alcGetCurrentContext"), (uintptr_t)alcGetCurrentContext);
	hook_addr(so_symbol(&hrm_mod, "alcGetEnumValue"), (uintptr_t)alcGetEnumValue);
	hook_addr(so_symbol(&hrm_mod, "alcGetError"), (uintptr_t)alcGetError);
	hook_addr(so_symbol(&hrm_mod, "alcGetIntegerv"), (uintptr_t)alcGetIntegerv);
	hook_addr(so_symbol(&hrm_mod, "alcGetProcAddress"), (uintptr_t)alcGetProcAddress);
	hook_addr(so_symbol(&hrm_mod, "alcGetString"), (uintptr_t)alcGetString);
	hook_addr(so_symbol(&hrm_mod, "alcGetThreadContext"), (uintptr_t)alcGetThreadContext);
	hook_addr(so_symbol(&hrm_mod, "alcIsExtensionPresent"), (uintptr_t)alcIsExtensionPresent);
	hook_addr(so_symbol(&hrm_mod, "alcIsRenderFormatSupportedSOFT"), (uintptr_t)alcIsRenderFormatSupportedSOFT);
	hook_addr(so_symbol(&hrm_mod, "alcLoopbackOpenDeviceSOFT"), (uintptr_t)alcLoopbackOpenDeviceSOFT);
	hook_addr(so_symbol(&hrm_mod, "alcMakeContextCurrent"), (uintptr_t)alcMakeContextCurrent);
	hook_addr(so_symbol(&hrm_mod, "alcOpenDevice"), (uintptr_t)alcOpenDevice);
	hook_addr(so_symbol(&hrm_mod, "alcProcessContext"), (uintptr_t)alcProcessContext);
	hook_addr(so_symbol(&hrm_mod, "alcRenderSamplesSOFT"), (uintptr_t)alcRenderSamplesSOFT);
	hook_addr(so_symbol(&hrm_mod, "alcSetThreadContext"), (uintptr_t)alcSetThreadContext);
	hook_addr(so_symbol(&hrm_mod, "alcSuspendContext"), (uintptr_t)alcSuspendContext);
	hook_addr(so_symbol(&hrm_mod, "alBufferMarkNeedsFreed"), (uintptr_t)ret0);
	hook_addr(so_symbol(&hrm_mod, "_Z22alBufferMarkNeedsFreedj"), (uintptr_t)ret0);
	hook_addr(so_symbol(&hrm_mod, "_Z17alBufferDebugNamejPKc"), (uintptr_t)ret0);
}

void *hrm_main(void *argv) {
	char *args[1];
	args[0] = DATA_PATH;
	
	int (* SDL_main)(int argc, char *args[]) = (void *) so_symbol(&hrm_mod, "SDL_main");
	SDL_main(1, args);
	
	return NULL;
}

#define NUM_BUTTONS 6
typedef struct {
	int mask;
	int x;
	int y;
} btn_emu;
btn_emu btns[NUM_BUTTONS];

int __wrap_sceTouchPeek(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs) {
	int num = __real_sceTouchPeek(port, pData, nBufs);
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	for (int i = 0; i < NUM_BUTTONS; i++) {
		if (pad.buttons & btns[i].mask) {
			pData->reportNum = 1;
			pData->report[0].x = btns[i].x * 2;
			pData->report[0].y = btns[i].y * 2;
			break;
		}
	}
	return num;
}

int main(int argc, char *argv[]) {
	// Play
	btns[0].mask = SCE_CTRL_CROSS;
	btns[0].x = 240;
	btns[0].y = 502;
	// Restart
	btns[1].mask = SCE_CTRL_SQUARE;
	btns[1].x = 135;
	btns[1].y = 508;
	// Next Move
	btns[2].mask = SCE_CTRL_RTRIGGER;
	btns[2].x = 304;
	btns[2].y = 502;
	// Prev Move
	btns[3].mask = SCE_CTRL_LTRIGGER;
	btns[3].x = 186;
	btns[3].y = 502;
	// Cancel
	btns[4].mask = SCE_CTRL_START;
	btns[4].x = 37;
	btns[4].y = 515;
	// Mute/Unmute
	btns[5].mask = SCE_CTRL_TRIANGLE;
	btns[5].x = 509;
	btns[5].y = 516;
	
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);
	srand(time(NULL));
	
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error: kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error: libshacccg.suprx is not installed.");
	
	printf("Loading libc++_shared\n");
	if (so_file_load(&cpp_mod, DATA_PATH "/libc++_shared.so", LOAD_ADDRESS + 0x3000000) < 0)
		fatal_error("Error could not load %s.", DATA_PATH "/libc++_shared.so");
	so_relocate(&cpp_mod);
	so_resolve(&cpp_mod, default_dynlib, sizeof(default_dynlib), 0);
	so_flush_caches(&cpp_mod);
	so_initialize(&cpp_mod);
	
	printf("Loading libHumanResourceMachine\n");
	if (so_file_load(&hrm_mod, DATA_PATH "/libHumanResourceMachine.so", LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", DATA_PATH "/libHumanResourceMachine.so");
	so_relocate(&hrm_mod);
	so_resolve(&hrm_mod, default_dynlib, sizeof(default_dynlib), 0);
	
	patch_game();
	so_flush_caches(&hrm_mod);
	so_initialize(&hrm_mod);
	
	vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; //PushLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	
	// Disabling rearpad
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 0x800000);
	pthread_create(&t, &attr,hrm_main, NULL);
	pthread_join(t, NULL);
	
	return 0;
}
