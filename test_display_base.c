#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <getopt.h>

#include <spice.h>
#include <spice/enums.h>
#include <spice/macros.h>
#include <spice/qxl_dev.h>

#include "glyphs.h"

#include "spiceterm.h"

#define MEM_SLOT_GROUP_ID 0

#define NOTIFY_DISPLAY_BATCH (SINGLE_PART/2)
#define NOTIFY_CURSOR_BATCH 10

/* these colours are from linux kernel drivers/char/vt.c */
/* the default colour table, for VGA+ colour systems */
int default_red[] = {0x00,0xaa,0x00,0xaa,0x00,0xaa,0x00,0xaa,
    0x55,0xff,0x55,0xff,0x55,0xff,0x55,0xff};
int default_grn[] = {0x00,0x00,0xaa,0x55,0x00,0x00,0xaa,0xaa,
    0x55,0x55,0xff,0xff,0x55,0x55,0xff,0xff};
int default_blu[] = {0x00,0x00,0x00,0x00,0xaa,0xaa,0xaa,0xaa,
    0x55,0x55,0x55,0x55,0xff,0xff,0xff,0xff};

/* Parts cribbed from spice-display.h/.c/qxl.c */

typedef struct SimpleSpiceUpdate {
    QXLCommandExt ext; // first
    QXLDrawable drawable;
    QXLImage image;
    uint8_t *bitmap;
} SimpleSpiceUpdate;

static void test_spice_destroy_update(SimpleSpiceUpdate *update)
{
    if (!update) {
        return;
    }
    if (update->drawable.clip.type != SPICE_CLIP_TYPE_NONE) {
        uint8_t *ptr = (uint8_t*)update->drawable.clip.data;
        free(ptr);
    }
    g_free(update->bitmap);
    g_free(update);
}

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 320

//#define SINGLE_PART 4
//static const int angle_parts = 64 / SINGLE_PART;
static int unique = 1;
//static int color = -1;
//static int c_i = 0;

__attribute__((noreturn))
static void sigchld_handler(int signal_num) // wait for the child process and exit
{
    int status;
    wait(&status);
    exit(0);
}

static void set_cmd(QXLCommandExt *ext, uint32_t type, QXLPHYSICAL data)
{
    ext->cmd.type = type;
    ext->cmd.data = data;
    ext->cmd.padding = 0;
    ext->group_id = MEM_SLOT_GROUP_ID;
    ext->flags = 0;
}

static void simple_set_release_info(QXLReleaseInfo *info, intptr_t ptr)
{
    info->id = ptr;
    //info->group_id = MEM_SLOT_GROUP_ID;
}

// We shall now have a ring of commands, so that we can update
// it from a separate thread - since get_command is called from
// the worker thread, and we need to sometimes do an update_area,
// which cannot be done from red_worker context (not via dispatcher,
// since you get a deadlock, and it isn't designed to be done
// any other way, so no point testing that).


static void push_command(Test *test, QXLCommandExt *ext)
{
    g_mutex_lock(test->command_mutex);

    while (test->commands_end - test->commands_start >= COMMANDS_SIZE) {
        g_cond_wait(test->command_cond, test->command_mutex);
    }
    g_assert(test->commands_end - test->commands_start < COMMANDS_SIZE);
    test->commands[test->commands_end % COMMANDS_SIZE] = ext;
    test->commands_end++;
    g_mutex_unlock(test->command_mutex);

    test->qxl_worker->wakeup(test->qxl_worker);
}

/* bitmap are freed, so they must be allocated with g_malloc */
SimpleSpiceUpdate *test_spice_create_update_from_bitmap(uint32_t surface_id,
                                                        QXLRect bbox,
                                                        uint8_t *bitmap)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    uint32_t bw, bh;

    bh = bbox.bottom - bbox.top;
    bw = bbox.right - bbox.left;

    update   = g_new0(SimpleSpiceUpdate, 1);
    update->bitmap = bitmap;
    drawable = &update->drawable;
    image    = &update->image;

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_DRAW_COPY;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy.rop_descriptor  = SPICE_ROPD_OP_PUT;
    drawable->u.copy.src_bitmap      = (intptr_t)image;
    drawable->u.copy.src_area.right  = bw;
    drawable->u.copy.src_area.bottom = bh;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, unique);
    image->descriptor.type   = SPICE_IMAGE_TYPE_BITMAP;
    image->bitmap.flags      = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
    image->bitmap.stride     = bw * 4;
    image->descriptor.width  = image->bitmap.x = bw;
    image->descriptor.height = image->bitmap.y = bh;
    image->bitmap.data = (intptr_t)bitmap;
    image->bitmap.palette = 0;
    image->bitmap.format = SPICE_BITMAP_FMT_32BIT;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static SimpleSpiceUpdate *test_draw_char(Test *test, int x, int y, int c, int fg, int bg)
{
    int top, left;
    uint8_t *dst;
    uint8_t *bitmap;
    int bw, bh;
    int i, j;
    QXLRect bbox;

    left = x*8;
    top = y*16;

    // printf("DRAWCHAR %d %d %d\n", left, top, c);
 
    unique++;

    bw       = 8;
    bh       = 16;

    bitmap = dst = g_malloc(bw * bh * 4);

    unsigned char *data = vt_font_data + c*16;
    unsigned char d = *data;

    g_assert(fg >= 0 && fg < 16);
    g_assert(bg >= 0 && bg < 16);

    unsigned char fgc_red = default_red[fg];
    unsigned char fgc_blue = default_blu[fg];
    unsigned char fgc_green = default_grn[fg];
    unsigned char bgc_red = default_red[bg];
    unsigned char bgc_blue = default_blu[bg];
    unsigned char bgc_green = default_grn[bg];

    for (j = 0; j < 16; j++) {
        for (i = 0; i < 8; i++) {
            if ((i&7) == 0) {
                d=*data;
                data++;
            }
            if (d&0x80) {
                 *(dst) = fgc_blue;
                 *(dst+1) = fgc_green;
                 *(dst+2) = fgc_red;
                 *(dst+3) = 0;
            } else {
                 *(dst) = bgc_blue;
                 *(dst+1) = bgc_green;
                 *(dst+2) = bgc_red;
                 *(dst+3) = 0;
            }
            d<<=1;
            dst += 4;
        }
    }

    bbox.left = left; bbox.top = top;
    bbox.right = left + bw; bbox.bottom = top + bh;

    return test_spice_create_update_from_bitmap(0, bbox, bitmap);
}

void test_spice_scroll(Test *test, int x1, int y1, int x2, int y2, int src_x, int src_y)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLRect bbox;

    int surface_id = 0; // fixme

    update   = g_new0(SimpleSpiceUpdate, 1);
    drawable = &update->drawable;

    bbox.left = x1;
    bbox.top = y1;
    bbox.right = x2;
    bbox.bottom = y2;

    drawable->surface_id = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_COPY_BITS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy_bits.src_pos.x = src_x;
    drawable->u.copy_bits.src_pos.y = src_y;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    push_command(test, &update->ext);
}

void test_spice_clear(Test *test, int x1, int y1, int x2, int y2)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLRect bbox;

    int surface_id = 0; // fixme

    update   = g_new0(SimpleSpiceUpdate, 1);
    drawable = &update->drawable;

    bbox.left = x1;
    bbox.top = y1;
    bbox.right = x2;
    bbox.bottom = y2;

    drawable->surface_id = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_DRAW_BLACKNESS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    push_command(test, &update->ext);
}

static void create_primary_surface(Test *test, uint32_t width,
                                   uint32_t height)
{
    QXLWorker *qxl_worker = test->qxl_worker;
    QXLDevSurfaceCreate surface = { 0, };

    g_assert(height <= MAX_HEIGHT);
    g_assert(width <= MAX_WIDTH);
    g_assert(height > 0);
    g_assert(width > 0);

    surface.format     = SPICE_SURFACE_FMT_32_xRGB;
    surface.width      = test->primary_width = width;
    surface.height     = test->primary_height = height;
    surface.stride     = -width * 4; /* negative? */
    surface.mouse_mode = TRUE; /* unused by red_worker */
    surface.flags      = 0;
    surface.type       = 0;    /* unused by red_worker */
    surface.position   = 0;    /* unused by red_worker */
    surface.mem        = (uint64_t)&test->primary_surface;
    surface.group_id   = MEM_SLOT_GROUP_ID;

    test->width = width;
    test->height = height;

    qxl_worker->create_primary_surface(qxl_worker, 0, &surface);
}

QXLDevMemSlot slot = {
.slot_group_id = MEM_SLOT_GROUP_ID,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

static void attache_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    if (test->qxl_worker) {
        if (test->qxl_worker != _qxl_worker)
            printf("%s ignored, %p is set, ignoring new %p\n", __func__,
                   test->qxl_worker, _qxl_worker);
        else
            printf("%s ignored, redundant\n", __func__);
        return;
    }
    printf("%s\n", __func__);
    test->qxl_worker = _qxl_worker;
    test->qxl_worker->add_memslot(test->qxl_worker, &slot);
    create_primary_surface(test, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    test->qxl_worker->start(test->qxl_worker);
}

static void set_compression_level(QXLInstance *qin, int level)
{
    printf("%s\n", __func__);
}

static void set_mm_time(QXLInstance *qin, uint32_t mm_time)
{
}
static void get_init_info(QXLInstance *qin, QXLDevInitInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = 1;
}

// called from spice_server thread (i.e. red_worker thread)
static int get_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);
    int res = FALSE;

    g_mutex_lock(test->command_mutex);
    
    if ((test->commands_end - test->commands_start) == 0) {
        res = FALSE;
        goto ret;
    }

    *ext = *test->commands[test->commands_start % COMMANDS_SIZE];
    g_assert(test->commands_start < test->commands_end);
    test->commands_start++;
    g_cond_signal(test->command_cond);

    res = TRUE;

ret:
    g_mutex_unlock(test->command_mutex);
    return res;
}

static int req_cmd_notification(QXLInstance *qin)
{
    //Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);
    //test->core->timer_start(test->wakeup_timer, test->wakeup_ms);

    return TRUE;
}

static void release_resource(QXLInstance *qin, struct QXLReleaseInfoExt release_info)
{
    QXLCommandExt *ext = (QXLCommandExt*)(unsigned long)release_info.info->id;
    //printf("%s\n", __func__);
    g_assert(release_info.group_id == MEM_SLOT_GROUP_ID);
    switch (ext->cmd.type) {
        case QXL_CMD_DRAW:
            test_spice_destroy_update((void*)ext);
            break;
        case QXL_CMD_SURFACE:
            free(ext);
            break;
        case QXL_CMD_CURSOR: {
            QXLCursorCmd *cmd = (QXLCursorCmd *)(unsigned long)ext->cmd.data;
            if (cmd->type == QXL_CURSOR_SET) {
                free(cmd);
            }
            free(ext);
            break;
        }
        default:
            abort();
    }
}

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;

static void cursor_init()
{
    cursor.cursor.header.unique = 0;
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;

    // X drivers addes it to the cursor size because it could be
    // cursor data information or another cursor related stuffs.
    // Otherwise, the code will break in client/cursor.cpp side,
    // that expect the data_size plus cursor information.
    // Blame cursor protocol for this. :-)
    cursor.cursor.data_size += 128;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);
    static int set = 1;
    static int x = 0, y = 0;
    QXLCursorCmd *cursor_cmd;
    QXLCommandExt *cmd;

    if (!test->cursor_notify) {
        return FALSE;
    }

    test->cursor_notify--;
    cmd = calloc(sizeof(QXLCommandExt), 1);
    cursor_cmd = calloc(sizeof(QXLCursorCmd), 1);

    cursor_cmd->release_info.id = (unsigned long)cmd;

    if (set) {
        cursor_cmd->type = QXL_CURSOR_SET;
        cursor_cmd->u.set.position.x = 0;
        cursor_cmd->u.set.position.y = 0;
        cursor_cmd->u.set.visible = TRUE;
        cursor_cmd->u.set.shape = (unsigned long)&cursor;
        // Only a white rect (32x32) as cursor
        memset(cursor.data, 255, sizeof(cursor.data));
        set = 0;
    } else {
        cursor_cmd->type = QXL_CURSOR_MOVE;
        cursor_cmd->u.position.x = x++ % test->primary_width;
        cursor_cmd->u.position.y = y++ % test->primary_height;
    }

    cmd->cmd.data = (unsigned long)cursor_cmd;
    cmd->cmd.type = QXL_CMD_CURSOR;
    cmd->group_id = MEM_SLOT_GROUP_ID;
    cmd->flags    = 0;
    *ext = *cmd;
    //printf("%s\n", __func__);
    return TRUE;
}

static int req_cursor_notification(QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static void notify_update(QXLInstance *qin, uint32_t update_id)
{
    printf("%s\n", __func__);
}

static int flush_resources(QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static int client_monitors_config(QXLInstance *qin,
                                  VDAgentMonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        printf("%s: NULL monitors_config\n", __func__);
    } else {
        printf("%s: %d\n", __func__, monitors_config->num_of_monitors);
    }
    return 0;
}

static void set_client_capabilities(QXLInstance *qin,
                                    uint8_t client_present,
                                    uint8_t caps[58])
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    printf("%s: present %d caps %d\n", __func__, client_present, caps[0]);
    if (test->on_client_connected && client_present) {
        test->on_client_connected(test);
    }
    if (test->on_client_disconnected && !client_present) {
        test->on_client_disconnected(test);
    }
}

static int client_count = 0;

static void client_connected(Test *test)
{
    printf("Client connected\n");
    client_count++;
}

static void client_disconnected(Test *test)
{    

    if (client_count > 0) {
        client_count--;
        printf("Client disconnected\n");
        exit(0); // fixme: cleanup?
    }
}

static void do_conn_timeout(void *opaque)
{
    // Test *test = opaque;

    if (client_count <= 0) {
        printf("do_conn_timeout\n");
        exit (0); // fixme: cleanup?
    }
}


QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "test",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attache_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,

    /* the callbacks below are called from spice server thread context */
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
    .client_monitors_config = client_monitors_config,
    .set_client_capabilities = set_client_capabilities,
};

/* interface for tests */
void test_add_display_interface(Test* test)
{
    spice_server_add_interface(test->server, &test->qxl_instance.base);
}

static int vmc_write(SpiceCharDeviceInstance *sin, const uint8_t *buf, int len)
{
//    printf("%s: %d\n", __func__, len);
    return len;
}

static int vmc_read(SpiceCharDeviceInstance *sin, uint8_t *buf, int len)
{
//    printf("%s: %d\n", __func__, len);
    return 0;
}

static void vmc_state(SpiceCharDeviceInstance *sin, int connected)
{
//    printf("%s: %d\n", __func__, connected);
}

static SpiceCharDeviceInterface vdagent_sif = {
    .base.type          = SPICE_INTERFACE_CHAR_DEVICE,
    .base.description   = "test spice virtual channel char device",
    .base.major_version = SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
    .base.minor_version = SPICE_INTERFACE_CHAR_DEVICE_MINOR,
    .state              = vmc_state,
    .write              = vmc_write,
    .read               = vmc_read,
};

SpiceCharDeviceInstance vdagent_sin = {
    .base = {
        .sif = &vdagent_sif.base,
    },
    .subtype = "vdagent",
};

void test_add_agent_interface(SpiceServer *server)
{
    spice_server_add_interface(server, &vdagent_sin.base);
}

static void kbd_push_key(SpiceKbdInstance *sin, uint8_t frag)
{
    Test *test = SPICE_CONTAINEROF(sin, Test, keyboard_sin);

    printf("KEYCODE %u %p\n", frag, test);

}

void test_draw_update_char(Test *test, int x, int y, gunichar ch, TextAttributes attrib)
{
    int fg, bg;

    if (attrib.invers) {
        bg = attrib.fgcol;
        fg = attrib.bgcol;
    } else {
        bg = attrib.bgcol;
        fg = attrib.fgcol;
    }

    if (attrib.bold) {
        fg += 8;
    }

    // unsuported attributes = (attrib.blink || attrib.unvisible)

    //if (attrib.uline) {
    //rfbDrawLine (vt->screen, rx, ry + 14, rxe, ry + 14, fg);
    //}

    int c = vt_fontmap[ch];

    SimpleSpiceUpdate *update;
    update = test_draw_char(test, x, y, c, fg, bg);
    push_command(test, &update->ext);
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
    return 0;
}

static SpiceKbdInterface keyboard_sif = {
    .base.type          = SPICE_INTERFACE_KEYBOARD ,
    .base.description   = "spiceterm keyboard device",
    .base.major_version = SPICE_INTERFACE_KEYBOARD_MAJOR,
    .base.minor_version = SPICE_INTERFACE_KEYBOARD_MINOR,
    .push_scan_freg     = kbd_push_key,
    .get_leds           = kbd_get_leds,
};

void test_add_keyboard_interface(Test* test)
{
    spice_server_add_interface(test->server, &test->keyboard_sin.base);
}

Test *test_new(SpiceCoreInterface *core)
{
    int port = 5912;
    Test *test = g_new0(Test, 1);
    SpiceServer* server = spice_server_new();

    test->command_cond = g_cond_new();
    test->command_mutex = g_mutex_new();

    test->on_client_connected = client_connected,
    test->on_client_disconnected = client_disconnected,

    test->qxl_instance.base.sif = &display_sif.base;
    test->qxl_instance.id = 0;

    test->keyboard_sin.base.sif = &keyboard_sif.base;
 
    test->core = core;
    test->server = server;

    test->cursor_notify = NOTIFY_CURSOR_BATCH;
    // some common initialization for all display tests
    printf("TESTER: listening on port %d (unsecure)\n", port);
    spice_server_set_port(server, port);
    spice_server_set_noauth(server);
    int res = spice_server_init(server, core);
    if (res != 0) {
        g_error("spice_server_init failed, res = %d\n", res);
    }

    cursor_init();

    int timeout = 10; // max time to wait for client connection
    test->conn_timeout_timer = core->timer_add(do_conn_timeout, test);
    test->core->timer_start(test->conn_timeout_timer, timeout*1000);

    return test;
}
