#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <naomi/video.h>
#include <naomi/audio.h>
#include <naomi/maple.h>
#include <naomi/eeprom.h>
#include <naomi/thread.h>
#include <naomi/romfs.h>
#include <naomi/timer.h>

#define REPEAT_INITIAL_DELAY 500000
#define REPEAT_SUBSEQUENT_DELAY 25000

unsigned int repeat(unsigned int cur_state, int *repeat_count)
{
    // A held button will "repeat" itself 40x a second after a 1/2 second hold delay.
    if (*repeat_count < 0)
    {
        // If we have never pushed this button, don't try repeating
        // if it happened to be held.
        return 0;
    }

    if (cur_state == 0)
    {
        // Button isn't held, no repeats.
        timer_stop(*repeat_count);
        *repeat_count = -1;
        return 0;
    }

    if (timer_left(*repeat_count) == 0)
    {
        // We should restart this timer with a shorter delay
        // because we're in a repeat zone.
        timer_stop(*repeat_count);
        *repeat_count = timer_start(REPEAT_SUBSEQUENT_DELAY);
        return 1;
    }

    // Not currently being repeated.
    return 0;
}

void repeat_init(unsigned int pushed_state, int *repeat_count)
{
    if (pushed_state == 0)
    {
        // Haven't pushed the button yet.
        return;
    }

    // Clear out old timer if needed.
    if (*repeat_count >= 0)
    {
        timer_stop(*repeat_count);
    }

    // Set up a half-second timer for our first repeat.
    *repeat_count = timer_start(REPEAT_INITIAL_DELAY);
}

float chance()
{
    return (float)rand() / (float)RAND_MAX;
}

void *sprite_load(const char * const path)
{
    FILE *fp = fopen(path, "rb");
    if (fp)
    {
        // Get size of file.
        unsigned int size;
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        // Allocate space for the file.
        void *data = malloc(size);
        if (data)
        {
            fread(data, 1, size, fp);
            fclose(fp);

            return data;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return 0;
    }
}

void *sprite_dup_rotate_cw(void *sprite, int width, int height, int depth)
{
    switch(depth)
    {
        case 16:
        {
            uint16_t *data = (uint16_t *)sprite;
            uint16_t *newdata = malloc(width * height * sizeof(uint16_t));

            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int newx = (height - y) - 1;
                    int newy = x;
                    newdata[newx + (newy * width)] = data[x + (y * width)];
                }
            }

            return newdata;
        }
        default:
        {
            return 0;
        }
    }
}

// Core game rule adjustments.
#define PLAYFIELD_WIDTH 9
#define PLAYFIELD_HEIGHT 11

int gamerule_gravity = 0;
int gamerule_rotation = 0;
int gamerule_dragging = 0;
int gamerule_placing = 1;

typedef struct
{
    unsigned int block;
    unsigned int pipe;
    unsigned int color;
    unsigned int age;
} playfield_entry_t;

typedef struct
{
    unsigned int color;
} source_entry_t;

#define SOURCE_COLOR_NONE 0
#define SOURCE_COLOR_RED 0x1
#define SOURCE_COLOR_GREEN 0x2
#define SOURCE_COLOR_BLUE 0x4

#define UPNEXT_AMOUNT 5

typedef struct
{
    int width;
    int height;
    int curx;
    int cury;
    playfield_entry_t *entries;
    source_entry_t *sources;
    playfield_entry_t *upnext;
} playfield_t;

#define BLOCK_TYPE_NONE 0
#define BLOCK_TYPE_PURPLE 1
#define BLOCK_TYPE_ORANGE 2
#define BLOCK_TYPE_BLUE 3
#define BLOCK_TYPE_GREEN 4
#define BLOCK_TYPE_GRAY 5

#define PIPE_CONN_NONE 0
#define PIPE_CONN_N 0x1
#define PIPE_CONN_E 0x2
#define PIPE_CONN_S 0x4
#define PIPE_CONN_W 0x8

#define BLOCK_WIDTH 32
#define BLOCK_HEIGHT 32

#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64
#define CURSOR_OFFSET_X -16
#define CURSOR_OFFSET_Y -16

void *cursor = 0;

void *block_purple = 0;
void *block_orange = 0;
void *block_blue = 0;
void *block_green = 0;
void *block_gray = 0;

void *pipe_ns = 0;
void *red_ns = 0;
void *green_ns = 0;
void *blue_ns = 0;
void *cyan_ns = 0;
void *magenta_ns = 0;
void *yellow_ns = 0;
void *white_ns = 0;

void *pipe_ew = 0;
void *red_ew = 0;
void *green_ew = 0;
void *blue_ew = 0;
void *cyan_ew = 0;
void *magenta_ew = 0;
void *yellow_ew = 0;
void *white_ew = 0;

void *pipe_ne = 0;
void *red_ne = 0;
void *green_ne = 0;
void *blue_ne = 0;
void *cyan_ne = 0;
void *magenta_ne = 0;
void *yellow_ne = 0;
void *white_ne = 0;

void *pipe_se = 0;
void *red_se = 0;
void *green_se = 0;
void *blue_se = 0;
void *cyan_se = 0;
void *magenta_se = 0;
void *yellow_se = 0;
void *white_se = 0;

void *pipe_nw = 0;
void *red_nw = 0;
void *green_nw = 0;
void *blue_nw = 0;
void *cyan_nw = 0;
void *magenta_nw = 0;
void *yellow_nw = 0;
void *white_nw = 0;

void *pipe_sw = 0;
void *red_sw = 0;
void *green_sw = 0;
void *blue_sw = 0;
void *cyan_sw = 0;
void *magenta_sw = 0;
void *yellow_sw = 0;
void *white_sw = 0;

void *source_n = 0;
void *source_e = 0;
void *source_w = 0;
void *source_s = 0;

void *source_red = 0;
void *source_green = 0;
void *source_blue = 0;
void *source_cyan = 0;
void *source_magenta = 0;
void *source_yellow = 0;
void *source_white = 0;

void *red_n = 0;
void *green_n = 0;
void *blue_n = 0;
void *cyan_n = 0;
void *magenta_n = 0;
void *yellow_n = 0;
void *white_n = 0;

void *red_s = 0;
void *green_s = 0;
void *blue_s = 0;
void *cyan_s = 0;
void *magenta_s = 0;
void *yellow_s = 0;
void *white_s = 0;

void *red_e = 0;
void *green_e = 0;
void *blue_e = 0;
void *cyan_e = 0;
void *magenta_e = 0;
void *yellow_e = 0;
void *white_e = 0;

void *red_w = 0;
void *green_w = 0;
void *blue_w = 0;
void *cyan_w = 0;
void *magenta_w = 0;
void *yellow_w = 0;
void *white_w = 0;

#define PLAYFIELD_BORDER 2

void playfield_metrics(playfield_t *playfield, int *width, int *height)
{
    *width = (playfield->width + 2) * BLOCK_WIDTH;
    *height = (playfield->height + 2) * BLOCK_HEIGHT;

    if (gamerule_placing)
    {
        *width += BLOCK_WIDTH * 3;
    }
}

playfield_entry_t *playfield_entry(playfield_t *playfield, int x, int y)
{
    return playfield->entries + (y * playfield->width) + x;
}

void *playfield_block_sprite(playfield_entry_t *cur)
{
    switch(cur->block)
    {
        case BLOCK_TYPE_PURPLE:
        {
            return block_purple;
            break;
        }
        case BLOCK_TYPE_ORANGE:
        {
            return block_orange;
            break;
        }
        case BLOCK_TYPE_BLUE:
        {
            return block_blue;
            break;
        }
        case BLOCK_TYPE_GREEN:
        {
            return block_green;
            break;
        }
    }

    return 0;
}

void *playfield_pipe_sprite(playfield_entry_t *cur)
{
    switch(cur->pipe)
    {
        case PIPE_CONN_E | PIPE_CONN_W:
        {
            return pipe_ew;
        }
        case PIPE_CONN_N | PIPE_CONN_S:
        {
            return pipe_ns;
        }
        case PIPE_CONN_N | PIPE_CONN_E:
        {
            return pipe_ne;
        }
        case PIPE_CONN_N | PIPE_CONN_W:
        {
            return pipe_nw;
        }
        case PIPE_CONN_S | PIPE_CONN_E:
        {
            return pipe_se;
        }
        case PIPE_CONN_S | PIPE_CONN_W:
        {
            return pipe_sw;
        }
    }

    return 0;
}

void *playfield_color_sprite(playfield_entry_t *cur)
{
    switch(cur->pipe)
    {
        case PIPE_CONN_E | PIPE_CONN_W:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_ew;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_ew;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_ew;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_ew;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_ew;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_ew;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_ew;
            }
            break;
        }
        case PIPE_CONN_N | PIPE_CONN_S:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_ns;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_ns;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_ns;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_ns;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_ns;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_ns;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_ns;
            }
            break;
        }
        case PIPE_CONN_N | PIPE_CONN_E:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_ne;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_ne;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_ne;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_ne;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_ne;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_ne;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_ne;
            }
            break;
        }
        case PIPE_CONN_N | PIPE_CONN_W:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_nw;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_nw;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_nw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_nw;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_nw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_nw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_nw;
            }
            break;
        }
        case PIPE_CONN_S | PIPE_CONN_E:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_se;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_se;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_se;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_se;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_se;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_se;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_se;
            }
            break;
        }
        case PIPE_CONN_S | PIPE_CONN_W:
        {
            if (cur->color == SOURCE_COLOR_RED)
            {
                return red_sw;
            }
            if (cur->color == SOURCE_COLOR_GREEN)
            {
                return green_sw;
            }
            if (cur->color == SOURCE_COLOR_BLUE)
            {
                return blue_sw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
            {
                return magenta_sw;
            }
            if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return cyan_sw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
            {
                return yellow_sw;
            }
            if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
            {
                return white_sw;
            }
            break;
        }
    }

    return 0;
}

void playfield_draw(int x, int y, playfield_t *playfield)
{
    video_draw_box(
        x + BLOCK_WIDTH - PLAYFIELD_BORDER,
        y + BLOCK_HEIGHT - PLAYFIELD_BORDER,
        x + (BLOCK_WIDTH * (playfield->width + 1)) + (PLAYFIELD_BORDER - 1),
        y + (BLOCK_HEIGHT * (playfield->height + 1)) + (PLAYFIELD_BORDER - 1),
        rgb(255, 255, 255)
    );
    video_draw_box(
        x + BLOCK_WIDTH - PLAYFIELD_BORDER - 1,
        y + BLOCK_HEIGHT - PLAYFIELD_BORDER - 1,
        x + (BLOCK_WIDTH * (playfield->width + 1)) + PLAYFIELD_BORDER,
        y + (BLOCK_HEIGHT * (playfield->height + 1)) + PLAYFIELD_BORDER,
        rgb(255, 255, 255)
    );

    if (gamerule_placing)
    {
        video_draw_box(
            x + (BLOCK_WIDTH * (playfield->width + 4)) - PLAYFIELD_BORDER,
            y + BLOCK_HEIGHT - PLAYFIELD_BORDER,
            x + (BLOCK_WIDTH * (playfield->width + 5)) + (PLAYFIELD_BORDER - 1),
            y + BLOCK_HEIGHT * (1 + UPNEXT_AMOUNT) + (PLAYFIELD_BORDER - 1),
            rgb(255, 255, 255)
        );
        video_draw_box(
            x + (BLOCK_WIDTH * (playfield->width + 4)) - PLAYFIELD_BORDER - 1,
            y + BLOCK_HEIGHT - PLAYFIELD_BORDER - 1,
            x + (BLOCK_WIDTH * (playfield->width + 5)) + (PLAYFIELD_BORDER),
            y + BLOCK_HEIGHT * (1 + UPNEXT_AMOUNT) + (PLAYFIELD_BORDER),
            rgb(255, 255, 255)
        );

        for (int i = 0; i < UPNEXT_AMOUNT; i++)
        {
            playfield_entry_t *cur = &playfield->upnext[i];
            void *blocksprite = playfield_block_sprite(cur);
            void *pipesprite = playfield_pipe_sprite(cur);
            int xloc = x + (BLOCK_WIDTH * (playfield->width + 4));
            int yloc = y + (BLOCK_HEIGHT * (i + 1));

            if (blocksprite != 0)
            {
                video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, blocksprite);

                // Only draw pipes if there are blocks.
                if (pipesprite != 0)
                {
                    video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipesprite);
                }
            }
        }
    }

    for (int pheight = -1; pheight <= playfield->height; pheight++)
    {
        for (int pwidth = -1; pwidth <= playfield->width; pwidth++)
        {
            int xloc = x + ((pwidth + 1) * BLOCK_WIDTH);
            int yloc = y + ((pheight + 1) * BLOCK_HEIGHT);

            // First, draw the blocks on the playfield.
            if (pheight >= 0 && pheight < playfield->height && pwidth >= 0 && pwidth < playfield->width)
            {
                playfield_entry_t *cur = playfield_entry(playfield, pwidth, pheight);
                void *blocksprite = 0;

                // Handle displaying cursor ghost.
                if (cur->block == BLOCK_TYPE_NONE)
                {
                    if (gamerule_placing && playfield->upnext->block != BLOCK_TYPE_NONE && playfield->curx == pwidth && playfield->cury == pheight)
                    {
                        blocksprite = block_gray;
                        cur = playfield->upnext;
                    }
                }
                else
                {
                    blocksprite = playfield_block_sprite(cur);
                }

                void *pipesprite = playfield_pipe_sprite(cur);
                void *colorsprite = playfield_color_sprite(cur);

                if (blocksprite != 0)
                {
                    video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, blocksprite);

                    // Only draw pipes if there are blocks.
                    if (pipesprite != 0)
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipesprite);

                        // Only draw colors if there are pipes.
                        if (colorsprite != 0)
                        {
                            video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, colorsprite);
                        }
                    }
                }
            }

            // Now draw sources around the edges.
            source_entry_t *source = 0;
            void *sourcesprite = 0;
            void *pipecolorsprite = 0;
            if (pwidth == -1)
            {
                if (pheight >= 0 && pheight < playfield->height)
                {
                    source = playfield->sources + pheight;
                    sourcesprite = source_e;
                    playfield_entry_t *adj = playfield_entry(playfield, 0, pheight);
                    if (adj->pipe & PIPE_CONN_W)
                    {
                        switch(adj->color)
                        {
                            case SOURCE_COLOR_RED:
                            {
                                pipecolorsprite = red_e;
                                break;
                            }
                            case SOURCE_COLOR_GREEN:
                            {
                                pipecolorsprite = green_e;
                                break;
                            }
                            case SOURCE_COLOR_BLUE:
                            {
                                pipecolorsprite = blue_e;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = magenta_e;
                                break;
                            }
                            case (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = cyan_e;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN):
                            {
                                pipecolorsprite = yellow_e;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = white_e;
                                break;
                            }
                        }
                    }
                }
            }
            else if (pwidth == playfield->width)
            {
                if (pheight >= 0 && pheight < playfield->height)
                {
                    source = playfield->sources + playfield->height + pheight;
                    sourcesprite = source_w;
                    playfield_entry_t *adj = playfield_entry(playfield, playfield->width - 1, pheight);
                    if (adj->pipe & PIPE_CONN_E)
                    {
                        switch(adj->color)
                        {
                            case SOURCE_COLOR_RED:
                            {
                                pipecolorsprite = red_w;
                                break;
                            }
                            case SOURCE_COLOR_GREEN:
                            {
                                pipecolorsprite = green_w;
                                break;
                            }
                            case SOURCE_COLOR_BLUE:
                            {
                                pipecolorsprite = blue_w;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = magenta_w;
                                break;
                            }
                            case (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = cyan_w;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN):
                            {
                                pipecolorsprite = yellow_w;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = white_w;
                                break;
                            }
                        }
                    }
                }
            }
            else if (pheight == playfield->height)
            {
                if (pwidth >= 0 && pwidth < playfield->width)
                {
                    source = playfield->sources + (2 * playfield->height) + pwidth;
                    sourcesprite = source_n;
                    playfield_entry_t *adj = playfield_entry(playfield, pwidth, playfield->height - 1);
                    if (adj->pipe & PIPE_CONN_S)
                    {
                        switch(adj->color)
                        {
                            case SOURCE_COLOR_RED:
                            {
                                pipecolorsprite = red_n;
                                break;
                            }
                            case SOURCE_COLOR_GREEN:
                            {
                                pipecolorsprite = green_n;
                                break;
                            }
                            case SOURCE_COLOR_BLUE:
                            {
                                pipecolorsprite = blue_n;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = magenta_n;
                                break;
                            }
                            case (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = cyan_n;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN):
                            {
                                pipecolorsprite = yellow_n;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = white_n;
                                break;
                            }
                        }
                    }
                }
            }
            else if (pheight == -1)
            {
                if (pwidth >= 0 && pwidth < playfield->width)
                {
                    source = playfield->sources + (2 * playfield->height) + playfield->width + pwidth;
                    sourcesprite = source_s;
                    playfield_entry_t *adj = playfield_entry(playfield, pwidth, 0);
                    if (adj->pipe & PIPE_CONN_N)
                    {
                        switch(adj->color)
                        {
                            case SOURCE_COLOR_RED:
                            {
                                pipecolorsprite = red_s;
                                break;
                            }
                            case SOURCE_COLOR_GREEN:
                            {
                                pipecolorsprite = green_s;
                                break;
                            }
                            case SOURCE_COLOR_BLUE:
                            {
                                pipecolorsprite = blue_s;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = magenta_s;
                                break;
                            }
                            case (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = cyan_s;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN):
                            {
                                pipecolorsprite = yellow_s;
                                break;
                            }
                            case (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE):
                            {
                                pipecolorsprite = white_s;
                                break;
                            }
                        }
                    }
                }
            }

            if (source != 0)
            {
                if (source->color != SOURCE_COLOR_NONE)
                {
                    video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, sourcesprite);
                }
                if (pipecolorsprite != 0)
                {
                    video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipecolorsprite);
                }

                switch(source->color)
                {
                    case SOURCE_COLOR_RED:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_red);
                        break;
                    }
                    case SOURCE_COLOR_GREEN:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_green);
                        break;
                    }
                    case SOURCE_COLOR_BLUE:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_blue);
                        break;
                    }
                    case SOURCE_COLOR_RED | SOURCE_COLOR_BLUE:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_magenta);
                        break;
                    }
                    case SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_cyan);
                        break;
                    }
                    case SOURCE_COLOR_RED | SOURCE_COLOR_GREEN:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_yellow);
                        break;
                    }
                    case SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, source_white);
                        break;
                    }
                }
            }

            // Finally, draw the cursor
            if (pwidth == playfield->curx && pheight == playfield->cury)
            {
                video_draw_sprite(xloc + CURSOR_OFFSET_X, yloc + CURSOR_OFFSET_Y, CURSOR_WIDTH, CURSOR_HEIGHT, cursor);
            }
        }
    }
}

playfield_t *playfield_new(int width, int height)
{
    playfield_entry_t *entries = malloc(sizeof(playfield_entry_t) * width * height);
    memset(entries, 0, sizeof(playfield_entry_t) * width * height);

    source_entry_t *sources = malloc(sizeof(source_entry_t) * ((width * 2) + (height * 2)));
    memset(sources, 0, sizeof(source_entry_t) * ((width * 2) + (height * 2)));

    playfield_entry_t *upnext = malloc(sizeof(playfield_entry_t) * UPNEXT_AMOUNT);
    memset(upnext, 0, sizeof(playfield_entry_t) * UPNEXT_AMOUNT);

    playfield_t *playfield = malloc(sizeof(playfield_t));
    playfield->width = width;
    playfield->height = height;
    playfield->entries = entries;
    playfield->sources = sources;
    playfield->upnext = upnext;

    playfield->curx = width / 2;
    playfield->cury = height / 2;

    return playfield;
}

void playfield_set_block(playfield_t *playfield, int x, int y, unsigned int block, unsigned int pipe)
{
    playfield_entry_t *cur = playfield_entry(playfield, x, y);
    cur->block = block;
    cur->pipe = pipe;
}

void playfield_generate_block(playfield_t *playfield, int x, int y, float block_chance)
{
    static unsigned int bits[4] = { PIPE_CONN_N, PIPE_CONN_E, PIPE_CONN_S, PIPE_CONN_W };

    if (chance() <= block_chance)
    {
        // First handle the color chance (asthetic only).
        playfield_entry_t *cur = playfield_entry(playfield, x, y);
        int color = (int)(chance() * 4.0) + 1;
        cur->block = color;

        // Now handle the connections.
        int corner = (int)(chance() * 4.0);
        int second = (int)(chance() * 3.0);
        cur->pipe = bits[corner] | bits[(corner + (second > 0 ? 2 : 1)) % 4];
    }
}

void playfield_generate_upnext(playfield_t *playfield)
{
    static unsigned int bits[4] = { PIPE_CONN_N, PIPE_CONN_E, PIPE_CONN_S, PIPE_CONN_W };

    for (int i = 0; i < UPNEXT_AMOUNT; i++)
    {
        playfield_entry_t *cur = playfield->upnext + i;

        if (cur->block == BLOCK_TYPE_NONE)
        {
            int color = (int)(chance() * 4.0) + 1;
            cur->block = color;

            int corner = (int)(chance() * 4.0);
            int second = (int)(chance() * 2.0);

            cur->pipe = bits[corner] | bits[(corner + (second > 0 ? 2 : 1)) % 4];
        }
    }
}

void playfield_set_source(playfield_t *playfield, int x, int y, unsigned int color)
{
    if (x == -1)
    {
        source_entry_t *cur = playfield->sources + y;
        cur->color = color;
    }
    else if (x == playfield->width)
    {
        source_entry_t *cur = playfield->sources + playfield->height + y;
        cur->color = color;
    }
    else if (y == playfield->height)
    {
        source_entry_t *cur = playfield->sources + (2 * playfield->height) + x;
        cur->color = color;
    }
    else if (y == -1)
    {
        source_entry_t *cur = playfield->sources + (2 * playfield->height) + playfield->width + x;
        cur->color = color;
    }
}

int playfield_touches_light(playfield_t *playfield, int x, int y, int in_direction, int color)
{
    // First, if this doesn't have a connection in the in direction, its always false.
    playfield_entry_t *cur = playfield_entry(playfield, x, y);
    if ((cur->pipe & in_direction) == 0)
    {
        return 0;
    }

    // Calculate the other direction of the pipe by removing the in direction.
    unsigned int out_direction = cur->pipe & (~in_direction);
    switch(out_direction)
    {
        case PIPE_CONN_N:
        {
            // Goes out north. Either it hits the sky or it goes to another block.
            if (y == 0)
            {
                source_entry_t *source = playfield->sources + (2 * playfield->height) + playfield->width + x;
                return (source->color & color) == color;
            }
            else
            {
                return playfield_touches_light(playfield, x, y - 1, PIPE_CONN_S, color);
            }
        }
        case PIPE_CONN_S:
        {
            // Goes out south. Either it hits a light block or it goes to another block.
            if (y == playfield->height - 1)
            {
                source_entry_t *source = playfield->sources + (2 * playfield->height) + x;
                return (source->color & color) == color;
            }
            else
            {
                return playfield_touches_light(playfield, x, y + 1, PIPE_CONN_N, color);
            }
        }
        case PIPE_CONN_E:
        {
            // Goes out east. Either it hits a light block or it goes to another block.
            if (x == playfield->width - 1)
            {
                source_entry_t *source = playfield->sources + playfield->height + y;
                return (source->color & color) == color;
            }
            else
            {
                return playfield_touches_light(playfield, x + 1, y, PIPE_CONN_W, color);
            }
        }
        case PIPE_CONN_W:
        {
            // Goes out west. Either it hits a light block or it goes to another block.
            if (x == 0)
            {
                source_entry_t *source = playfield->sources + y;
                return (source->color & color) == color;
            }
            else
            {
                return playfield_touches_light(playfield, x - 1, y, PIPE_CONN_E, color);
            }
        }
    }

    // If we get here, who knows why, but we don't have a connection.
    return 0;
}

void playfield_fill_light(playfield_t *playfield, int x, int y, int in_direction, int color)
{
    // First, if this doesn't have a connection in the in direction, don't fill it.
    playfield_entry_t *cur = playfield_entry(playfield, x, y);
    if ((cur->pipe & in_direction) == 0)
    {
        return;
    }

    // Calculate the other direction of the pipe by removing the in direction.
    unsigned int out_direction = cur->pipe & (~in_direction);
    cur->color = color;
    switch(out_direction)
    {
        case PIPE_CONN_N:
        {
            // Goes out north. Either it hits the sky or it goes to another block.
            if (y > 0)
            {
                return playfield_fill_light(playfield, x, y - 1, PIPE_CONN_S, color);
            }
            break;
        }
        case PIPE_CONN_S:
        {
            // Goes out south. Either it hits a light block or it goes to another block.
            if (y < playfield->height - 1)
            {
                return playfield_fill_light(playfield, x, y + 1, PIPE_CONN_N, color);
            }
            break;
        }
        case PIPE_CONN_E:
        {
            // Goes out east. Either it hits a light block or it goes to another block.
            if (x < playfield->width - 1)
            {
                return playfield_fill_light(playfield, x + 1, y, PIPE_CONN_W, color);
            }
            break;
        }
        case PIPE_CONN_W:
        {
            // Goes out west. Either it hits a light block or it goes to another block.
            if (x > 0)
            {
                return playfield_fill_light(playfield, x - 1, y, PIPE_CONN_E, color);
            }
            break;
        }
    }
}

void playfield_check_connections(playfield_t *playfield)
{
    // Keep track of what changed so we can reset countdowns.
    playfield_entry_t *oldentries = malloc(sizeof(playfield_entry_t) * playfield->width * playfield->height);
    memcpy(oldentries, playfield->entries, sizeof(playfield_entry_t) * playfield->width * playfield->height);

    for (int y = 0; y < playfield->height; y++)
    {
        for (int x = 0; x < playfield->width; x++)
        {
            // Turn off all connections and then recalculate.
            playfield_entry(playfield, x, y)->color = SOURCE_COLOR_NONE;
        }
    }

    // Now, go through each light source and see if it connects to another of its color.
    for (int lsy = 0; lsy < playfield->height; lsy++)
    {
        source_entry_t *source = playfield->sources + lsy;
        if (source->color != SOURCE_COLOR_NONE)
        {
            if (playfield_touches_light(playfield, 0, lsy, PIPE_CONN_W, source->color))
            {
                playfield_fill_light(playfield, 0, lsy, PIPE_CONN_W, source->color);
            }
        }

        source = playfield->sources + lsy + playfield->height;
        if (source->color != SOURCE_COLOR_NONE)
        {
            if (playfield_touches_light(playfield, playfield->width - 1, lsy, PIPE_CONN_E, source->color))
            {
                playfield_fill_light(playfield, playfield->width - 1, lsy, PIPE_CONN_E, source->color);
            }
        }
    }

    for (int lsx = 0; lsx < playfield->width; lsx++)
    {
        source_entry_t *source = playfield->sources + (2 * playfield->height) + lsx;
        if (source->color != SOURCE_COLOR_NONE)
        {
            if (playfield_touches_light(playfield, lsx, playfield->height - 1, PIPE_CONN_S, source->color))
            {
                playfield_fill_light(playfield, lsx, playfield->height - 1, PIPE_CONN_S, source->color);
            }
        }

        source = playfield->sources + (2 * playfield->height) + playfield->width + lsx;
        if (source->color != SOURCE_COLOR_NONE)
        {
            if (playfield_touches_light(playfield, lsx, 0, PIPE_CONN_N, source->color))
            {
                playfield_fill_light(playfield, lsx, 0, PIPE_CONN_N, source->color);
            }
        }
    }

    // Now, for anything that changed, reset its age.
    for (int y = 0; y < playfield->height; y++)
    {
        for (int x = 0; x < playfield->width; x++)
        {
            // Turn off all connections and then recalculate.
            playfield_entry_t *cur = playfield_entry(playfield, x, y);
            playfield_entry_t *old = oldentries + (y * playfield->width) + x;

            if (cur->color != old->color)
            {
                cur->age = 0;
            }
        }
    }

    // Now that we don't need the old entries, free them.
    free(oldentries);
}

#define CURSOR_ROTATE_LEFT 11
#define CURSOR_ROTATE_RIGHT 12

void playfield_cursor_rotate(playfield_t *playfield, int direction)
{
    switch(direction)
    {
        case CURSOR_ROTATE_LEFT:
        {
            playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);

            if (cur->block != BLOCK_TYPE_NONE)
            {
                unsigned int new_rotation = 0;
                new_rotation |= (cur->pipe & PIPE_CONN_N) ? PIPE_CONN_W : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_E) ? PIPE_CONN_N : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_S) ? PIPE_CONN_E : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_W) ? PIPE_CONN_S : 0;
                cur->pipe = new_rotation;
            }

            break;
        }
        case CURSOR_ROTATE_RIGHT:
        {
            playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);

            if (cur->block != BLOCK_TYPE_NONE)
            {
                unsigned int new_rotation = 0;
                new_rotation |= (cur->pipe & PIPE_CONN_N) ? PIPE_CONN_E : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_E) ? PIPE_CONN_S : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_S) ? PIPE_CONN_W : 0;
                new_rotation |= (cur->pipe & PIPE_CONN_W) ? PIPE_CONN_N : 0;
                cur->pipe = new_rotation;
            }

            break;
        }
    }

    playfield_check_connections(playfield);
}

void playfield_apply_gravity(playfield_t *playfield)
{
    // Don't need to check the top row, nothing could fall onto it.
    for (int y = playfield->height - 1; y > 0; y--)
    {
        for (int x = 0; x < playfield->width; x++)
        {
            // Only need to drop blocks into this spot if it is empty.
            playfield_entry_t *cur = playfield_entry(playfield, x, y);
            if (cur->block == BLOCK_TYPE_NONE)
            {
                // Look for a potential block to drop into this slot.
                for (int py = y - 1; py >= 0; py--)
                {
                    playfield_entry_t *potential = playfield_entry(playfield, x, py);
                    if (potential->block != BLOCK_TYPE_NONE)
                    {
                        // Drop this block in.
                        playfield_entry_t temp;
                        memcpy(&temp, potential, sizeof(playfield_entry_t));
                        memcpy(potential, cur, sizeof(playfield_entry_t));
                        memcpy(cur, &temp, sizeof(playfield_entry_t));
                        break;
                    }
                }
            }
        }
    }

    playfield_check_connections(playfield);
}

#define MAX_AGE 30

void playfield_age(playfield_t *playfield)
{
    for (int y = 0; y < playfield->height; y++)
    {
        for (int x = 0; x < playfield->width; x++)
        {
            // Kill any connections with light active that are older than
            // some age.
            playfield_entry_t *cur = playfield_entry(playfield, x, y);
            if (cur->block != BLOCK_TYPE_NONE && cur->color != SOURCE_COLOR_NONE)
            {
                if (cur->age > MAX_AGE)
                {
                    memset(cur, 0, sizeof(playfield_entry_t));
                }
                else
                {
                    cur->age ++;
                }
            }
        }
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }
}

#define CURSOR_MOVE_UP 1
#define CURSOR_MOVE_DOWN 2
#define CURSOR_MOVE_LEFT 3
#define CURSOR_MOVE_RIGHT 4

void playfield_cursor_move(playfield_t *playfield, int direction)
{
    switch(direction)
    {
        case CURSOR_MOVE_UP:
        {
            if (playfield->cury > 0)
            {
                playfield->cury--;
            }
            break;
        }
        case CURSOR_MOVE_DOWN:
        {
            if (playfield->cury < (playfield->height - 1))
            {
                playfield->cury++;
            }
            break;
        }
        case CURSOR_MOVE_LEFT:
        {
            if (playfield->curx > 0)
            {
                playfield->curx--;
            }
            break;
        }
        case CURSOR_MOVE_RIGHT:
        {
            if (playfield->curx < (playfield->width - 1))
            {
                playfield->curx++;
            }
            break;
        }
    }
}

void playfield_cursor_drag(playfield_t *playfield, int direction)
{
    switch(direction)
    {
        case CURSOR_MOVE_UP:
        {
            if (playfield->cury > 0)
            {
                playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
                playfield_entry_t *swap = playfield_entry(playfield, playfield->curx, playfield->cury - 1);

                if (cur->block != BLOCK_TYPE_NONE && swap->block != BLOCK_TYPE_NONE)
                {
                    playfield_entry_t temp;
                    memcpy(&temp, cur, sizeof(playfield_entry_t));
                    memcpy(cur, swap, sizeof(playfield_entry_t));
                    memcpy(swap, &temp, sizeof(playfield_entry_t));
                    playfield->cury--;
                }
            }
            break;
        }
        case CURSOR_MOVE_DOWN:
        {
            if (playfield->cury < (playfield->height - 1))
            {
                playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
                playfield_entry_t *swap = playfield_entry(playfield, playfield->curx, playfield->cury + 1);

                if (cur->block != BLOCK_TYPE_NONE && swap->block != BLOCK_TYPE_NONE)
                {
                    playfield_entry_t temp;
                    memcpy(&temp, cur, sizeof(playfield_entry_t));
                    memcpy(cur, swap, sizeof(playfield_entry_t));
                    memcpy(swap, &temp, sizeof(playfield_entry_t));
                    playfield->cury++;
                }
            }
            break;
        }
        case CURSOR_MOVE_LEFT:
        {
            if (playfield->curx > 0)
            {
                playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
                playfield_entry_t *swap = playfield_entry(playfield, playfield->curx - 1, playfield->cury);

                if (gamerule_gravity)
                {
                    // We allow bumping down for horizontal movements.
                    if (cur->block != BLOCK_TYPE_NONE)
                    {
                        int simplemove = swap->block != BLOCK_TYPE_NONE;

                        if (simplemove)
                        {
                            playfield->curx--;
                        }
                        else
                        {
                            playfield->curx--;
                            while(playfield_entry(playfield, playfield->curx, playfield->cury + 1)->block == BLOCK_TYPE_NONE)
                            {
                                playfield->cury++;
                            }
                        }

                        playfield_entry_t temp;
                        memcpy(&temp, cur, sizeof(playfield_entry_t));
                        memcpy(cur, swap, sizeof(playfield_entry_t));
                        memcpy(swap, &temp, sizeof(playfield_entry_t));
                    }
                }
                else
                {
                    if (cur->block != BLOCK_TYPE_NONE && swap->block != BLOCK_TYPE_NONE)
                    {
                        playfield_entry_t temp;
                        memcpy(&temp, cur, sizeof(playfield_entry_t));
                        memcpy(cur, swap, sizeof(playfield_entry_t));
                        memcpy(swap, &temp, sizeof(playfield_entry_t));
                        playfield->cury++;
                    }
                }
            }
            break;
        }
        case CURSOR_MOVE_RIGHT:
        {
            if (playfield->curx < (playfield->width - 1))
            {
                playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
                playfield_entry_t *swap = playfield_entry(playfield, playfield->curx + 1, playfield->cury);

                if (gamerule_gravity)
                {
                    // We allow bumping down for horizontal movements.
                    if (cur->block != BLOCK_TYPE_NONE)
                    {
                        int simplemove = swap->block != BLOCK_TYPE_NONE;

                        if (simplemove)
                        {
                            playfield->curx++;
                        }
                        else
                        {
                            playfield->curx++;
                            while(playfield_entry(playfield, playfield->curx, playfield->cury + 1)->block == BLOCK_TYPE_NONE)
                            {
                                playfield->cury++;
                            }
                        }

                        playfield_entry_t temp;
                        memcpy(&temp, cur, sizeof(playfield_entry_t));
                        memcpy(cur, swap, sizeof(playfield_entry_t));
                        memcpy(swap, &temp, sizeof(playfield_entry_t));
                    }
                }
                else
                {
                    if (cur->block != BLOCK_TYPE_NONE && swap->block != BLOCK_TYPE_NONE)
                    {
                        playfield_entry_t temp;
                        memcpy(&temp, cur, sizeof(playfield_entry_t));
                        memcpy(cur, swap, sizeof(playfield_entry_t));
                        memcpy(swap, &temp, sizeof(playfield_entry_t));
                        playfield->cury++;
                    }
                }
            }
            break;
        }
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }
}

#define SWAP_DIRECTION_HORIZONTAL 21
#define SWAP_DIRECTION_VERTICAL 22

void playfield_cursor_swap(playfield_t *playfield, int direction)
{
    switch(direction)
    {
        case SWAP_DIRECTION_HORIZONTAL:
        {
            if (playfield->curx > 0 && playfield->curx < (playfield->width - 1))
            {
                playfield_entry_t *swap1 = playfield_entry(playfield, playfield->curx - 1, playfield->cury);
                playfield_entry_t *swap2 = playfield_entry(playfield, playfield->curx + 1, playfield->cury);

                if (swap1->block != BLOCK_TYPE_NONE && swap2->block != BLOCK_TYPE_NONE)
                {
                    playfield_entry_t temp;
                    memcpy(&temp, swap1, sizeof(playfield_entry_t));
                    memcpy(swap1, swap2, sizeof(playfield_entry_t));
                    memcpy(swap2, &temp, sizeof(playfield_entry_t));
                }
            }
            break;
        }
        case SWAP_DIRECTION_VERTICAL:
        {
            if (playfield->cury > 0 && playfield->cury < (playfield->height - 1))
            {
                playfield_entry_t *swap1 = playfield_entry(playfield, playfield->curx, playfield->cury + 1);
                playfield_entry_t *swap2 = playfield_entry(playfield, playfield->curx, playfield->cury - 1);

                if (swap1->block != BLOCK_TYPE_NONE && swap2->block != BLOCK_TYPE_NONE)
                {
                    playfield_entry_t temp;
                    memcpy(&temp, swap1, sizeof(playfield_entry_t));
                    memcpy(swap1, swap2, sizeof(playfield_entry_t));
                    memcpy(swap2, &temp, sizeof(playfield_entry_t));
                }
            }
            break;
        }
    }

    playfield_check_connections(playfield);
}

void playfield_cursor_drop(playfield_t *playfield)
{
    playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
    if (cur->block == BLOCK_TYPE_NONE && playfield->upnext->block != BLOCK_TYPE_NONE)
    {
        // Assign the block to the actual playfield.
        memcpy(cur, playfield->upnext, sizeof(playfield_entry_t));

        // Prepare the next upnext block.
        memmove(&playfield->upnext[0], &playfield->upnext[1], sizeof(playfield_entry_t) * (UPNEXT_AMOUNT - 1));
        memset(&playfield->upnext[UPNEXT_AMOUNT - 1], 0, sizeof(playfield_entry_t));
        playfield_generate_upnext(playfield);
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }
}

void main()
{
    // Get settings so we know how many controls to read.
    eeprom_t settings;
    eeprom_read(&settings);

    // Initialize some crappy video.
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(0, 0, 0));

    // Initialize the ROMFS.
    romfs_init_default();

    // Load sprites.
    cursor = sprite_load("rom://sprites/cursor");
    block_purple = sprite_load("rom://sprites/purpleblock");
    block_blue = sprite_load("rom://sprites/blueblock");
    block_green = sprite_load("rom://sprites/greenblock");
    block_orange = sprite_load("rom://sprites/orangeblock");
    block_gray = sprite_load("rom://sprites/grayblock");

    // Pipes
    pipe_ew = sprite_load("rom://sprites/straightpipe");
    pipe_ns = sprite_dup_rotate_cw(pipe_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_sw = sprite_load("rom://sprites/cornerpipe");
    pipe_nw = sprite_dup_rotate_cw(pipe_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_ne = sprite_dup_rotate_cw(pipe_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_se = sprite_dup_rotate_cw(pipe_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    // Pipe light colors
    red_ew = sprite_load("rom://sprites/straightred");
    red_ns = sprite_dup_rotate_cw(red_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    green_ew = sprite_load("rom://sprites/straightgreen");
    green_ns = sprite_dup_rotate_cw(green_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    blue_ew = sprite_load("rom://sprites/straightblue");
    blue_ns = sprite_dup_rotate_cw(blue_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    cyan_ew = sprite_load("rom://sprites/straightcyan");
    cyan_ns = sprite_dup_rotate_cw(cyan_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    magenta_ew = sprite_load("rom://sprites/straightmagenta");
    magenta_ns = sprite_dup_rotate_cw(magenta_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    yellow_ew = sprite_load("rom://sprites/straightyellow");
    yellow_ns = sprite_dup_rotate_cw(yellow_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    white_ew = sprite_load("rom://sprites/straightwhite");
    white_ns = sprite_dup_rotate_cw(white_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    red_sw = sprite_load("rom://sprites/cornerred");
    red_nw = sprite_dup_rotate_cw(red_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    red_ne = sprite_dup_rotate_cw(red_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    red_se = sprite_dup_rotate_cw(red_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    green_sw = sprite_load("rom://sprites/cornergreen");
    green_nw = sprite_dup_rotate_cw(green_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    green_ne = sprite_dup_rotate_cw(green_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    green_se = sprite_dup_rotate_cw(green_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    blue_sw = sprite_load("rom://sprites/cornerblue");
    blue_nw = sprite_dup_rotate_cw(blue_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    blue_ne = sprite_dup_rotate_cw(blue_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    blue_se = sprite_dup_rotate_cw(blue_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    cyan_sw = sprite_load("rom://sprites/cornercyan");
    cyan_nw = sprite_dup_rotate_cw(cyan_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    cyan_ne = sprite_dup_rotate_cw(cyan_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    cyan_se = sprite_dup_rotate_cw(cyan_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    magenta_sw = sprite_load("rom://sprites/cornermagenta");
    magenta_nw = sprite_dup_rotate_cw(magenta_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    magenta_ne = sprite_dup_rotate_cw(magenta_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    magenta_se = sprite_dup_rotate_cw(magenta_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    yellow_sw = sprite_load("rom://sprites/corneryellow");
    yellow_nw = sprite_dup_rotate_cw(yellow_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    yellow_ne = sprite_dup_rotate_cw(yellow_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    yellow_se = sprite_dup_rotate_cw(yellow_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    white_sw = sprite_load("rom://sprites/cornerwhite");
    white_nw = sprite_dup_rotate_cw(white_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    white_ne = sprite_dup_rotate_cw(white_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    white_se = sprite_dup_rotate_cw(white_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    // Sources and their associated colors
    source_e = sprite_load("rom://sprites/source");
    source_s = sprite_dup_rotate_cw(source_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_w = sprite_dup_rotate_cw(source_s, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_n = sprite_dup_rotate_cw(source_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    source_red = sprite_load("rom://sprites/red");
    source_green = sprite_load("rom://sprites/green");
    source_blue = sprite_load("rom://sprites/blue");
    source_cyan = sprite_load("rom://sprites/cyan");
    source_magenta = sprite_load("rom://sprites/magenta");
    source_yellow = sprite_load("rom://sprites/yellow");
    source_white = sprite_load("rom://sprites/white");

    red_w = sprite_load("rom://sprites/endred");
    red_n = sprite_dup_rotate_cw(red_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    red_e = sprite_dup_rotate_cw(red_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    red_s = sprite_dup_rotate_cw(red_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    green_w = sprite_load("rom://sprites/endgreen");
    green_n = sprite_dup_rotate_cw(green_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    green_e = sprite_dup_rotate_cw(green_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    green_s = sprite_dup_rotate_cw(green_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    blue_w = sprite_load("rom://sprites/endblue");
    blue_n = sprite_dup_rotate_cw(blue_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    blue_e = sprite_dup_rotate_cw(blue_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    blue_s = sprite_dup_rotate_cw(blue_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    cyan_w = sprite_load("rom://sprites/endcyan");
    cyan_n = sprite_dup_rotate_cw(cyan_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    cyan_e = sprite_dup_rotate_cw(cyan_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    cyan_s = sprite_dup_rotate_cw(cyan_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    magenta_w = sprite_load("rom://sprites/endmagenta");
    magenta_n = sprite_dup_rotate_cw(magenta_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    magenta_e = sprite_dup_rotate_cw(magenta_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    magenta_s = sprite_dup_rotate_cw(magenta_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    yellow_w = sprite_load("rom://sprites/endyellow");
    yellow_n = sprite_dup_rotate_cw(yellow_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    yellow_e = sprite_dup_rotate_cw(yellow_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    yellow_s = sprite_dup_rotate_cw(yellow_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    white_w = sprite_load("rom://sprites/endwhite");
    white_n = sprite_dup_rotate_cw(white_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    white_e = sprite_dup_rotate_cw(white_n, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    white_s = sprite_dup_rotate_cw(white_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);

    playfield_t *playfield = playfield_new(PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT);

    if (gamerule_placing)
    {
        playfield_generate_upnext(playfield);
    }
    else
    {
        for (int y = 0; y < PLAYFIELD_HEIGHT; y++)
        {
            for (int x = 0; x < PLAYFIELD_WIDTH; x++)
            {
                playfield_generate_block(playfield, x, y, 0.75);
            }
        }
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }

    playfield_set_source(playfield, -1, 1, SOURCE_COLOR_RED);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, 1, SOURCE_COLOR_RED);

    playfield_set_source(playfield, -1, 3, SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, 3, SOURCE_COLOR_GREEN);

    playfield_set_source(playfield, -1, 5, SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, 5, SOURCE_COLOR_BLUE);

    playfield_set_source(playfield, -1, 7, SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, 7, SOURCE_COLOR_GREEN);

    playfield_set_source(playfield, -1, 9, SOURCE_COLOR_RED);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, 9, SOURCE_COLOR_RED);

    playfield_set_source(playfield, 1, PLAYFIELD_HEIGHT, SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 3, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 5, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, 7, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);

    playfield_set_source(playfield, 7, -1, SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 5, -1, SOURCE_COLOR_RED | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 3, -1, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, 1, -1, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);

    // FPS calculation for debugging.
    double fps_value = 60.0;
    unsigned int draw_time = 0;

    // Cursor repeat tracking.
    int repeats[4] = { -1, -1, -1, -1 };

    // Run the game engine.
    while ( 1 )
    {
        // Get FPS measurements.
        int fps = profile_start();
        int drawprofile = profile_start();

        // Grab inputs.
        maple_poll_buttons();
        jvs_buttons_t pressed = maple_buttons_pressed();
        jvs_buttons_t held = maple_buttons_held();
        jvs_buttons_t released = maple_buttons_released();
        int dragging = 0;

        // Handle drag modifier.
        if (gamerule_dragging)
        {
            if (held.player1.button3)
            {
                dragging = 1;

                if (pressed.player1.up)
                {
                    playfield_cursor_drag(playfield, CURSOR_MOVE_UP);
                }
                if (pressed.player1.down)
                {
                    playfield_cursor_drag(playfield, CURSOR_MOVE_DOWN);
                }
                if (pressed.player1.left)
                {
                    playfield_cursor_drag(playfield, CURSOR_MOVE_LEFT);
                }
                if (pressed.player1.right)
                {
                    playfield_cursor_drag(playfield, CURSOR_MOVE_RIGHT);
                }
            }
            else if(released.player1.button3)
            {
                // Let go of a drag, lets reset repeats.
                for (int i = 0; i < 4; i++)
                {
                    repeats[i] = -1;
                }
            }
        }

        // Handle normal cursor movement.
        if (!dragging)
        {
            if (pressed.player1.up)
            {
                repeat_init(pressed.player1.up, &repeats[0]);
                playfield_cursor_move(playfield, CURSOR_MOVE_UP);
            }
            else if (repeat(held.player1.up, &repeats[0]))
            {
                playfield_cursor_move(playfield, CURSOR_MOVE_UP);
            }
            if (pressed.player1.down)
            {
                repeat_init(pressed.player1.down, &repeats[1]);
                playfield_cursor_move(playfield, CURSOR_MOVE_DOWN);
            }
            else if (repeat(held.player1.down, &repeats[1]))
            {
                playfield_cursor_move(playfield, CURSOR_MOVE_DOWN);
            }
            if (pressed.player1.left)
            {
                repeat_init(pressed.player1.left, &repeats[2]);
                playfield_cursor_move(playfield, CURSOR_MOVE_LEFT);
            }
            else if (repeat(held.player1.left, &repeats[2]))
            {
                playfield_cursor_move(playfield, CURSOR_MOVE_LEFT);
            }
            if (pressed.player1.right)
            {
                repeat_init(pressed.player1.right, &repeats[3]);
                playfield_cursor_move(playfield, CURSOR_MOVE_RIGHT);
            }
            else if (repeat(held.player1.right, &repeats[3]))
            {
                playfield_cursor_move(playfield, CURSOR_MOVE_RIGHT);
            }

            if (gamerule_rotation)
            {
                if (pressed.player1.button1)
                {
                    playfield_cursor_rotate(playfield, CURSOR_ROTATE_LEFT);
                }
                if (pressed.player1.button2)
                {
                    playfield_cursor_rotate(playfield, CURSOR_ROTATE_RIGHT);
                }
            }
            else if (gamerule_dragging)
            {
                if (pressed.player1.button1)
                {
                    playfield_cursor_swap(playfield, SWAP_DIRECTION_HORIZONTAL);
                }
                if (pressed.player1.button2)
                {
                    playfield_cursor_swap(playfield, SWAP_DIRECTION_VERTICAL);
                }
            }
            else if (gamerule_placing)
            {
                if (pressed.player1.button1)
                {
                    playfield_cursor_drop(playfield);
                }
            }
        }

        // Age the playfield so we can get rid of any beams that have stuck
        // around too long.
        playfield_age(playfield);

        // Draw the playfield
        int width;
        int height;
        playfield_metrics(playfield, &width, &height);
        playfield_draw((video_width() - width) / 2, 24, playfield);

        // Draw debugging
        video_draw_debug_text(
            (video_width() / 2) - (18 * 4),
            video_height() - 32,
            rgb(0, 200, 255),
            "FPS: %.01f, %dx%d\n  us frame: %u",
            fps_value, video_width(), video_height(),
            draw_time
        );

        // Calculate draw time
        draw_time = profile_end(drawprofile);

        // Wait for vblank and draw it!
        video_display_on_vblank();

        // Calcualte instantaneous FPS, adjust animation counters.
        uint32_t uspf = profile_end(fps);
        fps_value = (1000000.0 / (double)uspf) + 0.01;
    }
}

void test()
{
    video_init(VIDEO_COLOR_1555);

    while ( 1 )
    {
        video_fill_screen(rgb(48, 48, 48));
        video_draw_debug_text(320 - 56, 236, rgb(255, 255, 255), "test mode stub");
        video_display_on_vblank();
    }
}
