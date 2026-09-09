#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#include <xecore/xboxkrnl_types.h>
#include <xecore/xboxkrnl_io.h>
#include <xecore/xam_io.h>
#include <xecore/xam_loader.h>

typedef NTSTATUS STATUS;

#define STBI_NO_STDIO
#include "stb_image.h"

#include "stb_image_resize.h"

#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define XRS_VERSION "0.1.1"
#define XRS_PATH_MAX 1024
#define XRS_DIR_BUFFER_SIZE (64u * 1024u)
#define XRS_FILE_BEGIN 0u
#define XRS_FILE_END 2u
#define XRS_DEFAULT_MAX_INPUT_MB 128u
#define XRS_MAX_TARGET_DIMENSION 8192

#ifndef FILE_SHARE_DELETE
#define FILE_SHARE_DELETE 0x00000004u
#endif

#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#endif

typedef struct Config {
    char path[XRS_PATH_MAX];
    int width;
    int height;
    bool recursive;
    int jpeg_quality;
    uint32_t max_input_mb;
    bool dry_run;
} Config;

typedef struct Stats {
    uint32_t directories;
    uint32_t images_found;
    uint32_t already_correct;
    uint32_t resized;
    uint32_t failed;
    uint32_t skipped_too_large;
} Stats;

typedef struct PathStack {
    char **items;
    size_t count;
    size_t capacity;
} PathStack;

typedef struct MemoryBuffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
    bool failed;
} MemoryBuffer;

static HANDLE g_log = INVALID_HANDLE_VALUE;
static Config g_cfg;
static Stats g_stats;

static int ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int ascii_stricmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";

    while (*a && *b) {
        int ca = ascii_tolower((unsigned char)*a);
        int cb = ascii_tolower((unsigned char)*b);

        if (ca != cb) return ca - cb;

        ++a;
        ++b;
    }

    return ascii_tolower((unsigned char)*a) -
           ascii_tolower((unsigned char)*b);
}

static char *trim(char *s) {
    char *end;

    while (*s && isspace((unsigned char)*s)) ++s;

    if (!*s) return s;

    end = s + strlen(s) - 1;

    while (end >= s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    return s;
}

static bool parse_bool(const char *value, bool fallback) {
    if (!value) return fallback;

    if (!ascii_stricmp(value, "true") ||
        !ascii_stricmp(value, "yes") ||
        !strcmp(value, "1") ||
        !ascii_stricmp(value, "on")) {
        return true;
    }

    if (!ascii_stricmp(value, "false") ||
        !ascii_stricmp(value, "no") ||
        !strcmp(value, "0") ||
        !ascii_stricmp(value, "off")) {
        return false;
    }

    return fallback;
}

static void log_raw(const char *text) {
    if (!text || g_log == INVALID_HANDLE_VALUE) return;

    size_t total = strlen(text);
    size_t pos = 0;

    while (pos < total) {
        uint32_t chunk =
            (uint32_t)((total - pos) > 0x7fffffffu
                           ? 0x7fffffffu
                           : (total - pos));

        uint32_t written = 0;

        if (!WriteFile(
                g_log,
                (void *)(text + pos),
                chunk,
                &written,
                NULL) ||
            written == 0) {
            break;
        }

        pos += written;
    }
}

static void xrs_logf(const char *fmt, ...) {
    char buffer[2048];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    buffer[sizeof(buffer) - 1] = '\0';

    log_raw(buffer);
}

static HANDLE xrs_open_file(
    const char *path,
    uint32_t access,
    uint32_t creation
) {
    return CreateFileA(
        (char *)path,
        access,
        FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
        NULL,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
}

static bool write_all_file(
    const char *path,
    const void *data,
    size_t size
) {
    HANDLE file =
        xrs_open_file(
            path,
            GENERIC_WRITE,
            CREATE_ALWAYS
        );

    size_t pos = 0;

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    while (pos < size) {
        uint32_t chunk =
            (uint32_t)((size - pos) > 0x7fffffffu
                           ? 0x7fffffffu
                           : (size - pos));

        uint32_t written = 0;

        if (!WriteFile(
                file,
                (void *)(
                    (const unsigned char *)data + pos
                ),
                chunk,
                &written,
                NULL) ||
            written != chunk) {
            CloseHandle(file);
            return false;
        }

        pos += written;
    }

    CloseHandle(file);

    return true;
}

static bool read_entire_file(
    const char *path,
    unsigned char **out_data,
    size_t *out_size,
    uint32_t max_mb
) {
    HANDLE file;
    uint32_t low_size;
    uint32_t high_size = 0;
    unsigned char *data;
    size_t size;
    size_t pos = 0;

    *out_data = NULL;
    *out_size = 0;

    file =
        xrs_open_file(
            path,
            GENERIC_READ,
            OPEN_EXISTING
        );

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    low_size =
        SetFilePointer(
            file,
            0,
            (int32_t *)&high_size,
            XRS_FILE_END
        );

    if (high_size != 0) {
        CloseHandle(file);
        return false;
    }

    SetFilePointer(
        file,
        0,
        NULL,
        XRS_FILE_BEGIN
    );

    size = (size_t)low_size;

    if (max_mb &&
        size >
            (size_t)max_mb *
                1024u *
                1024u) {
        CloseHandle(file);
        return false;
    }

    data =
        (unsigned char *)malloc(
            size ? size : 1
        );

    if (!data) {
        CloseHandle(file);
        return false;
    }

    while (pos < size) {
        uint32_t chunk =
            (uint32_t)((size - pos) > 0x7fffffffu
                           ? 0x7fffffffu
                           : (size - pos));

        uint32_t got = 0;

        if (!ReadFile(
                file,
                data + pos,
                chunk,
                &got,
                NULL) ||
            got == 0) {
            free(data);
            CloseHandle(file);
            return false;
        }

        pos += got;
    }

    CloseHandle(file);

    *out_data = data;
    *out_size = size;

    return true;
}

static bool file_size_exceeds_limit(
    const char *path,
    uint32_t max_mb
) {
    HANDLE file;
    uint32_t low_size;
    uint32_t high_size = 0;
    uint64_t size;

    if (!max_mb) return false;

    file =
        xrs_open_file(
            path,
            GENERIC_READ,
            OPEN_EXISTING
        );

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    low_size =
        SetFilePointer(
            file,
            0,
            (int32_t *)&high_size,
            XRS_FILE_END
        );

    CloseHandle(file);

    size =
        ((uint64_t)high_size << 32) |
        low_size;

    return size >
           (uint64_t)max_mb *
               1024u *
               1024u;
}

static bool make_sibling_path(
    const char *source,
    const char *suffix,
    char out[XRS_PATH_MAX]
) {
    size_t a = strlen(source);
    size_t b = strlen(suffix);

    if (a + b + 1 > XRS_PATH_MAX) {
        return false;
    }

    memcpy(out, source, a);
    memcpy(out + a, suffix, b + 1);

    return true;
}

static const char *file_extension(
    const char *path
) {
    const char *dot =
        strrchr(path, '.');

    const char *slash1 =
        strrchr(path, '\\');

    const char *slash2 =
        strrchr(path, '/');

    const char *slash = slash1;

    if (!slash ||
        (slash2 && slash2 > slash)) {
        slash = slash2;
    }

    if (!dot ||
        (slash && dot < slash)) {
        return "";
    }

    return dot;
}

static bool supported_image(
    const char *path
) {
    const char *ext =
        file_extension(path);

    return
        !ascii_stricmp(ext, ".png") ||
        !ascii_stricmp(ext, ".jpg") ||
        !ascii_stricmp(ext, ".jpeg") ||
        !ascii_stricmp(ext, ".bmp");
}

static bool is_jpeg(
    const char *path
) {
    const char *ext =
        file_extension(path);

    return
        !ascii_stricmp(ext, ".jpg") ||
        !ascii_stricmp(ext, ".jpeg");
}

static bool is_png(
    const char *path
) {
    return !ascii_stricmp(
        file_extension(path),
        ".png"
    );
}

static bool is_bmp(
    const char *path
) {
    return !ascii_stricmp(
        file_extension(path),
        ".bmp"
    );
}

static void mem_writer(
    void *context,
    void *data,
    int size
) {
    MemoryBuffer *buf =
        (MemoryBuffer *)context;

    size_t needed;
    size_t new_capacity;
    unsigned char *new_data;

    if (buf->failed ||
        size <= 0) {
        return;
    }

    needed =
        buf->size +
        (size_t)size;

    if (needed < buf->size) {
        buf->failed = true;
        return;
    }

    if (needed > buf->capacity) {
        new_capacity =
            buf->capacity
                ? buf->capacity
                : 64u * 1024u;

        while (new_capacity < needed) {
            size_t next =
                new_capacity * 2u;

            if (next <= new_capacity) {
                new_capacity = needed;
                break;
            }

            new_capacity = next;
        }

        new_data =
            (unsigned char *)realloc(
                buf->data,
                new_capacity
            );

        if (!new_data) {
            buf->failed = true;
            return;
        }

        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    memcpy(
        buf->data + buf->size,
        data,
        (size_t)size
    );

    buf->size +=
        (size_t)size;
}

static bool encode_image_to_memory(
    const char *path,
    const unsigned char *pixels,
    int width,
    int height,
    int channels,
    MemoryBuffer *out
) {
    int ok = 0;

    memset(
        out,
        0,
        sizeof(*out)
    );

    if (is_png(path)) {
        ok =
            stbi_write_png_to_func(
                mem_writer,
                out,
                width,
                height,
                channels,
                pixels,
                width * channels
            );
    } else if (is_jpeg(path)) {
        ok =
            stbi_write_jpg_to_func(
                mem_writer,
                out,
                width,
                height,
                channels,
                pixels,
                g_cfg.jpeg_quality
            );
    } else if (is_bmp(path)) {
        ok =
            stbi_write_bmp_to_func(
                mem_writer,
                out,
                width,
                height,
                channels,
                pixels
            );
    }

    return
        ok != 0 &&
        !out->failed &&
        out->size > 0;
}

static void process_image(
    const char *path
) {
    unsigned char *source = NULL;
    size_t source_size = 0;

    int source_w = 0;
    int source_h = 0;
    int source_comp = 0;

    int channels;

    unsigned char *decoded = NULL;
    unsigned char *resized = NULL;

    size_t resized_bytes;

    MemoryBuffer encoded;

    char temp_path[XRS_PATH_MAX];
    char backup_path[XRS_PATH_MAX];

    bool temp_written = false;
    bool backup_written = false;

    ++g_stats.images_found;

    if (file_size_exceeds_limit(
            path,
            g_cfg.max_input_mb)) {
        ++g_stats.skipped_too_large;

        xrs_logf(
            "SKIP TOO LARGE | %s\r\n",
            path
        );

        return;
    }

    if (!read_entire_file(
            path,
            &source,
            &source_size,
            g_cfg.max_input_mb)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL READ | %s\r\n",
            path
        );

        return;
    }

    if (source_size > 0x7fffffffu ||
        !stbi_info_from_memory(
            source,
            (int)source_size,
            &source_w,
            &source_h,
            &source_comp)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL IMAGE HEADER | %s\r\n",
            path
        );

        free(source);
        return;
    }

    if (source_w == g_cfg.width &&
        source_h == g_cfg.height) {
        ++g_stats.already_correct;

        xrs_logf(
            "OK %dx%d | %s\r\n",
            source_w,
            source_h,
            path
        );

        free(source);
        return;
    }

    if (g_cfg.dry_run) {
        ++g_stats.resized;

        xrs_logf(
            "DRY RUN %dx%d -> %dx%d | %s\r\n",
            source_w,
            source_h,
            g_cfg.width,
            g_cfg.height,
            path
        );

        free(source);
        return;
    }

    channels =
        is_jpeg(path)
            ? 3
            : 4;

    decoded =
        stbi_load_from_memory(
            source,
            (int)source_size,
            &source_w,
            &source_h,
            &source_comp,
            channels
        );

    if (!decoded) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL DECODE | %s | %s\r\n",
            path,
            stbi_failure_reason()
                ? stbi_failure_reason()
                : "unknown"
        );

        free(source);
        return;
    }

    if ((size_t)g_cfg.width >
            SIZE_MAX /
                (size_t)g_cfg.height ||
        (size_t)g_cfg.width *
                (size_t)g_cfg.height >
            SIZE_MAX /
                (size_t)channels) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL TARGET OVERFLOW | %s\r\n",
            path
        );

        stbi_image_free(decoded);
        free(source);

        return;
    }

    resized_bytes =
        (size_t)g_cfg.width *
        (size_t)g_cfg.height *
        (size_t)channels;

    resized =
        (unsigned char *)malloc(
            resized_bytes
        );

    if (!resized) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL ALLOC RESIZE | %s\r\n",
            path
        );

        stbi_image_free(decoded);
        free(source);

        return;
    }

    if (!stbir_resize_uint8(
            decoded,
            source_w,
            source_h,
            0,
            resized,
            g_cfg.width,
            g_cfg.height,
            0,
            channels)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL RESIZE | %s\r\n",
            path
        );

        free(resized);
        stbi_image_free(decoded);
        free(source);

        return;
    }

    if (!encode_image_to_memory(
            path,
            resized,
            g_cfg.width,
            g_cfg.height,
            channels,
            &encoded)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL ENCODE | %s\r\n",
            path
        );

        free(encoded.data);
        free(resized);
        stbi_image_free(decoded);
        free(source);

        return;
    }

    if (!make_sibling_path(
            path,
            ".xrs.tmp",
            temp_path) ||
        !make_sibling_path(
            path,
            ".xrs.bak",
            backup_path)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL PATH TOO LONG | %s\r\n",
            path
        );

        free(encoded.data);
        free(resized);
        stbi_image_free(decoded);
        free(source);

        return;
    }

    DeleteFileA(temp_path);
    DeleteFileA(backup_path);

    temp_written =
        write_all_file(
            temp_path,
            encoded.data,
            encoded.size
        );

    if (!temp_written) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL TEMP WRITE | %s\r\n",
            path
        );

        goto cleanup;
    }

    backup_written =
        write_all_file(
            backup_path,
            source,
            source_size
        );

    if (!backup_written) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL BACKUP WRITE (original untouched) | %s\r\n",
            path
        );

        goto cleanup;
    }

    if (!write_all_file(
            path,
            encoded.data,
            encoded.size)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL ORIGINAL WRITE | %s | recovery: %s | resized temp: %s\r\n",
            path,
            backup_path,
            temp_path
        );

        temp_written = false;
        backup_written = false;

        goto cleanup;
    }

    ++g_stats.resized;

    xrs_logf(
        "RESIZED %dx%d -> %dx%d | %s\r\n",
        source_w,
        source_h,
        g_cfg.width,
        g_cfg.height,
        path
    );

cleanup:

    if (temp_written) {
        DeleteFileA(temp_path);
    }

    if (backup_written) {
        DeleteFileA(backup_path);
    }

    free(encoded.data);
    free(resized);
    stbi_image_free(decoded);
    free(source);
}

static bool stack_push(
    PathStack *stack,
    const char *path
) {
    char *copy;

    if (stack->count ==
        stack->capacity) {
        size_t new_capacity =
            stack->capacity
                ? stack->capacity * 2u
                : 32u;

        char **new_items =
            (char **)realloc(
                stack->items,
                new_capacity *
                    sizeof(char *)
            );

        if (!new_items) {
            return false;
        }

        stack->items = new_items;
        stack->capacity = new_capacity;
    }

    copy =
        (char *)malloc(
            strlen(path) + 1u
        );

    if (!copy) {
        return false;
    }

    strcpy(copy, path);

    stack->items[
        stack->count++
    ] = copy;

    return true;
}

static char *stack_pop(
    PathStack *stack
) {
    if (!stack->count) {
        return NULL;
    }

    return stack->items[
        --stack->count
    ];
}

static void stack_free(
    PathStack *stack
) {
    while (stack->count) {
        free(
            stack->items[
                --stack->count
            ]
        );
    }

    free(stack->items);

    memset(
        stack,
        0,
        sizeof(*stack)
    );
}

static bool is_drive_root(
    const char *path
) {
    size_t len =
        strlen(path);

    return
        len >= 2 &&
        path[len - 1] == '\\' &&
        path[len - 2] == ':';
}

static void normalize_directory(
    char *path
) {
    size_t i;
    size_t len;

    for (i = 0; path[i]; ++i) {
        if (path[i] == '/') {
            path[i] = '\\';
        }
    }

    len = strlen(path);

    while (len > 0 &&
           path[len - 1] == '\\' &&
           !is_drive_root(path)) {
        path[--len] = '\0';
    }
}

static bool join_path(
    const char *dir,
    const char *name,
    char out[XRS_PATH_MAX]
) {
    size_t d = strlen(dir);
    size_t n = strlen(name);

    bool separator =
        d > 0 &&
        dir[d - 1] != '\\' &&
        dir[d - 1] != '/';

    size_t total =
        d +
        (separator ? 1u : 0u) +
        n +
        1u;

    if (total > XRS_PATH_MAX) {
        return false;
    }

    memcpy(
        out,
        dir,
        d
    );

    if (separator) {
        out[d++] = '\\';
    }

    memcpy(
        out + d,
        name,
        n + 1u
    );

    return true;
}

/*
 * Xbox 360 native kernel paths use the system DOS-device
 * namespace for dashboard/homebrew drive mappings such as
 * Hdd1:. The old version used \??\Hdd1:, which caused
 * NtCreateFile to fail when the app was launched as its own
 * title from Aurora.
 */
static bool to_native_path(
    const char *path,
    char out[XRS_PATH_MAX]
) {
    static const char system_prefix[] =
        "\\System??\\";

    static const char local_prefix[] =
        "\\??\\";

    size_t p = strlen(path);

    if (p >= 10 &&
        !strncmp(
            path,
            system_prefix,
            sizeof(system_prefix) - 1u
        )) {
        if (p + 1u > XRS_PATH_MAX) {
            return false;
        }

        strcpy(out, path);

        return true;
    }

    /*
     * Preserve an explicitly supplied native/local namespace
     * rather than adding another prefix.
     */
    if (p >= 4 &&
        !strncmp(
            path,
            local_prefix,
            sizeof(local_prefix) - 1u
        )) {
        if (p + 1u > XRS_PATH_MAX) {
            return false;
        }

        strcpy(out, path);

        return true;
    }

    {
        size_t pre =
            sizeof(system_prefix) - 1u;

        if (pre + p + 1u >
            XRS_PATH_MAX) {
            return false;
        }

        memcpy(
            out,
            system_prefix,
            pre
        );

        memcpy(
            out + pre,
            path,
            p + 1u
        );
    }

    return true;
}

static bool wide_name_to_ascii(
    const FILE_DIRECTORY_INFORMATION *entry,
    char *out,
    size_t out_size
) {
    size_t count;
    size_t i;

    if (!out_size) {
        return false;
    }

    count =
        entry->FileNameLength /
        sizeof(wchar_t);

    if (count + 1u > out_size) {
        return false;
    }

    for (i = 0;
         i < count;
         ++i) {
        uint32_t wc =
            (uint32_t)entry
                ->FileName[i];

        out[i] =
            wc <= 0xffu
                ? (char)wc
                : '?';
    }

    out[count] = '\0';

    return true;
}

static bool open_directory_native(
    const char *path,
    HANDLE *out_handle
) {
    char native[XRS_PATH_MAX];

    ANSI_STRING name;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;

    NTSTATUS status;

    size_t len;

    *out_handle =
        INVALID_HANDLE_VALUE;

    if (!to_native_path(
            path,
            native)) {
        xrs_logf(
            "OPEN DIR PATH CONVERSION FAILED | %s\r\n",
            path
        );

        return false;
    }

    len = strlen(native);

    if (len > 0xffffu) {
        xrs_logf(
            "OPEN DIR PATH TOO LONG | %s\r\n",
            native
        );

        return false;
    }

    name.Length =
        (uint16_t)len;

    name.MaximumLength =
        (uint16_t)(len + 1u);

    name.Buffer = native;

    attrs.root_directory = 0;
    attrs.name_ptr = &name;
    attrs.attributes =
        OBJ_CASE_INSENSITIVE;

    memset(
        &iosb,
        0,
        sizeof(iosb)
    );

    status =
        NtCreateFile(
            out_handle,
            FILE_LIST_DIRECTORY |
                SYNCHRONIZE,
            &attrs,
            &iosb,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            FILE_OPEN,
            FILE_DIRECTORY_FILE |
                FILE_SYNCHRONOUS_IO_NONALERT
        );

    if (FAILED(status) ||
        *out_handle ==
            INVALID_HANDLE_VALUE) {
        xrs_logf(
            "OPEN DIR NATIVE status=0x%08X | %s -> %s\r\n",
            (unsigned)status,
            path,
            native
        );

        return false;
    }

    return true;
}

static void scan_directory(
    const char *dir,
    PathStack *stack
) {
    HANDLE handle;
    unsigned char *buffer;
    bool restart = true;

    ++g_stats.directories;

    xrs_logf(
        "DIR | %s\r\n",
        dir
    );

    if (!open_directory_native(
            dir,
            &handle)) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL OPEN DIR | %s\r\n",
            dir
        );

        return;
    }

    buffer =
        (unsigned char *)malloc(
            XRS_DIR_BUFFER_SIZE
        );

    if (!buffer) {
        ++g_stats.failed;

        xrs_logf(
            "FAIL ALLOC DIR BUFFER | %s\r\n",
            dir
        );

        NtClose(handle);

        return;
    }

    for (;;) {
        IO_STATUS_BLOCK iosb;
        NTSTATUS status;

        uint32_t offset = 0;

        memset(
            &iosb,
            0,
            sizeof(iosb)
        );

        status =
            NtQueryDirectoryFile(
                handle,
                NULL,
                NULL,
                NULL,
                &iosb,
                (FILE_DIRECTORY_INFORMATION *)
                    buffer,
                XRS_DIR_BUFFER_SIZE,
                NULL,
                restart ? 1u : 0u
            );

        restart = false;

        if (status ==
                STATUS_NO_MORE_FILES ||
            iosb.Information == 0) {
            break;
        }

        if (FAILED(status) &&
            status !=
                STATUS_BUFFER_OVERFLOW) {
            ++g_stats.failed;

            xrs_logf(
                "FAIL QUERY DIR status=0x%08X | %s\r\n",
                (unsigned)status,
                dir
            );

            break;
        }

        while (offset <
               iosb.Information) {
            FILE_DIRECTORY_INFORMATION *entry =
                (FILE_DIRECTORY_INFORMATION *)
                    (buffer + offset);

            char name[XRS_PATH_MAX];
            char full[XRS_PATH_MAX];

            if (!wide_name_to_ascii(
                    entry,
                    name,
                    sizeof(name))) {
                ++g_stats.failed;

                xrs_logf(
                    "FAIL DIRECTORY NAME | %s\r\n",
                    dir
                );
            } else if (
                strcmp(name, ".") &&
                strcmp(name, "..")) {

                if (!join_path(
                        dir,
                        name,
                        full)) {
                    ++g_stats.failed;

                    xrs_logf(
                        "FAIL PATH TOO LONG | %s + %s\r\n",
                        dir,
                        name
                    );
                } else if (
                    entry->FileAttributes &
                    FILE_ATTRIBUTE_DIRECTORY) {

                    if (g_cfg.recursive &&
                        !stack_push(
                            stack,
                            full)) {
                        ++g_stats.failed;

                        xrs_logf(
                            "FAIL QUEUE DIR | %s\r\n",
                            full
                        );
                    }

                } else if (
                    supported_image(full)) {

                    process_image(full);
                }
            }

            if (entry->NextEntryOffset ==
                0) {
                break;
            }

            if (entry->NextEntryOffset >
                iosb.Information -
                    offset) {
                ++g_stats.failed;

                xrs_logf(
                    "FAIL CORRUPT DIRECTORY BUFFER | %s\r\n",
                    dir
                );

                offset =
                    iosb.Information;

                break;
            }

            offset +=
                entry->NextEntryOffset;
        }
    }

    free(buffer);
    NtClose(handle);
}

static void set_defaults(
    Config *cfg
) {
    memset(
        cfg,
        0,
        sizeof(*cfg)
    );

    strcpy(
        cfg->path,
        "Hdd1:\\Images\\"
    );

    cfg->width = 900;
    cfg->height = 600;
    cfg->recursive = true;
    cfg->jpeg_quality = 90;
    cfg->max_input_mb =
        XRS_DEFAULT_MAX_INPUT_MB;
    cfg->dry_run = false;
}

static bool load_config_file(
    const char *path,
    Config *cfg
) {
    unsigned char *data = NULL;

    size_t size = 0;

    char *cursor;
    char *end;

    if (!read_entire_file(
            path,
            &data,
            &size,
            1u)) {
        return false;
    }

    {
        unsigned char *grown =
            (unsigned char *)realloc(
                data,
                size + 1u
            );

        if (!grown) {
            free(data);
            return false;
        }

        data = grown;
    }

    data[size] = '\0';

    cursor = (char *)data;
    end = cursor + size;

    while (cursor < end) {
        char *line = cursor;
        char *eq;
        char *key;
        char *value;

        while (cursor < end &&
               *cursor != '\n' &&
               *cursor != '\r') {
            ++cursor;
        }

        if (cursor < end) {
            *cursor++ = '\0';
        }

        while (cursor < end &&
               (*cursor == '\n' ||
                *cursor == '\r')) {
            ++cursor;
        }

        line = trim(line);

        if (!*line ||
            *line == ';' ||
            *line == '#' ||
            *line == '[') {
            continue;
        }

        eq = strchr(line, '=');

        if (!eq) {
            continue;
        }

        *eq = '\0';

        key = trim(line);
        value = trim(eq + 1);

        if (!ascii_stricmp(
                key,
                "Path")) {

            if (strlen(value) + 1u <=
                sizeof(cfg->path)) {
                strcpy(
                    cfg->path,
                    value
                );
            }

        } else if (
            !ascii_stricmp(
                key,
                "Width")) {

            cfg->width =
                atoi(value);

        } else if (
            !ascii_stricmp(
                key,
                "Height")) {

            cfg->height =
                atoi(value);

        } else if (
            !ascii_stricmp(
                key,
                "Recursive")) {

            cfg->recursive =
                parse_bool(
                    value,
                    cfg->recursive
                );

        } else if (
            !ascii_stricmp(
                key,
                "JpegQuality")) {

            cfg->jpeg_quality =
                atoi(value);

        } else if (
            !ascii_stricmp(
                key,
                "MaxInputMB")) {

            int mb =
                atoi(value);

            if (mb >= 0) {
                cfg->max_input_mb =
                    (uint32_t)mb;
            }

        } else if (
            !ascii_stricmp(
                key,
                "DryRun")) {

            cfg->dry_run =
                parse_bool(
                    value,
                    cfg->dry_run
                );
        }
    }

    free(data);

    return true;
}

static bool load_config(
    Config *cfg,
    char loaded_from[XRS_PATH_MAX]
) {
    static const char *candidates[] = {
        "game:\\config.ini",
        "config.ini"
    };

    size_t i;

    set_defaults(cfg);

    for (i = 0;
         i <
            sizeof(candidates) /
                sizeof(candidates[0]);
         ++i) {

        if (load_config_file(
                candidates[i],
                cfg)) {

            strncpy(
                loaded_from,
                candidates[i],
                XRS_PATH_MAX - 1u
            );

            loaded_from[
                XRS_PATH_MAX - 1u
            ] = '\0';

            return true;
        }
    }

    strcpy(
        loaded_from,
        "<defaults>"
    );

    return false;
}

static bool validate_config(
    Config *cfg
) {
    normalize_directory(
        cfg->path
    );

    if (!cfg->path[0]) {
        return false;
    }

    if (cfg->width <= 0 ||
        cfg->height <= 0 ||
        cfg->width >
            XRS_MAX_TARGET_DIMENSION ||
        cfg->height >
            XRS_MAX_TARGET_DIMENSION) {
        return false;
    }

    if (cfg->jpeg_quality < 1) {
        cfg->jpeg_quality = 1;
    }

    if (cfg->jpeg_quality > 100) {
        cfg->jpeg_quality = 100;
    }

    return true;
}

static void open_log(void) {
    g_log =
        xrs_open_file(
            "game:\\resize.log",
            GENERIC_WRITE,
            CREATE_ALWAYS
        );

    if (g_log ==
        INVALID_HANDLE_VALUE) {
        g_log =
            xrs_open_file(
                "resize.log",
                GENERIC_WRITE,
                CREATE_ALWAYS
            );
    }
}

int main(void) {
    char config_source[
        XRS_PATH_MAX
    ];

    PathStack stack;
    char *dir;

    memset(
        &stack,
        0,
        sizeof(stack)
    );

    memset(
        &g_stats,
        0,
        sizeof(g_stats)
    );

    load_config(
        &g_cfg,
        config_source
    );

    open_log();

    xrs_logf(
        "Aurora Image Resizer %s\r\n",
        XRS_VERSION
    );

    xrs_logf(
        "Config: %s\r\n",
        config_source
    );

    if (!validate_config(
            &g_cfg)) {

        log_raw(
            "FATAL: invalid configuration.\r\n"
        );

        if (g_log !=
            INVALID_HANDLE_VALUE) {
            CloseHandle(g_log);
        }

        XamLoaderTerminateTitle();

        return 1;
    }

    xrs_logf(
        "Root: %s\r\n",
        g_cfg.path
    );

    xrs_logf(
        "Target: %dx%d\r\n",
        g_cfg.width,
        g_cfg.height
    );

    xrs_logf(
        "Recursive: %s\r\n",
        g_cfg.recursive
            ? "true"
            : "false"
    );

    xrs_logf(
        "JPEG quality: %d\r\n",
        g_cfg.jpeg_quality
    );

    xrs_logf(
        "Max input MB: %u\r\n",
        (unsigned)
            g_cfg.max_input_mb
    );

    xrs_logf(
        "Dry run: %s\r\n\r\n",
        g_cfg.dry_run
            ? "true"
            : "false"
    );

    if (!stack_push(
            &stack,
            g_cfg.path)) {

        log_raw(
            "FATAL: unable to queue root directory.\r\n"
        );

        if (g_log !=
            INVALID_HANDLE_VALUE) {
            CloseHandle(g_log);
        }

        XamLoaderTerminateTitle();

        return 1;
    }

    while ((dir =
                stack_pop(&stack)) !=
           NULL) {

        scan_directory(
            dir,
            &stack
        );

        free(dir);
    }

    stack_free(&stack);

    log_raw(
        "\r\n--- SUMMARY ---\r\n"
    );

    xrs_logf(
        "Directories: %u\r\n",
        (unsigned)
            g_stats.directories
    );

    xrs_logf(
        "Images found: %u\r\n",
        (unsigned)
            g_stats.images_found
    );

    xrs_logf(
        "Already correct: %u\r\n",
        (unsigned)
            g_stats.already_correct
    );

    xrs_logf(
        "Resized: %u\r\n",
        (unsigned)
            g_stats.resized
    );

    xrs_logf(
        "Too large: %u\r\n",
        (unsigned)
            g_stats.skipped_too_large
    );

    xrs_logf(
        "Failed: %u\r\n",
        (unsigned)
            g_stats.failed
    );

    if (g_log !=
        INVALID_HANDLE_VALUE) {

        CloseHandle(g_log);

        g_log =
            INVALID_HANDLE_VALUE;
    }

    XamLoaderTerminateTitle();

    return 0;
}
