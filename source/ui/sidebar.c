/* sidebar.c - left sidebar with pinned folders and connected devices. */

#include "ui/sidebar.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <strings.h>

#include <libudev.h>

#include "config.h"
#include "icons/icons.h"
#include "ui/theme.h"

#define LIZ_SIDEBAR_PAD_X 10
#define LIZ_SIDEBAR_ICON_GAP 6 /* between an item's icon and its label */

static int liz_sidebar_top(void)
{
    return LIZ_UI_NAV_H;
}

static int liz_sidebar_bottom(xwindow* w)
{
    return w->height - LIZ_UI_STATUS_H;
}

static void liz_sidebar_add_pinned(liz_app* app, const char* label, const char* path,
                                  const char* icon)
{
    if (app->sidebar.pinned_count >= LIZ_SIDEBAR_PINNED_MAX)
        return;
    liz_sidebar_entry* e = &app->sidebar.pinned[app->sidebar.pinned_count++];
    snprintf(e->label, sizeof(e->label), "%.*s", (int)sizeof(e->label) - 1, label);
    snprintf(e->path, sizeof(e->path), "%.*s", (int)sizeof(e->path) - 1, path);
    snprintf(e->icon, sizeof(e->icon), "%.*s", (int)sizeof(e->icon) - 1, icon);
    e->mounted = true; /* pinned links are always available: draw them bright */
}

static void liz_sidebar_add_device(liz_app* app, const char* label, const char* path,
                                  const char* dev, const char* uri, bool mounted,
                                  const char* icon)
{
    if (app->sidebar.devices_count >= LIZ_SIDEBAR_DEVICES_MAX)
        return;
    liz_sidebar_entry* e = &app->sidebar.devices[app->sidebar.devices_count++];
    snprintf(e->label, sizeof(e->label), "%.*s", (int)sizeof(e->label) - 1, label);
    snprintf(e->path, sizeof(e->path), "%.*s", (int)sizeof(e->path) - 1, path);
    snprintf(e->dev, sizeof(e->dev), "%.*s", (int)sizeof(e->dev) - 1,
             dev ? dev : "");
    snprintf(e->uri, sizeof(e->uri), "%.*s", (int)sizeof(e->uri) - 1,
             uri ? uri : "");
    e->mounted = mounted;
    snprintf(e->icon, sizeof(e->icon), "%.*s", (int)sizeof(e->icon) - 1, icon);
}

/* Media mounts are the ones real file managers surface as removable or
 * manually mounted devices: anything under /media, /mnt or /run/media. */
static bool liz_sidebar_is_media_mount(const char* mnt)
{
    return strncmp(mnt, "/media/", 7) == 0
        || strncmp(mnt, "/mnt/", 5) == 0
        || strncmp(mnt, "/run/media/", 11) == 0;
}

/* Formats `bytes` the way UDisks does for unnamed volumes, so the sidebar
 * matches Thunar/Nautilus: power-of-ten units, one decimal below 10 and
 * none at or above it. */
static void liz_sidebar_format_size(unsigned long long bytes, char* out, size_t outsz)
{
    const double kb = 1000.0;
    const double mb = 1000.0 * 1000.0;
    const double gb = 1000.0 * 1000.0 * 1000.0;
    const double tb = 1000.0 * 1000.0 * 1000.0 * 1000.0;

    double val;
    const char* unit;
    if (bytes < mb) {
        val = (double)bytes / kb;
        unit = "KB";
    } else if (bytes < gb) {
        val = (double)bytes / mb;
        unit = "MB";
    } else if (bytes < tb) {
        val = (double)bytes / gb;
        unit = "GB";
    } else {
        val = (double)bytes / tb;
        unit = "TB";
    }
    snprintf(out, outsz, "%.*f %s", val < 10.0 ? 1 : 0, val, unit);
}

/* Builds the display name for a mounted device the way GIO/UDisks do: the
 * filesystem label when the volume has one, otherwise "<size> Volume".
 * Returns true on success, false when udev has no record for `dev` and the
 * caller should keep its mountpoint fallback. */
static bool liz_sidebar_device_name(struct udev* udev_ctx, const char* dev,
                                   char* out, size_t outsz)
{
    const char* sysname = strrchr(dev, '/');
    sysname = (sysname && sysname[1]) ? sysname + 1 : dev;

    struct udev_device* d =
        udev_device_new_from_subsystem_sysname(udev_ctx, "block", sysname);
    if (!d)
        return false;

    bool ok = false;
    const char* label = udev_device_get_property_value(d, "ID_FS_LABEL");
    if (label && label[0]) {
        snprintf(out, outsz, "%.*s", (int)outsz - 1, label);
        ok = true;
    } else {
        const char* size = udev_device_get_sysattr_value(d, "size");
        unsigned long long bytes = size ? strtoull(size, NULL, 10) * 512ULL : 0;
        if (bytes > 0) {
            liz_sidebar_format_size(bytes, out, outsz);
            strncat(out, " Volume", outsz - strlen(out) - 1);
            ok = true;
        }
    }
    udev_device_unref(d);
    return ok;
}

/* Basenames of the block devices currently seen mounted in /proc/mounts,
 * so the unmounted-device scan below can skip volumes already listed. */
#define LIZ_SIDEBAR_MOUNTED_MAX 64
#define LIZ_SIDEBAR_DEVNAME_MAX 64

static bool liz_sidebar_dev_mounted(const char mounted[][LIZ_SIDEBAR_DEVNAME_MAX],
                                    int count, const char* sysname)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(mounted[i], sysname) == 0)
            return true;
    }
    return false;
}

/* True when sysname is a virtual or otherwise non-physical block device
 * that should never appear as a mountable volume. */
static bool liz_sidebar_is_virtual_blockdev(const char* sysname)
{
    static const char* prefixes[] = {
        "loop", "ram", "zram", "sr", "dm-", "md", "fd", "nbd",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(sysname, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    }
    return false;
}

/* True for udev block devices that are partitions of (or whole) removable
 * disks: the same volumes Thunar/Nautilus list even while unmounted. */
static bool liz_sidebar_is_removable(struct udev_device* d)
{
    const char* devtype = udev_device_get_devtype(d);
    /* both the device itself and get_parent_with_subsystem_devtype() hand
     * out borrowed pointers owned by `d` -- they must never be unref'd */
    struct udev_device* disk =
        (devtype && strcmp(devtype, "disk") == 0)
            ? d
            : udev_device_get_parent_with_subsystem_devtype(d, "block", "disk");
    if (!disk)
        return false;

    bool removable = false;
    const char* attr = udev_device_get_sysattr_value(disk, "removable");
    if (attr && strcmp(attr, "1") == 0)
        removable = true;

    /* some readers report removable=0 on the disk but tag the media via
     * udev properties (SD/flash bridges do this) */
    if (!removable) {
        const char* prop = udev_device_get_property_value(d, "ID_DRIVE_REMOVABLE");
        if (prop && strcmp(prop, "1") == 0)
            removable = true;
    }

    return removable;
}

/* Adds still-unmounted partitions/volumes of removable disks. They carry
 * no mount point yet; mounting happens from the context menu. */
static void liz_sidebar_scan_unmounted(liz_app* app, struct udev* udev_ctx,
                                       const char mounted[][LIZ_SIDEBAR_DEVNAME_MAX],
                                       int mounted_count)
{
    struct udev_enumerate* en = udev_enumerate_new(udev_ctx);
    if (!en)
        return;
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry* it;
    udev_list_entry_foreach(it, udev_enumerate_get_list_entry(en)) {
        struct udev_device* d =
            udev_device_new_from_syspath(udev_ctx, udev_list_entry_get_name(it));
        if (!d)
            continue;

        const char* sysname = udev_device_get_sysname(d);
        const char* devtype = udev_device_get_devtype(d);
        bool partition = devtype && strcmp(devtype, "partition") == 0;
        if ((!partition && !(devtype && strcmp(devtype, "disk") == 0))
            || liz_sidebar_is_virtual_blockdev(sysname)
            || liz_sidebar_dev_mounted(mounted, mounted_count, sysname)) {
            udev_device_unref(d);
            continue;
        }

        /* only volumes that actually hold something mountable: plain
         * partition-table holders expose no ID_FS_TYPE */
        const char* fstype = udev_device_get_property_value(d, "ID_FS_TYPE");
        if (!fstype || !fstype[0] || !liz_sidebar_is_removable(d)) {
            udev_device_unref(d);
            continue;
        }

        const char* node = udev_device_get_devnode(d);
        char name[LIZ_FS_NAME_MAX];
        if (!liz_sidebar_device_name(udev_ctx, node, name, sizeof(name))) {
            snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1,
                     strrchr(node, '/') ? strrchr(node, '/') + 1 : node);
        }
        liz_sidebar_add_device(app, name, "", node, "", false,
                               "drive-removable-media");
        udev_device_unref(d);
    }
    udev_enumerate_unref(en);
}

/* gvfs percent-encodes the fuse directory name for a mount; only the
 * characters it considers unsafe in a host string need escaping here. */
static void liz_sidebar_gvfs_encode(const char* host, char* out, size_t outsz)
{
    size_t n = 0;
    for (; *host && n + 4 <= outsz; host++) {
        if (*host == '[' || *host == ']' || *host == ':' || *host == ','
            || *host == '%') {
            snprintf(out + n, 4, "%%%02X", (unsigned char)*host);
            n += 3;
        } else {
            out[n++] = *host;
        }
    }
    out[n] = '\0';
}

/* Case/underscore-insensitive comparison key, so predicted mount names
 * match however gvfs chose to sanitize them. */
static void liz_sidebar_canonical(char* out, size_t outsz, const char* in)
{
    size_t n = 0;
    for (; in && *in && n + 1 < outsz; in++) {
        if (isalnum((unsigned char)*in))
            out[n++] = (char)tolower((unsigned char)*in);
    }
    out[n] = '\0';
}

/* Adds USB phones/media players connected in file-transfer mode. They are
 * never block devices -- MTP/PTP only speaks through GVFS -- so they show
 * up neither in /proc/mounts nor in the block scan above. Detection uses
 * the same signals gvfs itself uses: ID_MEDIA_PLAYER from libmtp's udev
 * rules, or a still-image (class 06) USB interface for PTP cameras.
 *
 * Activation is keyed on gvfs's own naming: current versions identify an
 * MTP device by stable identity ("<vendor>_<model>_<serial>", where udev's
 * usb_id builtin already replaced spaces with underscores), while older
 * ones and gphoto2 use "[usb:<bus>,<dev>]". Rather than guessing one form,
 * the existing $XDG_RUNTIME_DIR/gvfs entries are scanned and matched
 * against both candidates. */
static void liz_sidebar_scan_phones(liz_app* app, struct udev* udev_ctx)
{
    struct udev_enumerate* en = udev_enumerate_new(udev_ctx);
    if (!en)
        return;
    udev_enumerate_add_match_subsystem(en, "usb");
    udev_enumerate_add_match_property(en, "DEVTYPE", "usb_device");
    udev_enumerate_scan_devices(en);

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    char gvfs_base[512];
    if (runtime && runtime[0]) {
        snprintf(gvfs_base, sizeof(gvfs_base), "%s/gvfs", runtime);
    } else {
        snprintf(gvfs_base, sizeof(gvfs_base), "/run/user/%d/gvfs", getuid());
    }

    struct udev_list_entry* it;
    udev_list_entry_foreach(it, udev_enumerate_get_list_entry(en)) {
        struct udev_device* d =
            udev_device_new_from_syspath(udev_ctx, udev_list_entry_get_name(it));
        if (!d)
            continue;

        const char* scheme = NULL;
        if (udev_device_get_property_value(d, "ID_MEDIA_PLAYER")) {
            scheme = "mtp";
        } else {
            /* ID_USB_INTERFACES is a colon-wrapped list of interface
             * descriptors ("::060101:..."), class 06 = still image/PTP */
            const char* ifs =
                udev_device_get_property_value(d, "ID_USB_INTERFACES");
            if (ifs && strstr(ifs, ":06"))
                scheme = "gphoto2";
        }
        if (!scheme) {
            udev_device_unref(d);
            continue;
        }

        const char* bus_str = udev_device_get_property_value(d, "BUSNUM");
        const char* dev_str = udev_device_get_property_value(d, "DEVNUM");
        if (!bus_str || !dev_str) {
            bus_str = udev_device_get_sysattr_value(d, "busnum");
            dev_str = udev_device_get_sysattr_value(d, "devnum");
        }
        if (!bus_str || !dev_str) {
            udev_device_unref(d);
            continue;
        }

        unsigned bus = (unsigned)strtoul(bus_str, NULL, 10);
        unsigned devnum = (unsigned)strtoul(dev_str, NULL, 10);

        /* stable identity, matching what gvfs derives from the same udev
         * data (usb_id already substituted '_' into vendor/model) */
        const char* id_vendor = udev_device_get_property_value(d, "ID_VENDOR");
        const char* id_model = udev_device_get_property_value(d, "ID_MODEL");
        const char* serial = udev_device_get_sysattr_value(d, "serial");
        char host_stable[160] = "";
        if (id_vendor && id_vendor[0] && id_model && id_model[0]) {
            if (serial && serial[0])
                snprintf(host_stable, sizeof(host_stable), "%s_%s_%s",
                         id_vendor, id_model, serial);
            else
                snprintf(host_stable, sizeof(host_stable), "%s_%s",
                         id_vendor, id_model);
        }
        char host_usb[32];
        snprintf(host_usb, sizeof(host_usb), "[usb:%03u,%03u]", bus, devnum);

        /* find an existing gvfs activation of this device, whatever name
         * form this gvfs version uses */
        char active_dir[PATH_MAX] = "";
        char host_used[160] = "";
        DIR* gd = opendir(gvfs_base);
        if (gd) {
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s:host=", scheme);
            struct dirent* de;
            while ((de = readdir(gd))) {
                if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                    continue;
                const char* encoded = de->d_name + strlen(prefix);
                char decoded[160];
                size_t o = 0;
                for (size_t i = 0; encoded[i] && o + 1 < sizeof(decoded); i++) {
                    unsigned hi, lo;
                    if (encoded[i] == '%'
                        && sscanf(encoded + i, "%%%1X%1X", &hi, &lo) == 2) {
                        decoded[o++] = (char)((hi << 4) | lo);
                        i += 2;
                    } else {
                        decoded[o++] = encoded[i];
                    }
                }
                decoded[o] = '\0';

                char c_dec[LIZ_SIDEBAR_DEVNAME_MAX], c_stable[LIZ_SIDEBAR_DEVNAME_MAX],
                    c_usb[LIZ_SIDEBAR_DEVNAME_MAX];
                liz_sidebar_canonical(c_dec, sizeof(c_dec), decoded);
                liz_sidebar_canonical(c_stable, sizeof(c_stable), host_stable);
                liz_sidebar_canonical(c_usb, sizeof(c_usb), host_usb);
                bool stable_match = host_stable[0] && strcmp(c_dec, c_stable) == 0;
                bool usb_match = strcmp(c_dec, c_usb) == 0;
                if (!stable_match && !usb_match)
                    continue;

                snprintf(active_dir, sizeof(active_dir), "%s/%s", gvfs_base,
                         de->d_name);
                snprintf(host_used, sizeof(host_used), "%s", decoded);
                break;
            }
            closedir(gd);
        }

        /* preferred names when the device has no activation yet: the
         * stable identifier when we can derive it, USB address otherwise */
        const char* host_prefer = host_stable[0] ? host_stable : host_usb;
        char dir[PATH_MAX];
        if (active_dir[0]) {
            snprintf(dir, sizeof(dir), "%s", active_dir);
        } else {
            char encoded[224];
            liz_sidebar_gvfs_encode(host_prefer, encoded, sizeof(encoded));
            snprintf(dir, sizeof(dir), "%s/%s:host=%s", gvfs_base, scheme,
                     encoded);
        }

        const char* host_uri = host_used[0] ? host_used : host_prefer;
        char uri[192];
        snprintf(uri, sizeof(uri), "%s://%s", scheme, host_uri);

        struct stat st;
        bool mounted = active_dir[0] != '\0' && stat(dir, &st) == 0
            && S_ISDIR(st.st_mode);

        /* label: the device's own strings first, database names second */
        const char* vendor = udev_device_get_sysattr_value(d, "manufacturer");
        const char* model = udev_device_get_sysattr_value(d, "product");
        if (!vendor || !vendor[0])
            vendor = udev_device_get_property_value(d, "ID_VENDOR_FROM_DATABASE");
        if (!model || !model[0])
            model = udev_device_get_property_value(d, "ID_MODEL_FROM_DATABASE");
        char label[LIZ_FS_NAME_MAX];
        if (model && model[0]
            && (!vendor || !vendor[0]
                || strncasecmp(vendor, model, strlen(vendor)) == 0)) {
            snprintf(label, sizeof(label), "%.*s", (int)sizeof(label) - 1, model);
        } else if (vendor && vendor[0] && model && model[0]) {
            snprintf(label, sizeof(label), "%.*s %.*s", (int)sizeof(label) / 2 - 1,
                     vendor, (int)sizeof(label) / 2 - 1, model);
        } else {
            snprintf(label, sizeof(label), "%s",
                     strcmp(scheme, "mtp") == 0 ? "Media player" : "Camera");
        }

        liz_sidebar_add_device(app, label, dir, "", uri, mounted, "phone");
        udev_device_unref(d);
    }
    udev_enumerate_unref(en);
}

/* Re-reads /proc/mounts and rebuilds the device list. The filesystem root
 * is always present as the first entry, followed by mounted media, then
 * unmounted removable volumes and USB phones.
 *
 * Drawing calls this on every frame (every hover repaint), and the udev
 * scans below walk all of sysfs -- so rescans are throttled to two per
 * second; plugged-in devices still appear near-instantly without the UI
 * stuttering between frames. */
static void liz_sidebar_refresh_devices(liz_app* app)
{
    double now = liz_app_now();
    if (now - app->sidebar.devices_checked_at < 0.5)
        return;
    app->sidebar.devices_checked_at = now;

    app->sidebar.devices_count = 0;
    liz_sidebar_add_device(app, "File system", "/", "", "", true,
                           "drive-harddisk-root");

    char mounted[LIZ_SIDEBAR_MOUNTED_MAX][LIZ_SIDEBAR_DEVNAME_MAX];
    int mounted_count = 0;

    FILE* f = fopen("/proc/mounts", "r");
    if (!f)
        return;

    struct udev* udev_ctx = udev_new();

    /* Read the file line by line and split off just the fields we use (the
     * device and the mount point). The mount options that follow can run to
     * thousands of characters (overlay/network mounts), so field-width
     * scanning is unreliable: a single too-long options field used to make
     * fscanf bail out mid-file, silently dropping every later media mount. */
    char* line = NULL;
    size_t linecap = 0;
    while (getline(&line, &linecap, f) >= 0) {
        char* save = NULL;
        char* dev = strtok_r(line, " \t\n", &save);
        char* mnt = dev ? strtok_r(NULL, " \t\n", &save) : NULL;
        if (!mnt)
            continue;

        if (strcmp(mnt, "/") == 0)
            continue;
        if (!liz_sidebar_is_media_mount(mnt))
            continue;
        if (strncmp(dev, "/dev/", 5) != 0)
            continue;

        const char* leaf = strrchr(mnt, '/');
        leaf = (leaf && leaf[1]) ? leaf + 1 : mnt;

        if (mounted_count < LIZ_SIDEBAR_MOUNTED_MAX) {
            const char* base = strrchr(dev, '/');
            base = (base && base[1]) ? base + 1 : dev;
            snprintf(mounted[mounted_count], LIZ_SIDEBAR_DEVNAME_MAX, "%s", base);
            mounted_count++;
        }

        char name[LIZ_FS_NAME_MAX];
        if (!udev_ctx || !liz_sidebar_device_name(udev_ctx, dev, name, sizeof(name)))
            snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, leaf);
        liz_sidebar_add_device(app, name, mnt, dev, "", true,
                               "drive-removable-media");
    }
    free(line);

    if (udev_ctx) {
        liz_sidebar_scan_unmounted(app, udev_ctx, mounted, mounted_count);
        liz_sidebar_scan_phones(app, udev_ctx);
        udev_unref(udev_ctx);
    }
    fclose(f);
}

static void liz_sidebar_expand_path(char* out, size_t outsz, const char* path,
                                    const char* home, const char* trash)
{
    if (strncmp(path, ":home:", 6) == 0) {
        snprintf(out, outsz, "%s%s", home, path + 6);
    } else if (strncmp(path, ":trash:", 7) == 0) {
        snprintf(out, outsz, "%s%s", trash, path + 7);
    } else {
        snprintf(out, outsz, "%.*s", (int)outsz - 1, path);
    }
}

void liz_sidebar_init(liz_app* app)
{
    app->sidebar.pinned_count = 0;
    app->sidebar.devices_count = 0;
    app->sidebar.hover_item = -1;

    char home[PATH_MAX];
    const char* h = getenv("HOME");
    if (!h || !h[0]) {
        struct passwd* pw = getpwuid(getuid());
        h = (pw && pw->pw_dir) ? pw->pw_dir : "/";
    }
    snprintf(home, sizeof(home), "%s", h);

    char trash[PATH_MAX];
    const char* xdg_data = getenv("XDG_DATA_HOME");
    const char* trash_suffix;
    const char* base;
    if (xdg_data && xdg_data[0]) {
        base = xdg_data;
        trash_suffix = "/Trash";
    } else {
        base = home;
        trash_suffix = "/.local/share/Trash";
    }
    size_t baselen = strlen(base);
    size_t suflen = strlen(trash_suffix);
    if (baselen + suflen < sizeof(trash)) {
        memcpy(trash, base, baselen);
        memcpy(trash + baselen, trash_suffix, suflen + 1);
    } else {
        trash[0] = '\0';
    }

    const char* icons[] = {
        "user-home", "user-desktop", "folder-download", "folder-pictures",
        "folder-videos", "folder-music", "folder-documents", "user-trash",
        "folder", "folder", "folder", "folder",
        "folder", "folder", "folder", "folder",
    };

    struct pinned_entry { const char* label; const char* path; };
    static const struct pinned_entry pinned[] = LIZ_PINNED;
    int n = (int)(sizeof(pinned) / sizeof(pinned[0]));
    for (int i = 0; i < n && app->sidebar.pinned_count < LIZ_SIDEBAR_PINNED_MAX; i++) {
        char resolved[PATH_MAX];
        liz_sidebar_expand_path(resolved, sizeof(resolved), pinned[i].path, home, trash);
        const char* icon = (i < 16) ? icons[i] : "folder";
        liz_sidebar_add_pinned(app, pinned[i].label, resolved, icon);
    }

    liz_sidebar_refresh_devices(app);
}

/* Maps a window y coordinate to a flattened item index: pinned entries come
 * first, then device entries. Returns -1 when the pointer is not over any
 * item (including the section headers). */
int liz_sidebar_item_at(liz_app* app, int y)
{
    int top = liz_sidebar_top();
    int bottom = liz_sidebar_bottom(app->win);
    if (y < top || y >= bottom)
        return -1;

    int idx = 0;
    int py = top + LIZ_UI_ROW_H; /* skip the "Pinned" header */
    for (int i = 0; i < app->sidebar.pinned_count; i++, idx++) {
        if (y >= py && y < py + LIZ_UI_ROW_H)
            return idx;
        py += LIZ_UI_ROW_H;
    }
    py += LIZ_UI_ROW_H; /* skip the "Devices" header */
    for (int i = 0; i < app->sidebar.devices_count; i++, idx++) {
        if (y >= py && y < py + LIZ_UI_ROW_H)
            return idx;
        py += LIZ_UI_ROW_H;
    }
    return -1;
}

static void liz_sidebar_draw_item(liz_app* app, xwindow* w, const liz_sidebar_entry* e,
                                 int idx, int y, int icon_x, int text_x, int max_w,
                                 int ascent, int descent)
{
    if (idx == app->sidebar.hover_item)
        xc_rect(w, 0, y, LIZ_UI_SIDEBAR_W, LIZ_UI_ROW_H, liz_theme_hover_bg);

    /* not-yet-mounted volumes and phones are drawn in the dim palette so
     * their state is readable at a glance */
    xc_font* font = e->mounted ? app->font : app->font_dim;

#ifdef ICON_SUPPORT
    const xc_image* icon =
        liz_icons_by_name(w, e->icon, e->mounted ? liz_theme_text : liz_theme_text_dim);
    if (icon)
        xc_image_draw(w, icon, icon_x, y + (LIZ_UI_ROW_H - LIZ_ICON_SIZE) / 2);
#else
    (void)icon_x;
#endif

    int ty = y + (LIZ_UI_ROW_H - (ascent + descent)) / 2 + ascent;
    liz_ui_text_clip(w, text_x, ty, e->label, (int)strlen(e->label), font, max_w);
}

static void liz_sidebar_draw_header(xwindow* w, const char* title, int len, int y,
                                   int text_x, xc_font* f, int ascent, int descent)
{
    int ty = y + (LIZ_UI_ROW_H - (ascent + descent)) / 2 + ascent;
    xc_text(w, text_x, ty, title, len, f);
}

void liz_sidebar_draw(liz_app* app)
{
    if (!app->sidebar_visible)
        return;

    xwindow* w = app->win;
    int top = liz_sidebar_top();
    int bottom = liz_sidebar_bottom(w);
    if (bottom <= top)
        return;

    liz_sidebar_refresh_devices(app);

    xc_rect(w, 0, top, LIZ_UI_SIDEBAR_W, bottom - top, liz_theme_panel);
    xc_rect(w, LIZ_UI_SIDEBAR_W - 1, top, 1, bottom - top, liz_theme_panel_edge);

    app->sidebar.hover_item = -1;
    if (app->mouse_x >= 0 && app->mouse_x < LIZ_UI_SIDEBAR_W)
        app->sidebar.hover_item = liz_sidebar_item_at(app, app->mouse_y);

    int ascent = 0, descent = 0;
    xc_font_metrics(app->font, &ascent, &descent);
    /* section headers keep the outer padding; items are indented past
     * their icon so the labels line up in one column */
    int icon_x = LIZ_SIDEBAR_PAD_X;
#ifdef ICON_SUPPORT
    int text_x = icon_x + LIZ_ICON_SIZE + LIZ_SIDEBAR_ICON_GAP;
#else
    int text_x = LIZ_SIDEBAR_PAD_X;
#endif
    int max_w = LIZ_UI_SIDEBAR_W - text_x - LIZ_UI_PAD;

    int y = top;
    liz_sidebar_draw_header(w, "Pinned", 6, y, LIZ_SIDEBAR_PAD_X, app->font_dim, ascent, descent);
    y += LIZ_UI_ROW_H;

    int idx = 0;
    for (int i = 0; i < app->sidebar.pinned_count; i++, idx++) {
        if (y + LIZ_UI_ROW_H > bottom)
            return;
        liz_sidebar_draw_item(app, w, &app->sidebar.pinned[i], idx, y, icon_x, text_x,
                              max_w, ascent, descent);
        y += LIZ_UI_ROW_H;
    }

    if (y + LIZ_UI_ROW_H > bottom)
        return;
    liz_sidebar_draw_header(w, "Devices", 7, y, LIZ_SIDEBAR_PAD_X, app->font_dim, ascent, descent);
    y += LIZ_UI_ROW_H;

    for (int i = 0; i < app->sidebar.devices_count; i++, idx++) {
        if (y + LIZ_UI_ROW_H > bottom)
            break;
        liz_sidebar_draw_item(app, w, &app->sidebar.devices[i], idx, y, icon_x, text_x,
                              max_w, ascent, descent);
        y += LIZ_UI_ROW_H;
    }
}

bool liz_sidebar_click(liz_app* app, int x, int y)
{
    if (!app->sidebar_visible || x < 0 || x >= LIZ_UI_SIDEBAR_W)
        return false;

    /* keep the layout consistent with what liz_sidebar_draw just painted */
    liz_sidebar_refresh_devices(app);

    int idx = liz_sidebar_item_at(app, y);
    if (idx < 0)
        return false;

    if (idx < app->sidebar.pinned_count) {
        liz_app_navigate(app, app->sidebar.pinned[idx].path);
        return true;
    }

    int di = idx - app->sidebar.pinned_count;
    if (di < 0 || di >= app->sidebar.devices_count)
        return false;
    const liz_sidebar_entry* e = &app->sidebar.devices[di];
    if (!e->mounted || !e->path[0])
        return true; /* swallow the click; mounting happens from the menu */
    liz_app_navigate(app, e->path);
    return true;
}
