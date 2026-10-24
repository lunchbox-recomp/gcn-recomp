/* A program that needs what <build>/game_libs.txt says the game links after SDL, which
 * test_game_link.cmake links this with: the platform's GL, and the system libraries and
 * frameworks SDL's own code calls into. It declares the two libraries' functions itself, so
 * it needs neither one's headers. SDL_Init(0) starts no subsystem, so it runs anywhere. */
#include <stdbool.h>

bool SDL_Init(unsigned int flags);
void SDL_Quit(void);
unsigned int glGetError(void);

/* Taken, not called: GL wants a context first. */
unsigned int (*volatile gcn_test_gl_get_error)(void) = glGetError;

int main(void) {
  if (!SDL_Init(0)) return 1;
  SDL_Quit();
  return 0;
}
