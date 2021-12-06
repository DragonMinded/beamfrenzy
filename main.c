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
#include <naomi/rtc.h>
#include <naomi/timer.h>
#include <naomi/system.h>
#include <xmp.h>

#define BUFSIZE 8192
#define SAMPLERATE 44100

typedef struct
{
    char filename[1024];
    volatile int exit;
    volatile int error;
    uint32_t thread;
} audiothread_instructions_t;

void *audiothread_main(void *param)
{
    audiothread_instructions_t *instructions = (audiothread_instructions_t *)param;

    xmp_context ctx = xmp_create_context();

    if (xmp_load_module(ctx, instructions->filename) < 0)
    {
        instructions->error = 1;
        return 0;
    }
    else if (xmp_start_player(ctx, SAMPLERATE, 0) != 0)
    {
        instructions->error = 2;
        xmp_release_module(ctx);
    }
    else
    {
        audio_register_ringbuffer(AUDIO_FORMAT_16BIT, SAMPLERATE, BUFSIZE);

        while (xmp_play_frame(ctx) == 0 && instructions->exit == 0)
        {
            struct xmp_frame_info fi;
            xmp_get_frame_info(ctx, &fi);

            unsigned int numsamples = fi.buffer_size / 4;
            uint32_t *samples = (uint32_t *)fi.buffer;

            while (numsamples > 0)
            {
                unsigned int actual_written = audio_write_stereo_data(samples, numsamples);
                if (actual_written < numsamples)
                {
                    numsamples -= actual_written;
                    samples += actual_written;

                    // Sleep for the time it takes to play half our buffer so we can wake up and
                    // fill it again.
                    thread_sleep((int)(1000000.0 * (((float)BUFSIZE / 4.0) / (float)SAMPLERATE)));
                }
                else
                {
                    numsamples = 0;
                }
            }
        }

        audio_unregister_ringbuffer();

        xmp_end_player(ctx);
        xmp_release_module(ctx);
    }

    xmp_free_context(ctx);
    return 0;
}

audiothread_instructions_t * music_play(char *filename)
{
    audiothread_instructions_t *inst = malloc(sizeof(audiothread_instructions_t));
    memset(inst, 0, sizeof(audiothread_instructions_t));
    strcpy(inst->filename, filename);

    inst->thread = thread_create("audio", &audiothread_main, inst);
    thread_priority(inst->thread, 1);
    thread_start(inst->thread);
    return inst;
}

void music_stop(audiothread_instructions_t *inst)
{
    inst->exit = 1;
    thread_join(inst->thread);
    free(inst);
}

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

void *asset_load(const char * const path, unsigned int *length)
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

            if (length)
            {
                *length = size;
            }
            return data;
        }
        else
        {
            if (length)
            {
                *length = 0;
            }
            return 0;
        }
    }
    else
    {
        return 0;
    }
}

void *sprite_load(const char * const path)
{
    return asset_load(path, 0);
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
#define PLACE_TIME 5.0

int gamerule_gravity = 0;
int gamerule_rotation = 0;
int gamerule_dragging = 0;
int gamerule_placing = 1;
int gamerule_placetimer = 1;

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
#define SOURCE_COLOR_IMPOSSIBLE 0x8

#define UPNEXT_AMOUNT 5

typedef struct
{
    int width;
    int height;
    int curx;
    int cury;
    int score;
    int running;
    int vertical;
    float timeleft;
    playfield_entry_t *entries;
    source_entry_t *sources;
    playfield_entry_t *upnext;
    audiothread_instructions_t *instructions;
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
void *impossible = 0;

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

int activate_sound = -1;
int bad_sound = -1;
int clear_sound = -1;
int drop_sound = -1;
int scroll_sound = -1;

#define PLAYFIELD_BORDER 2

void playfield_metrics(playfield_t *playfield, int *width, int *height)
{
    if (playfield->vertical)
    {
        *width = (playfield->width + 2) * BLOCK_WIDTH;
        *height = (playfield->height + 2) * BLOCK_HEIGHT;

        if (gamerule_placing)
        {
            *height += BLOCK_WIDTH * 3;
        }
    }
    else
    {
        *width = (playfield->width + 2) * BLOCK_WIDTH;
        *height = (playfield->height + 2) * BLOCK_HEIGHT;

        if (gamerule_placing)
        {
            *width += BLOCK_WIDTH * 3;
        }
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
    if (cur->color == SOURCE_COLOR_IMPOSSIBLE)
    {
        return impossible;
    }

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

int playfield_game_over(playfield_t *playfield)
{
    for (int y = 0; y < PLAYFIELD_HEIGHT; y++)
    {
        for (int x = 0; x < PLAYFIELD_WIDTH; x++)
        {
            playfield_entry_t *cur = playfield_entry(playfield, x, y);
            if (cur->block == BLOCK_TYPE_NONE)
            {
                return 0;
            }
        }
    }

    return 1;
}

void playfield_draw(int x, int y, playfield_t *playfield)
{
    int xoff = 0;
    int yoff = 0;

    if (playfield->vertical)
    {
        if (gamerule_placing)
        {
            yoff += BLOCK_HEIGHT * 2;

            video_draw_box(
                x + BLOCK_WIDTH - PLAYFIELD_BORDER,
                y,
                x + (BLOCK_WIDTH * (1 + UPNEXT_AMOUNT)) + (PLAYFIELD_BORDER - 1),
                y + (BLOCK_HEIGHT) + (PLAYFIELD_BORDER + 2),
                rgb(255, 255, 255)
            );
            video_draw_box(
                x + BLOCK_WIDTH - PLAYFIELD_BORDER - 1,
                y + 1,
                x + (BLOCK_WIDTH * (1 + UPNEXT_AMOUNT)) + (PLAYFIELD_BORDER),
                y + (BLOCK_HEIGHT) + (PLAYFIELD_BORDER + 3),
                rgb(255, 255, 255)
            );

            for (int i = 0; i < UPNEXT_AMOUNT; i++)
            {
                playfield_entry_t *cur = &playfield->upnext[i];
                void *blocksprite = playfield_block_sprite(cur);
                void *pipesprite = playfield_pipe_sprite(cur);
                int xloc = x + (BLOCK_WIDTH * (i + 1));
                int yloc = y + PLAYFIELD_BORDER + 1;

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

            if (playfield->running && gamerule_placetimer)
            {
                int left = ((int)playfield->timeleft) + 1;
                if (left > 5)
                {
                    left = 5;
                }
                if (left < 0)
                {
                    left = 0;
                }

                video_draw_debug_text(
                    x + 12, y + 12,
                    rgb(255, 255, 255),
                    "%d", left
                );
            }

            char message[128];
            memset(message, 0, 128);
            if (playfield_game_over(playfield))
            {
                strcpy(message, "Game over!");
            }
            else if (!playfield->running)
            {
                strcpy(message, "Press start!");
            }

            // Draw score and such.
            video_draw_debug_text(
                x + xoff + BLOCK_WIDTH, y + yoff + (BLOCK_HEIGHT * (playfield->height + 2)) + 12,
                rgb(255, 255, 255),
                "Score: %d\n\n%s",
                playfield->score,
                message
            );
        }
    }
    else
    {
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

            if (playfield->running && gamerule_placetimer)
            {
                int left = ((int)playfield->timeleft) + 1;
                if (left > 5)
                {
                    left = 5;
                }
                if (left < 0)
                {
                    left = 0;
                }

                video_draw_debug_text(
                    x + (BLOCK_WIDTH * (playfield->width + 3)) + 12, y + BLOCK_HEIGHT + 12,
                    rgb(255, 255, 255),
                    "%d", left
                );
            }

            char message[128];
            memset(message, 0, 128);
            if (playfield_game_over(playfield))
            {
                strcpy(message, "Game over!");
            }
            else if (!playfield->running)
            {
                strcpy(message, "Press start!");
            }

            // Draw score and such.
            video_draw_debug_text(
                x + (BLOCK_WIDTH * (playfield->width + 2)) + 12, y + (BLOCK_HEIGHT * (playfield->height)),
                rgb(255, 255, 255),
                "Score: %d\n\n%s",
                playfield->score,
                message
            );
        }
    }

    video_draw_box(
        x + xoff + BLOCK_WIDTH - PLAYFIELD_BORDER,
        y + yoff + BLOCK_HEIGHT - PLAYFIELD_BORDER,
        x + xoff + (BLOCK_WIDTH * (playfield->width + 1)) + (PLAYFIELD_BORDER - 1),
        y + yoff + (BLOCK_HEIGHT * (playfield->height + 1)) + (PLAYFIELD_BORDER - 1),
        rgb(255, 255, 255)
    );
    video_draw_box(
        x + xoff + BLOCK_WIDTH - PLAYFIELD_BORDER - 1,
        y + yoff + BLOCK_HEIGHT - PLAYFIELD_BORDER - 1,
        x + xoff + (BLOCK_WIDTH * (playfield->width + 1)) + PLAYFIELD_BORDER,
        y + yoff + (BLOCK_HEIGHT * (playfield->height + 1)) + PLAYFIELD_BORDER,
        rgb(255, 255, 255)
    );

    for (int pheight = -1; pheight <= playfield->height; pheight++)
    {
        for (int pwidth = -1; pwidth <= playfield->width; pwidth++)
        {
            int xloc = x + xoff + ((pwidth + 1) * BLOCK_WIDTH);
            int yloc = y + yoff + ((pheight + 1) * BLOCK_HEIGHT);

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
            if (playfield->running && pwidth == playfield->curx && pheight == playfield->cury)
            {
                video_draw_sprite(xloc + CURSOR_OFFSET_X, yloc + CURSOR_OFFSET_Y, CURSOR_WIDTH, CURSOR_HEIGHT, cursor);
            }
        }
    }
}

playfield_t *playfield_new(int vertical, int width, int height)
{
    playfield_entry_t *entries = malloc(sizeof(playfield_entry_t) * width * height);
    memset(entries, 0, sizeof(playfield_entry_t) * width * height);

    source_entry_t *sources = malloc(sizeof(source_entry_t) * ((width * 2) + (height * 2)));
    memset(sources, 0, sizeof(source_entry_t) * ((width * 2) + (height * 2)));

    playfield_entry_t *upnext = malloc(sizeof(playfield_entry_t) * UPNEXT_AMOUNT);
    memset(upnext, 0, sizeof(playfield_entry_t) * UPNEXT_AMOUNT);

    playfield_t *playfield = malloc(sizeof(playfield_t));
    memset(playfield, 0, sizeof(playfield_t));
    playfield->width = width;
    playfield->height = height;
    playfield->vertical = vertical;
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
    static int chance_add = 0;

    if (chance() <= block_chance)
    {
        // First handle the color chance (asthetic only).
        playfield_entry_t *cur = playfield_entry(playfield, x, y);
        int color = (int)(chance() * 4.0) + 1;
        cur->block = color;

        // Now handle the connections.
        int corner = (int)(chance() * 4.0) + chance_add;
        int second = (int)(chance() * 3.0);
        cur->pipe = bits[corner % 4] | bits[(corner + (second > 0 ? 2 : 1)) % 4];
        chance_add++;
    }
}

void playfield_generate_upnext(playfield_t *playfield)
{
    static unsigned int bits[4] = { PIPE_CONN_N, PIPE_CONN_E, PIPE_CONN_S, PIPE_CONN_W };
    static int chance_add = 0;

    for (int i = 0; i < UPNEXT_AMOUNT; i++)
    {
        playfield_entry_t *cur = playfield->upnext + i;

        if (cur->block == BLOCK_TYPE_NONE)
        {
            int color = (int)(chance() * 4.0) + 1;
            cur->block = color;

            int corner = (int)(chance() * 4.0) + chance_add;
            int second = (int)(chance() * 2.0);

            cur->pipe = bits[corner % 4] | bits[(corner + (second > 0 ? 2 : 1)) % 4];
            chance_add++;
        }
    }

    if (gamerule_placetimer)
    {
        playfield->timeleft = PLACE_TIME;
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
            // Goes out north. Either it hits a light block or it goes to another block.
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
            // Goes out north. Either it hits a light block or it goes to another block.
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

int playfield_possible_color(playfield_t *playfield, int x, int y, char *visited, int in_direction)
{
    // First, if this doesn't have a connection in the in direction, its always no color.
    playfield_entry_t *cur = playfield_entry(playfield, x, y);
    if (visited[x + (y * playfield->width)])
    {
        // We already visited this, there's a loop or we point inward at ourselves
        // in a way that's impossible to recover from.
        return SOURCE_COLOR_IMPOSSIBLE;
    }
    if (cur->block == BLOCK_TYPE_NONE)
    {
        // No block here, so its possible to place another block change this pipe
        // to any color.
        return SOURCE_COLOR_NONE;
    }
    if (in_direction != 0)
    {
        if ((cur->pipe & in_direction) == 0)
        {
            // Block here, but it doesn't connect, so it could possibly be cleared.
            // We should pretend that this is a no-color.
            return SOURCE_COLOR_NONE;
        }
    }

    // Mark that we visited this block.
    visited[x + (y * playfield->width)] = 1;

    // Calculate the other directions of the pipe by removing the in direction.
    unsigned int out_directions = cur->pipe & (~in_direction);
    unsigned int source_color = SOURCE_COLOR_NONE;
    for (int i = 0; i < 4; i++)
    {
        // Calculate what direction we need to examine.
        unsigned int out_direction = out_directions & (1 << i);
        if (out_direction == 0)
        {
            continue;
        }

        // Calculate the color in that direction.
        unsigned int direction_color = SOURCE_COLOR_IMPOSSIBLE;
        switch(out_direction)
        {
            case PIPE_CONN_N:
            {
                // Goes out north. Either it hits a light block or it goes to another block.
                if (y == 0)
                {
                    source_entry_t *source = playfield->sources + (2 * playfield->height) + playfield->width + x;
                    direction_color = source->color ? source->color : SOURCE_COLOR_IMPOSSIBLE;
                }
                else
                {
                    direction_color = playfield_possible_color(playfield, x, y - 1, visited, PIPE_CONN_S);
                }
                break;
            }
            case PIPE_CONN_S:
            {
                // Goes out south. Either it hits a light block or it goes to another block.
                if (y == playfield->height - 1)
                {
                    source_entry_t *source = playfield->sources + (2 * playfield->height) + x;
                    direction_color = source->color ? source->color : SOURCE_COLOR_IMPOSSIBLE;
                }
                else
                {
                    direction_color = playfield_possible_color(playfield, x, y + 1, visited, PIPE_CONN_N);
                }
                break;
            }
            case PIPE_CONN_E:
            {
                // Goes out east. Either it hits a light block or it goes to another block.
                if (x == playfield->width - 1)
                {
                    source_entry_t *source = playfield->sources + playfield->height + y;
                    direction_color = source->color ? source->color : SOURCE_COLOR_IMPOSSIBLE;
                }
                else
                {
                    direction_color = playfield_possible_color(playfield, x + 1, y, visited, PIPE_CONN_W);
                }
                break;
            }
            case PIPE_CONN_W:
            {
                // Goes out west. Either it hits a light block or it goes to another block.
                if (x == 0)
                {
                    source_entry_t *source = playfield->sources + y;
                    direction_color = source->color ? source->color : SOURCE_COLOR_IMPOSSIBLE;
                }
                else
                {
                    direction_color = playfield_possible_color(playfield, x - 1, y, visited, PIPE_CONN_E);
                }
                break;
            }
        }

        if (direction_color == SOURCE_COLOR_IMPOSSIBLE)
        {
            // We got our answer.
            return SOURCE_COLOR_IMPOSSIBLE;
        }

        if (source_color == SOURCE_COLOR_NONE && direction_color != SOURCE_COLOR_NONE)
        {
            source_color = direction_color;
        }
        else if (source_color != SOURCE_COLOR_NONE && direction_color == SOURCE_COLOR_NONE)
        {
            // This is fine, leave source color alone.
        }
        else if (source_color == direction_color)
        {
            // This is fine, leave source color alone.
        }
        else
        {
            if ((source_color & direction_color) == source_color)
            {
                // This is okay, the direction color contains more bands than ourselves,
                // or its identical to the source color, so the color remains the same.
            }
            else if ((source_color & direction_color) == direction_color)
            {
                // This is okay, the source color contains more bands than the direction
                // color or it is identical to the source color, so we update to the
                // direction color.
                source_color = direction_color;
            }
            else
            {
                // This is not okay! Wrong color bands touching.
                return SOURCE_COLOR_IMPOSSIBLE;
            }
        }
    }

    return source_color;
}

void playfield_mark_impossible(playfield_t *playfield, int x, int y, int in_direction)
{
    // First, if this doesn't have a connection in the in direction, don't destroy it.
    playfield_entry_t *cur = playfield_entry(playfield, x, y);
    if (cur->color == SOURCE_COLOR_IMPOSSIBLE)
    {
        return;
    }
    if (cur->block == BLOCK_TYPE_NONE)
    {
        // No block here, so do nothing.
        return;
    }
    if (in_direction != 0)
    {
        if ((cur->pipe & in_direction) == 0)
        {
            return;
        }
    }

    // Calculate the other directions of the pipe by removing the in direction.
    unsigned int out_directions = cur->pipe & (~in_direction);
    cur->color = SOURCE_COLOR_IMPOSSIBLE;
    for (int i = 0; i < 4; i++)
    {
        unsigned int out_direction = out_directions & (1 << i);
        switch(out_direction)
        {
            case PIPE_CONN_N:
            {
                // Goes out north. Either it hits a light block or it goes to another block.
                if (y > 0)
                {
                    playfield_mark_impossible(playfield, x, y - 1, PIPE_CONN_S);
                }
                break;
            }
            case PIPE_CONN_S:
            {
                // Goes out south. Either it hits a light block or it goes to another block.
                if (y < playfield->height - 1)
                {
                    playfield_mark_impossible(playfield, x, y + 1, PIPE_CONN_N);
                }
                break;
            }
            case PIPE_CONN_E:
            {
                // Goes out east. Either it hits a light block or it goes to another block.
                if (x < playfield->width - 1)
                {
                    playfield_mark_impossible(playfield, x + 1, y, PIPE_CONN_W);
                }
                break;
            }
            case PIPE_CONN_W:
            {
                // Goes out west. Either it hits a light block or it goes to another block.
                if (x > 0)
                {
                    playfield_mark_impossible(playfield, x - 1, y, PIPE_CONN_E);
                }
                break;
            }
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

    // Now, find and mark impossible chunks of pipes.
    if (gamerule_placing)
    {
        for (int y = 0; y < playfield->height; y++)
        {
            for (int x = 0; x < playfield->width; x++)
            {
                playfield_entry_t *cur = playfield_entry(playfield, x, y);
                if (cur->block != BLOCK_TYPE_NONE && cur->color == SOURCE_COLOR_NONE)
                {
                    char *visited = malloc(playfield->width * playfield->height);
                    memset(visited, 0, playfield->width * playfield->height);

                    if (playfield_possible_color(playfield, x, y, visited, 0) == SOURCE_COLOR_IMPOSSIBLE)
                    {
                        playfield_mark_impossible(playfield, x, y, 0);
                    }
                    free(visited);
                }
            }
        }
    }

    // Now, for anything that changed, reset its age.
    int activated = 0;
    int wrong = 0;
    for (int y = 0; y < playfield->height; y++)
    {
        for (int x = 0; x < playfield->width; x++)
        {
            // Turn off all connections and then recalculate.
            playfield_entry_t *cur = playfield_entry(playfield, x, y);
            playfield_entry_t *old = oldentries + (y * playfield->width) + x;

            if (cur->color != old->color)
            {
                if (cur->color == SOURCE_COLOR_IMPOSSIBLE)
                {
                    wrong = 1;
                }
                else if (cur->color != SOURCE_COLOR_NONE)
                {
                    activated = 1;
                }
                cur->age = 0;
            }
        }
    }

    if (activated)
    {
        audio_play_registered_sound(activate_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 1.0);
    }
    if (wrong)
    {
        audio_play_registered_sound(bad_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 1.0);
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
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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

#define MAX_AGE 60

void playfield_age(playfield_t *playfield)
{
    static int mult[8] = {0, 1, 1, 2, 1, 2, 2, 4};
    int cleared = 0;

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
                    if (cur->color == SOURCE_COLOR_IMPOSSIBLE)
                    {
                        playfield->score -= 5;
                    }
                    else
                    {
                        cleared = 1;
                        playfield->score += mult[cur->color & 7] * 5;
                    }

                    memset(cur, 0, sizeof(playfield_entry_t));
                }
                else
                {
                    cur->age ++;
                }
            }
        }
    }

    if (cleared)
    {
        audio_play_registered_sound(clear_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 1.0);
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }

    if (playfield->score < 0)
    {
        playfield->score = 0;
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
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
            }
            break;
        }
        case CURSOR_MOVE_DOWN:
        {
            if (playfield->cury < (playfield->height - 1))
            {
                playfield->cury++;
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
            }
            break;
        }
        case CURSOR_MOVE_LEFT:
        {
            if (playfield->curx > 0)
            {
                playfield->curx--;
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
            }
            break;
        }
        case CURSOR_MOVE_RIGHT:
        {
            if (playfield->curx < (playfield->width - 1))
            {
                playfield->curx++;
                audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                    audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                    audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                        audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                        audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                        audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                        audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                    audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
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
                    audio_play_registered_sound(scroll_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 0.8);
                }
            }
            break;
        }
    }

    playfield_check_connections(playfield);
}

int playfield_cursor_drop(playfield_t *playfield)
{
    int dropped = 0;
    playfield_entry_t *cur = playfield_entry(playfield, playfield->curx, playfield->cury);
    if (cur->block == BLOCK_TYPE_NONE && playfield->upnext->block != BLOCK_TYPE_NONE)
    {
        // Assign the block to the actual playfield.
        memcpy(cur, playfield->upnext, sizeof(playfield_entry_t));
        audio_play_registered_sound(drop_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 1.0);

        // Prepare the next upnext block.
        memmove(&playfield->upnext[0], &playfield->upnext[1], sizeof(playfield_entry_t) * (UPNEXT_AMOUNT - 1));
        memset(&playfield->upnext[UPNEXT_AMOUNT - 1], 0, sizeof(playfield_entry_t));
        playfield_generate_upnext(playfield);
        dropped = 1;
    }

    if (gamerule_gravity)
    {
        playfield_apply_gravity(playfield);
    }
    else
    {
        playfield_check_connections(playfield);
    }

    return dropped;
}

void playfield_decrease_placetime(playfield_t *playfield, float elapsed)
{
    if (gamerule_placetimer)
    {
        playfield->timeleft -= elapsed;
    }
}

void playfield_drop_anywhere(playfield_t *playfield)
{
    if (gamerule_placetimer)
    {
        if (playfield->timeleft <= 0.0 && playfield->upnext->block != BLOCK_TYPE_NONE)
        {
            // Try to drop on the cursor.
            int success = playfield_cursor_drop(playfield);
            if (success)
            {
                return;
            }

            // Drop randomly.
            int available = 0;
            for (int y = 0; y < playfield->height; y++)
            {
                for (int x = 0; x < playfield->width; x++)
                {
                    playfield_entry_t *cur = playfield_entry(playfield, x, y);
                    if (cur->block == BLOCK_TYPE_NONE)
                    {
                        available++;
                    }
                }
            }

            if (available)
            {
                int location = (int)(chance() * available);
                int actual = 0;
                for (int y = 0; y < playfield->height; y++)
                {
                    for (int x = 0; x < playfield->width; x++)
                    {
                        playfield_entry_t *cur = playfield_entry(playfield, x, y);
                        if (cur->block == BLOCK_TYPE_NONE)
                        {
                            if (actual == location)
                            {
                                // Assign the block to the actual playfield.
                                memcpy(cur, playfield->upnext, sizeof(playfield_entry_t));
                                audio_play_registered_sound(drop_sound, SPEAKER_LEFT | SPEAKER_RIGHT, 1.0);

                                // Prepare the next upnext block.
                                memmove(&playfield->upnext[0], &playfield->upnext[1], sizeof(playfield_entry_t) * (UPNEXT_AMOUNT - 1));
                                memset(&playfield->upnext[UPNEXT_AMOUNT - 1], 0, sizeof(playfield_entry_t));
                                playfield_generate_upnext(playfield);

                                if (gamerule_gravity)
                                {
                                    playfield_apply_gravity(playfield);
                                }
                                else
                                {
                                    playfield_check_connections(playfield);
                                }

                                return;
                            }
                            actual++;
                        }
                    }
                }
            }
        }
    }
}

void playfield_run(playfield_t *playfield)
{
    memset(playfield->entries, 0, sizeof(playfield_entry_t) * playfield->width * playfield->height);
    memset(playfield->sources, 0, sizeof(source_entry_t) * ((playfield->width * 2) + (playfield->height * 2)));
    memset(playfield->upnext, 0, sizeof(playfield_entry_t) * UPNEXT_AMOUNT);

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

    playfield->score = 0;
    playfield->running = 1;

    // Choose a random audio track and start it.
    char *audiotracks[5] = {
        "rom://music/ts1.xm",
        "rom://music/ts2.xm",
        "rom://music/ts3.xm",
        "rom://music/ts4.xm",
        "rom://music/ts5.xm",
    };

    playfield->instructions = music_play(audiotracks[(int)(chance() * 5.0)]);
}

void playfield_stop(playfield_t *playfield)
{
    playfield->running = 0;
    if (playfield->instructions)
    {
        music_stop(playfield->instructions);
        playfield->instructions = 0;
    }
}

int playfield_running(playfield_t *playfield)
{
    if (playfield_game_over(playfield))
    {
        playfield_stop(playfield);
    }

    if (playfield->running == 0)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

void main()
{
    // Make sure we have truer random.
    srand(rtc_get());

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
    impossible = sprite_load("rom://sprites/impossible");

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

    // Load sound effects.
    unsigned int activate_length;
    void *activate = asset_load("rom://sounds/activate", &activate_length);
    unsigned int bad_length;
    void *bad = asset_load("rom://sounds/bad", &bad_length);
    unsigned int clear_length;
    void *clear = asset_load("rom://sounds/clear", &clear_length);
    unsigned int drop_length;
    void *drop = asset_load("rom://sounds/drop", &drop_length);
    unsigned int scroll_length;
    void *scroll = asset_load("rom://sounds/scroll", &scroll_length);

    // Register sounds to be played whenever.
    audio_init();
    activate_sound = audio_register_sound(AUDIO_FORMAT_16BIT, 44100, activate, activate_length / 2);
    bad_sound = audio_register_sound(AUDIO_FORMAT_16BIT, 44100, bad, bad_length / 2);
    clear_sound = audio_register_sound(AUDIO_FORMAT_16BIT, 44100, clear, clear_length / 2);
    drop_sound = audio_register_sound(AUDIO_FORMAT_16BIT, 44100, drop, drop_length / 2);
    scroll_sound = audio_register_sound(AUDIO_FORMAT_16BIT, 44100, scroll, scroll_length / 2);

    playfield_t *playfield = playfield_new(video_is_vertical(), PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT);

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

        if (pressed.test || pressed.psw1)
        {
            enter_test_mode();
        }

        if (playfield_running(playfield))
        {
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

            if (gamerule_placing)
            {
                playfield_drop_anywhere(playfield);
            }
        }
        else
        {
            if (pressed.player1.start)
            {
                playfield_run(playfield);
            }
        }

        if (playfield_running(playfield))
        {
            // Age the playfield so we can get rid of any beams that have stuck
            // around too long.
            playfield_age(playfield);
        }

        // Draw the playfield
        int width;
        int height;
        playfield_metrics(playfield, &width, &height);
        playfield_draw((video_width() - width) / 2, 24, playfield);

        // Draw debugging
        if (held.player1.service || held.player2.service || held.psw2)
        {
            video_draw_debug_text(
                (video_width() / 2) - (18 * 4),
                video_height() - 32,
                rgb(0, 200, 255),
                "FPS: %.01f, %dx%d\n  us frame: %u",
                fps_value, video_width(), video_height(),
                draw_time
            );
        }

        // Calculate draw time
        draw_time = profile_end(drawprofile);

        // Wait for vblank and draw it!
        video_display_on_vblank();

        // Calcualte instantaneous FPS, adjust animation counters.
        uint32_t uspf = profile_end(fps);
        fps_value = (1000000.0 / (double)uspf) + 0.01;

        if (playfield_running(playfield) && gamerule_placing)
        {
            // Make sure there's some time limit for placing.
            playfield_decrease_placetime(playfield, (float)uspf / 1000000.0);
        }
    }
}

#define CREDITS_LINES 9

void test()
{
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(0, 0, 0));

    while ( 1 )
    {
        maple_poll_buttons();
        jvs_buttons_t pressed = maple_buttons_pressed();

        // Exit back to system menu on test pressed.
        if (pressed.test || pressed.psw1)
        {
            enter_test_mode();
        }

        char *lines[CREDITS_LINES] = {
            "Beam Frenzy",
            "Idea and code by DragonMinded",
            "",
            "You are free to use, play, remix or redistribute",
            "this for non-commercial purposes only!",
            "",
            "Happy homebrewing!",
            "",
            "press [test] to exit",
        };

        for (int i = 0; i < CREDITS_LINES; i++)
        {
            int len = strlen(lines[i]);

            video_draw_debug_text((video_width() - (len * 8)) / 2, (i * 8) + ((video_height() - (CREDITS_LINES * 8)) / 2), rgb(255, 255, 255), lines[i]);
        }

        video_display_on_vblank();
    }
}
