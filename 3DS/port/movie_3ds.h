#ifndef PORT_MOVIE_3DS_H
#define PORT_MOVIE_3DS_H

// Attract movie playback from build-time ETC1 frames (tools/video_convert.py,
// GRAPHICS_PLAN.md section 7). THPVideoDecode is replaced: instead of decoding
// the frame's JPEG into the game's Y/U/V buffers, it loads the matching
// pre-encoded frame into a texture registered under the Y buffer's address.
// The GX backend binds that texture when the game binds the Y plane and
// compiles the game's YUV->RGB TEV stages to a single texture read.

#include <stdbool.h>
#include <citro3d.h>

enum { MOVIE_PLANE_NONE, MOVIE_PLANE_Y, MOVIE_PLANE_UV };

// Which movie plane a texture address is; for MOVIE_PLANE_Y, *tex gets the
// frame texture. Cheap when no movie has played.
int movie_plane(const void* data, C3D_Tex** tex);
bool movie_active(void);
void movie_reset_slots(void);  // Save state restore: forget the buffer addresses

#endif // PORT_MOVIE_3DS_H
