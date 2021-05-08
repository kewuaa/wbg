#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <locale.h>
#include <assert.h>
#include <time.h>

#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/random.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <wlr-layer-shell-unstable-v1.h>
#include <pixman.h>
#include <tllist.h>

#define LOG_MODULE "wbg"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"

#if defined(WBG_HAVE_PNG)
 #include "png-wbg.h"
#endif
#if defined(WBG_HAVE_JPG)
 #include "jpg.h"
#endif

/* Top-level globals */
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;

int info = 0;
int exit_code = EXIT_SUCCESS;
int sig_fd = -1;
char *files_img[1024];

static bool have_xrgb8888 = false;

/* TODO: one per output */
static pixman_image_t *image;

struct output {
    struct wl_output *wl_output;
    uint32_t wl_name;

    char *make;
    char *model;

    int scale;
    int width;
    int height;

    int render_width;
    int render_height;

    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *layer;
    bool configured;
};
static tll(struct output) outputs;

static void
render(struct output *output)
{
    const int width = output->render_width;
    const int height = output->render_height;
    const int scale = output->scale;

    struct buffer *buf = shm_get_buffer(
        shm, width * scale, height * scale, (uintptr_t)(void *)output);

    if (buf == NULL)
        return;

    uint32_t *data = pixman_image_get_data(image);
    int img_width = pixman_image_get_width(image);
    int img_height = pixman_image_get_height(image);
    int img_stride = pixman_image_get_stride(image);
    pixman_format_code_t img_fmt = pixman_image_get_format(image);

    pixman_image_t *pix = pixman_image_create_bits_no_clear(
        img_fmt, img_width, img_height, data, img_stride);

    double sx = (double)img_width / (width * scale);
    double sy = (double)img_height / (height * scale);

    pixman_f_transform_t t;
    pixman_transform_t t2;
    pixman_f_transform_init_scale(&t, sx, sy);
    pixman_transform_from_pixman_f_transform(&t2, &t);
    pixman_image_set_transform(pix, &t2);
    pixman_image_set_filter(pix, PIXMAN_FILTER_BEST, NULL, 0);

    pixman_image_composite32(
        PIXMAN_OP_SRC,
        pix, NULL, buf->pix, 0, 0, 0, 0, 0, 0,
        width * scale, height * scale);

    pixman_image_unref(pix);

    if (info) 
        LOG_INFO("render: %dx%d (scaled from %dx%d)",
                 width * scale, height * scale,
                 img_width, img_height);

    wl_surface_set_buffer_scale(output->surf, scale);
    wl_surface_attach(output->surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(output->surf, 0, 0, width * scale, height * scale);
    wl_surface_commit(output->surf);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *output = data;
    output->render_width = w;
    output->render_height = h;
    output->configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    render(output);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
};

static void
output_destroy(struct output *output)
{
    free(output->make);
    free(output->model);
    if (output->layer != NULL)
        zwlr_layer_surface_v1_destroy(output->layer);
    if (output->surf != NULL)
        wl_surface_destroy(output->surf);
    if (output->wl_output != NULL)
        wl_output_release(output->wl_output);
}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct output *output = data;

    free(output->make);
    free(output->model);

    output->make = make != NULL ? strdup(make) : NULL;
    output->model = model != NULL ? strdup(model) : NULL;
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;

    struct output *output = data;
    output->width = width;
    output->height = height;
}

static void
output_done(void *data, struct wl_output *wl_output)
{
    struct output *output = data;
    const int width = output->width;
    const int height = output->height;
    const int scale = output->scale;

    if (info)
        LOG_INFO("output: %s %s (%dx%d, scale=%d)",
                 output->make, output->model, width, height, scale);
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct output *output = data;
    output->scale = factor;
    if (output->configured)
        render(output);
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    if (format == WL_SHM_FORMAT_XRGB8888)
        have_xrgb8888 = true;
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void
add_surface_to_output(struct output *output)
{
    if (compositor == NULL || layer_shell == NULL)
        return;

    if (output->surf != NULL)
        return;

    struct wl_surface *surf = wl_compositor_create_surface(compositor);

    /* Default input region is 'infinite', while we want it to be empty */
    struct wl_region *empty_region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surf, empty_region);
    wl_region_destroy(empty_region);

    struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surf, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");

    zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
    zwlr_layer_surface_v1_set_anchor(layer,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);

    output->surf = surf;
    output->layer = layer;

    zwlr_layer_surface_v1_add_listener(layer, &layer_surface_listener, output);
    wl_surface_commit(surf);
}

static bool
verify_iface_version(const char *iface, uint32_t version, uint32_t wanted)
{
    if (version >= wanted)
        return true;

    LOG_ERR("%s: need interface version %u, but compositor only implements %u",
            iface, wanted, version);
    return false;
}

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

        compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, required);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        shm = wl_registry_bind(
            registry, name, &wl_shm_interface, required);
        wl_shm_add_listener(shm, &shm_listener, NULL);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_output *wl_output = wl_registry_bind(
            registry, name, &wl_output_interface, required);

        tll_push_back(
            outputs, ((struct output){
                .wl_output = wl_output, .wl_name = name,
                .surf = NULL, .layer = NULL}));

        struct output *output = &tll_back(outputs);
        wl_output_add_listener(wl_output, &output_listener, output);
        add_surface_to_output(output);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required))
            return;

        layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, required);
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    tll_foreach(outputs, it) {
        if (it->item.wl_name == name) {
            LOG_DBG("destroyed: %s %s", it->item.make, it->item.model);
            output_destroy(&it->item);
            tll_remove(outputs, it);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static int 
display_init() {

    display = wl_display_connect(NULL);
    if (display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        return 1;
    }

    registry = wl_display_get_registry(display);
    if (registry == NULL)  {
        LOG_ERR("failed to get wayland registry");
        return 1;
    }

    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (compositor == NULL) {
        LOG_ERR("no compositor");
        return 1;
    }
    if (shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        return 1;
    }
    if (layer_shell == NULL) {
        LOG_ERR("no layer shell interface");
        return 1;
    }

    tll_foreach(outputs, it)
        add_surface_to_output(&it->item);

    wl_display_roundtrip(display);

    if (!have_xrgb8888) {
        LOG_ERR("shm: XRGB image format not available");
        exit_code = EXIT_FAILURE;
        return 1;
    }
    return 0;
}

static void 
display_close() {

    tll_foreach(outputs, it)
        output_destroy(&it->item);
    tll_free(outputs);

    if (layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(layer_shell);
    if (shm != NULL)
        wl_shm_destroy(shm);
    if (compositor != NULL)
        wl_compositor_destroy(compositor);
    if (registry != NULL)
        wl_registry_destroy(registry);
    if (display != NULL)
        wl_display_disconnect(display);
    if (image != NULL) {
        free(pixman_image_get_data(image));
        pixman_image_unref(image);
    }
}

static void
exit_close() {

    log_deinit();

    if (sig_fd >= 0)
        close(sig_fd);

    exit(exit_code);
}

int 
a_strcmp( const void* a, const void* b )
{
    return strcmp( *(const char**)a, *(const char**)b );
}

int
main(int argc, char *argv[])
{
    setlocale(LC_CTYPE, "");
    log_init(LOG_COLORIZE_AUTO, false, LOG_FACILITY_DAEMON, LOG_CLASS_WARNING);

    char *image_path = NULL;
    char *dir_path;
    int image_cnt = 0;

    int c;
    int dir = 0;
    int timer = 0;
    int random = 0;

    FILE *fp;

    opterr = 0;
    while ((c = getopt (argc, argv, "irt:")) != -1)
        switch (c)
        {
          case 'i':
            info = 1;
            break;
          case 'r':
            random = 1;
            break;
          case 't':
            timer = atoi(optarg);
            break;
          default:
            fprintf(stderr, "Usage:\n%s [-t sec] [-r] [-i] <file/dir name>\n", argv[0]);
            fprintf(stderr, "        -t timer seconds\n");
            fprintf(stderr, "        -r random yes (default no)\n");
            fprintf(stderr, "        -i info -- debugging\n");
            exit(0);
        }

    if (optind >= argc) {
        fprintf(stderr, "Usage:\n%s [-t sec] [-r] [-i] <file/dir name>\n", argv[0]);
        fprintf(stderr, "        -t timer seconds\n");
        fprintf(stderr, "        -r random yes (default no)\n");
        fprintf(stderr, "        -i info -- debugging\n");
        fprintf(stderr, "*** image path is required ***\n");
        exit(EXIT_FAILURE);
    }

    /* is it a file or dir */
    struct stat path_stat;
    stat(argv[optind], &path_stat);
    if (S_ISDIR(path_stat.st_mode)) {
        dir = 1;    
        dir_path = argv[optind];
    } else {
        random = 0;
        timer = 0;
        image_path = argv[optind];
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    if (timer > 0) 
        sigaddset(&mask, SIGALRM);

    sigprocmask(SIG_BLOCK, &mask, NULL);

    if ((sig_fd = signalfd(-1, &mask, 0)) < 0) {
        LOG_ERRNO("failed to create signal FD");
        exit(1);
    }

/**** handle directory of files */

    if (dir == 1) {
        int n = 0;
        DIR *d;
        struct dirent *dir;

        d = opendir(dir_path);

        /* Determine the number of files */
        while((dir = readdir(d)) != NULL) {
            /* look for *.jpg, *.png */
            if ( !strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..") || dir->d_type == DT_DIR )
            {
                ;
            } else {
                n++;
            }
        }
        rewinddir(d);

        /* Put file names into the array */
        while((dir = readdir(d)) != NULL) {
            if ( !strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..") || dir->d_type == DT_DIR ) {
                ;
            } else {
                files_img[image_cnt] = (char*) malloc (strlen(dir->d_name) + 1);
                memcpy (files_img[image_cnt], dir->d_name, strlen(dir->d_name));
                image_cnt++;
            }
        }
        rewinddir(d);
        qsort(files_img, n, sizeof(*files_img), a_strcmp);
    }

/* start loop IF dir instead of single file */
    int image_index = 0;
    char new_path[100];

    while (1) {
        image = NULL;

        if (dir == 1) {
            strcpy(new_path, dir_path);
            if ((new_path[strlen(new_path) - 1]) != '/') 
                strcat(new_path, "/");

            if (random == 1) {
                unsigned int tmp;
                if (getrandom(&tmp, sizeof(unsigned int), GRND_NONBLOCK) < 0)
                    continue;

                int rndm = tmp % image_cnt;
                strcat(new_path, files_img[rndm]);
            } else {
                strcat(new_path, files_img[image_index]);
                image_index++;
                if (image_index >= image_cnt)
                    image_index = 0;
            }
            image_path = new_path;
        }

        fp = fopen(image_path, "rb");
        if (fp == NULL) {
            LOG_ERRNO("%s: failed to open", image_path);
            exit_code = EXIT_FAILURE;
            display_close();
            exit_close();
        }

#if defined(WBG_HAVE_JPG)
        if (image == NULL)
            image = jpg_load(fp, image_path);
#endif

#if defined(WBG_HAVE_PNG)
        if (image == NULL)
            image = png_load(fp, image_path);
#endif
        fclose(fp);

        if (image == NULL) {
            if (dir == 1)
                continue;
            fprintf(stderr, "error: %s: failed to load\n", image_path);
            fclose(fp);
            exit_code = EXIT_FAILURE;
            display_close();
            exit_close();
        }
        if (info)
            LOG_INFO("image -> %s\n", image_path);

        if (display_init() != 0) {
            display_close();
            exit_close();
        }
        int err_no = 0;

        while (true) {
            wl_display_flush(display);

            struct pollfd fds[] = {
                {.fd = wl_display_get_fd(display), .events = POLLIN},
                {.fd = sig_fd, .events = POLLIN},
            };
            int ret = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);

            if (ret < 0) {
                if (errno == EINTR)
                    continue;

                LOG_ERRNO("failed to poll");
                err_no = 1;
                break;
            }

            if (fds[0].revents & POLLHUP) {
                LOG_WARN("disconnected by compositor");
                err_no = 1;
                break;
            }

            if (fds[0].revents & POLLIN)
                wl_display_dispatch(display);

            if (fds[1].revents & POLLHUP)
                abort();

            if (timer > 0) {
                alarm(timer);
            }

            if (fds[1].revents & POLLIN) {
                struct signalfd_siginfo info;
                ssize_t count = read(sig_fd, &info, sizeof(info));
                if (count < 0) {
                    if (errno == EINTR)
                        continue;

                    LOG_ERRNO("failed to read from signal FD");
                    err_no = 1;
                    break;
                }

                assert(count == sizeof(info));
                if (info.ssi_signo == SIGALRM) {
                } else {
                    err_no = 1;
                    exit_code = 1;
                }
                break;
            }
        }

        if ((err_no != 0) || (timer == 0))
            break;
        display_close();
    }
/* end of while directory images */

    exit_close();
}
