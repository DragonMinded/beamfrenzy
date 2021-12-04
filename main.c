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

#define BLOCK_WIDTH 16
#define BLOCK_HEIGHT 16

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32
#define CURSOR_OFFSET_X -8
#define CURSOR_OFFSET_Y -8

void *cursor = 0;

void *block_purple = 0;

void *pipe_ns = 0;
void *pipe_ew = 0;
void *pipe_ne = 0;
void *pipe_se = 0;
void *pipe_nw = 0;
void *pipe_sw = 0;

void *source_n = 0;
void *source_e = 0;
void *source_w = 0;
void *source_s = 0;

void *source_red = 0;
void *source_green = 0;
void *source_blue = 0;

#define PLAYFIELD_BORDER 2

void playfield_metrics(playfield_t *playfield, int *width, int *height)
{
    *width = (playfield->width + 2) * BLOCK_WIDTH;
    *height = ((playfield->height + 1) * BLOCK_HEIGHT) + PLAYFIELD_BORDER;
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

    for (int pheight = -1; pheight <= playfield->height; pheight++)
    {
        for (int pwidth = -1; pwidth <= playfield->width; pwidth++)
        {
            int xloc = x + ((pwidth + 1) * BLOCK_WIDTH);
            int yloc = y + (pheight * BLOCK_HEIGHT) + PLAYFIELD_BORDER;

            if (pheight >= 0 && pheight < playfield->height && pwidth >= 0 && pwidth < playfield->width)
            {
                playfield_entry_t *cur = playfield->entries + (pheight * playfield->width) + pwidth;

                switch(cur->block)
                {
                    case BLOCK_TYPE_NONE:
                    {
                        break;
                    }
                    case BLOCK_TYPE_PURPLE:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, block_purple);
                        break;
                    }
                    default:
                    {
                        // TODO: Display error sprite?
                        break;
                    }
                }

                switch(cur->pipe)
                {
                    case PIPE_CONN_E | PIPE_CONN_W:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_ew);
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_S:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_ns);
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_E:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_ne);
                        break;
                    }
                    case PIPE_CONN_N | PIPE_CONN_W:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_nw);
                        break;
                    }
                    case PIPE_CONN_S | PIPE_CONN_E:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_se);
                        break;
                    }
                    case PIPE_CONN_S | PIPE_CONN_W:
                    {
                        video_draw_sprite(xloc, yloc, BLOCK_WIDTH, BLOCK_HEIGHT, pipe_sw);
                        break;
                    }
                    default:
                    {
                        // TODO: Display error sprite?
                        break;
                    }
                }
            }

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
                }
            }

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
    playfield_entry_t *cur = playfield->entries + (y * playfield->width) + x;
    cur->block = block;
    cur->pipe = pipe;
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

#define PLAYFIELD_WIDTH 17
#define PLAYFIELD_HEIGHT 24

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
    pipe_ew = sprite_load("rom://sprites/straightpipe");
    pipe_ns = sprite_dup_rotate_cw(pipe_ew, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_sw = sprite_load("rom://sprites/cornerpipe");
    pipe_nw = sprite_dup_rotate_cw(pipe_sw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_ne = sprite_dup_rotate_cw(pipe_nw, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    pipe_se = sprite_dup_rotate_cw(pipe_ne, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_e = sprite_load("rom://sprites/source");
    source_s = sprite_dup_rotate_cw(source_e, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_w = sprite_dup_rotate_cw(source_s, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_n = sprite_dup_rotate_cw(source_w, BLOCK_WIDTH, BLOCK_HEIGHT, 16);
    source_red = sprite_load("rom://sprites/red");
    source_green = sprite_load("rom://sprites/green");
    source_blue = sprite_load("rom://sprites/blue");

    playfield_t *playfield = playfield_new(PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT);
    playfield_set_block(playfield, 0, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_NONE);
    playfield_set_block(playfield, 1, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_N | PIPE_CONN_E);
    playfield_set_block(playfield, 2, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_S | PIPE_CONN_E);
    playfield_set_block(playfield, 3, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_N | PIPE_CONN_W);
    playfield_set_block(playfield, 4, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_S | PIPE_CONN_W);
    playfield_set_block(playfield, PLAYFIELD_WIDTH - 2, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_N | PIPE_CONN_S);
    playfield_set_block(playfield, PLAYFIELD_WIDTH - 1, PLAYFIELD_HEIGHT - 1, BLOCK_TYPE_PURPLE, PIPE_CONN_E | PIPE_CONN_W);

    playfield_set_source(playfield, -1, PLAYFIELD_HEIGHT - 2, SOURCE_COLOR_RED);
    playfield_set_source(playfield, PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT - 1, SOURCE_COLOR_GREEN);
    playfield_set_source(playfield, 4, PLAYFIELD_HEIGHT, SOURCE_COLOR_BLUE);

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
