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

typedef struct
{
    int width;
    int height;
    int curx;
    int cury;
    playfield_entry_t *entries;
    source_entry_t *sources;
} playfield_t;

#define BLOCK_TYPE_NONE 0
#define BLOCK_TYPE_PURPLE 1

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

#define PLAYFIELD_BORDER 2

void playfield_metrics(playfield_t *playfield, int *width, int *height)
{
    *width = (playfield->width + 2) * BLOCK_WIDTH;
    *height = ((playfield->height + 1) * BLOCK_HEIGHT) + PLAYFIELD_BORDER;
}

playfield_entry_t *playfield_entry(playfield_t *playfield, int x, int y)
{
    return playfield->entries + (y * playfield->width) + x;
}

void playfield_draw(int x, int y, playfield_t *playfield)
{
    video_draw_box(
        x + BLOCK_WIDTH - PLAYFIELD_BORDER,
        y,
        x + (BLOCK_WIDTH * (playfield->width + 1)) + (PLAYFIELD_BORDER - 1),
        y + (BLOCK_HEIGHT * playfield->height) + ((PLAYFIELD_BORDER * 2) - 1),
        rgb(255, 255, 255)
    );
    video_draw_box(
        x + BLOCK_WIDTH - PLAYFIELD_BORDER - 1,
        y - 1,
        x + (BLOCK_WIDTH * (playfield->width + 1)) + PLAYFIELD_BORDER,
        y + (BLOCK_HEIGHT * playfield->height) + (PLAYFIELD_BORDER * 2),
        rgb(255, 255, 255)
    );

    for (int pheight = -1; pheight <= playfield->height; pheight++)
    {
        for (int pwidth = -1; pwidth <= playfield->width; pwidth++)
        {
            int xloc = x + ((pwidth + 1) * BLOCK_WIDTH);
            int yloc = y + (pheight * BLOCK_HEIGHT) + PLAYFIELD_BORDER;

            // First, draw the blocks on the playfield.
            if (pheight >= 0 && pheight < playfield->height && pwidth >= 0 && pwidth < playfield->width)
            {
                playfield_entry_t *cur = playfield_entry(playfield, pwidth, pheight);
                void *blocksprite = 0;

                switch(cur->block)
                {
                    case BLOCK_TYPE_NONE:
                    {
                        break;
                    }
                    case BLOCK_TYPE_PURPLE:
                    {
                        blocksprite = block_purple;
                        break;
                    }
                }

                void *pipesprite = 0;
                void *colorsprite = 0;
                switch(cur->pipe)
                {
                    case PIPE_CONN_E | PIPE_CONN_W:
                    {
                        pipesprite = pipe_ew;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_ew;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_ew;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_ew;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_ew;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_ew;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_ew;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_ew;
                        }
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_S:
                    {
                        pipesprite = pipe_ns;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_ns;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_ns;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_ns;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_ns;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_ns;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_ns;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_ns;
                        }
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_E:
                    {
                        pipesprite = pipe_ne;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_ne;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_ne;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_ne;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_ne;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_ne;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_ne;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_ne;
                        }
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_W:
                    {
                        pipesprite = pipe_nw;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_nw;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_nw;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_nw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_nw;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_nw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_nw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_nw;
                        }
                        break;
                    }
                    case PIPE_CONN_S | PIPE_CONN_E:
                    {
                        pipesprite = pipe_se;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_se;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_se;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_se;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_se;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_se;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_se;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_se;
                        }
                        break;
                    }
                    case PIPE_CONN_S | PIPE_CONN_W:
                    {
                        pipesprite = pipe_sw;
                        if (cur->color == SOURCE_COLOR_RED)
                        {
                            colorsprite = red_sw;
                        }
                        if (cur->color == SOURCE_COLOR_GREEN)
                        {
                            colorsprite = green_sw;
                        }
                        if (cur->color == SOURCE_COLOR_BLUE)
                        {
                            colorsprite = blue_sw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = magenta_sw;
                        }
                        if (cur->color == (SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = cyan_sw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN))
                        {
                            colorsprite = yellow_sw;
                        }
                        if (cur->color == (SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE))
                        {
                            colorsprite = white_sw;
                        }
                        break;
                    }
                }

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
            if (pwidth == -1)
            {
                if (pheight >= 0 && pheight < playfield->height)
                {
                    source = playfield->sources + pheight;
                    sourcesprite = source_e;
                }
            }
            else if (pwidth == playfield->width)
            {
                if (pheight >= 0 && pheight < playfield->height)
                {
                    source = playfield->sources + playfield->height + pheight;
                    sourcesprite = source_w;
                }
            }
            else if (pheight == playfield->height)
            {
                if (pwidth >= 0 && pwidth < playfield->width)
                {
                    source = playfield->sources + (2 * playfield->height) + pwidth;
                    sourcesprite = source_n;
                }
            }

            if (source != 0)
            {
                if (source->color != SOURCE_COLOR_NONE)
                {
                    video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, sourcesprite);
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

    source_entry_t *sources = malloc(sizeof(source_entry_t) * (width + (height * 2)));
    memset(sources, 0, sizeof(source_entry_t) * (width + (height * 2)));

    playfield_t *playfield = malloc(sizeof(playfield_t));
    playfield->width = width;
    playfield->height = height;
    playfield->entries = entries;
    playfield->sources = sources;

    playfield->curx = width / 2;
    playfield->cury = height - 1;

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
        int corner = (int)(chance() * 4.0);
        int second = (int)(chance() * 3.0);

        playfield_entry_t *cur = playfield_entry(playfield, x, y);
        cur->block = BLOCK_TYPE_PURPLE;
        cur->pipe = bits[corner] | bits[(corner + (second > 0 ? 2 : 1)) % 4];
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
                return 0;
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
                        cur->block = potential->block;
                        cur->pipe = potential->pipe;

                        potential->block = BLOCK_TYPE_NONE;
                        potential->pipe = PIPE_CONN_NONE;
                        break;
                    }
                }
            }
        }
    }

    playfield_check_connections(playfield);
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
            break;
        }
        case CURSOR_MOVE_RIGHT:
        {
            if (playfield->curx < (playfield->width - 1))
            {
                playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
                playfield_entry_t *swap = playfield_entry(playfield, playfield->curx + 1, playfield->cury);

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
            break;
        }
    }

    playfield_apply_gravity(playfield);
}

#define PLAYFIELD_WIDTH 9
#define PLAYFIELD_HEIGHT 12

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

    playfield_t *playfield = playfield_new(PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT);
    for (int y = 0; y < PLAYFIELD_HEIGHT; y++)
    {
        for (int x = 0; x < PLAYFIELD_WIDTH; x++)
        {
            playfield_generate_block(playfield, x, y, 0.75);
        }
    }
    playfield_apply_gravity(playfield);

    playfield_set_source(playfield, -1, PLAYFIELD_HEIGHT - 1, SOURCE_COLOR_RED);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT - 1, SOURCE_COLOR_RED);

    playfield_set_source(playfield, -1, PLAYFIELD_HEIGHT - 3, SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT - 3, SOURCE_COLOR_GREEN);

    playfield_set_source(playfield, -1, PLAYFIELD_HEIGHT - 5, SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT - 5, SOURCE_COLOR_BLUE);

    playfield_set_source(playfield, 1, PLAYFIELD_HEIGHT, SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 3, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_BLUE);
    playfield_set_source(playfield, 5, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, 7, PLAYFIELD_HEIGHT, SOURCE_COLOR_RED | SOURCE_COLOR_GREEN | SOURCE_COLOR_BLUE);

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
            if (pressed.player1.button1)
            {
                playfield_cursor_rotate(playfield, CURSOR_ROTATE_LEFT);
            }
            if (pressed.player1.button2)
            {
                playfield_cursor_rotate(playfield, CURSOR_ROTATE_RIGHT);
            }
        }

        // Draw the playfield
        int width;
        int height;
        playfield_metrics(playfield, &width, &height);
        playfield_draw((video_width() - width) / 2, 32, playfield);

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
