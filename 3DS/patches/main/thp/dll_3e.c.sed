# The PC player decodes at most one movie frame per VI retrace callback, and
# the port calls it once per game frame (20-30 fps on a 3DS, ~20 in Azahar
# during the movie), so the movie ran slower than real time and its audio
# track ended first. Catch up with every frame that is due, up to 4 per
# callback; a longer stall still drops time as before.
/^    if (now >= sPcMovieNextFrameTime) {$/,/^    }$/c\
    for (int due = 0; due < 4 && now >= sPcMovieNextFrameTime; due++) {\
        DecodeNextMovieFramePC();\
        sPcMovieNextFrameTime += sPcMovieFrameTicks;\
    }\
    if (now - sPcMovieNextFrameTime >= sPcMovieFrameTicks) {\
        sPcMovieNextFrameTime = now + sPcMovieFrameTicks;\
    }
