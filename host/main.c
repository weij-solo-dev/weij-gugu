// host/main.c — the simulator process. SDL must own the main thread on macOS, so the window, input
// pump and presentation live here; app_main() (the same code as on the device) runs in a worker
// thread. Flags: --scale N (window scale), --shot path.bmp --after ms (screenshot then exit),
// --quit-after ms, --replay file.wav [--turns N] (replay the utterance through a real call, then exit).
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "board/board.h"

void app_main(void);
// provided by board_host.c
void board_host_begin(int scale);
void board_host_sdl_event(const SDL_Event *e);
void board_host_present(SDL_Renderer *r, SDL_Texture *t);
void board_host_screenshot(SDL_Renderer *r, const char *path);

static void *app_thread(void *arg)
{
    (void)arg;
    app_main();
    return NULL;
}

int main(int argc, char **argv)
{
    int scale = 1, quit_after = 0, shot_after = 0;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc) quit_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--after") && i + 1 < argc) shot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) { setenv("GUGU_REPLAY_WAV", argv[++i], 1); setenv("GUGU_NO_MIC", "1", 1); }
        else if (!strcmp(argv[i], "--turns") && i + 1 < argc) setenv("GUGU_REPLAY_TURNS", argv[++i], 1);
        else if (!strcmp(argv[i], "--replay-list") && i + 1 < argc) { setenv("GUGU_REPLAY_WAVS", argv[++i], 1); setenv("GUGU_NO_MIC", "1", 1); }
    }
    if (scale < 1) scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("gugu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       BOARD_DISPLAY_W * scale, BOARD_DISPLAY_H * scale, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                         BOARD_DISPLAY_W, BOARD_DISPLAY_H);
    board_host_begin(scale);

    pthread_t th;
    pthread_create(&th, NULL, app_thread, NULL);

    Uint32 t0 = SDL_GetTicks();
    bool shot_done = false;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto out;
            board_host_sdl_event(&e);
        }
        board_host_present(ren, tex);
        Uint32 el = SDL_GetTicks() - t0;
        if (shot && !shot_done && el >= (Uint32)shot_after) {
            board_host_screenshot(ren, shot);
            shot_done = true;
            if (!quit_after) goto out;
        }
        if (quit_after && el >= (Uint32)quit_after) goto out;
        SDL_Delay(8);
    }
out:
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    _exit(0);
}
