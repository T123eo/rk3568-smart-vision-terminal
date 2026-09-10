#include <stdio.h>
#include <SDL2/SDL.h>

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    printf("SDL init success\n");

    SDL_Quit();

    return 0;
}