#include "sentry_unix_modulefinder_darwin.h"
#include "../sentry_sync.h"
#include "../sentry_value.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/arch.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach_init.h>
#include <mach/mach_traps.h>
#include <mach/task.h>
#include <mach/task_info.h>

#if UINTPTR_MAX == 0xffffffffULL
typedef mach_header platform_mach_header;
typedef segment_command mach_segment_command_type;
#    define MACHO_MAGIC_NUMBER MH_MAGIC
#    define CMD_SEGMENT LC_SEGMENT
#    define seg_size uint32_t
#else
typedef struct mach_header_64 platform_mach_header;
typedef struct segment_command_64 mach_segment_command_type;
#    define MACHO_MAGIC_NUMBER MH_MAGIC_64
#    define CMD_SEGMENT LC_SEGMENT_64
#    define seg_size uint64_t
#endif

static bool g_initialized = false;
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
static sentry_value_t g_modules;

static void
add_image(const struct mach_header *mh, intptr_t vmaddr_slide)
{
    sentry__mutex_lock(&g_mutex);

    const platform_mach_header *header = (const platform_mach_header *)mh;
    Dl_info info;
    if (!dladdr(header, &info)) {
        return;
    }

    sentry_value_t modules = g_modules;
    sentry_value_t new_modules = sentry_value_is_null(modules)
        ? sentry_value_new_list()
        : sentry__value_clone(modules);
    sentry_value_t module = sentry_value_new_object();
    sentry_value_set_by_key(
        module, "code_file", sentry_value_new_string(info.dli_fname));
    sentry_value_set_by_key(
        module, "image_addr", sentry__value_new_addr((uint64_t)info.dli_fbase));

    const struct load_command *cmd = (const struct load_command *)(header + 1);
    bool has_size = false;
    bool has_uuid = false;

    for (size_t i = 0; cmd && (i < header->ncmds) && (!has_uuid || !has_size);
         ++i,
                cmd
         = (const struct load_command *)((const char *)cmd + cmd->cmdsize)) {
        if (cmd->cmd == CMD_SEGMENT) {
            const mach_segment_command_type *seg
                = (const mach_segment_command_type *)cmd;

            if (strcmp(seg->segname, "__TEXT") == 0) {
                sentry_value_set_by_key(module, "image_size",
                    sentry_value_new_int32((uint32_t)seg->vmsize));
                has_size = true;
            }
        } else if (cmd->cmd == LC_UUID) {
            const struct uuid_command *ucmd = (const struct uuid_command *)cmd;
            sentry_uuid_t uuid
                = sentry_uuid_from_bytes((const char *)ucmd->uuid);
            sentry_value_set_by_key(
                module, "debug_id", sentry__value_new_uuid(&uuid));
            has_uuid = true;
        }
    }

    sentry_value_set_by_key(module, "type", sentry_value_new_string("macho"));
    sentry_value_append(new_modules, module);
    sentry_value_freeze(new_modules);
    sentry_value_decref(g_modules);
    g_modules = new_modules;

    sentry__mutex_unlock(&g_mutex);
}

static void
remove_image(const struct mach_header *mh, intptr_t vmaddr_slide)
{
    sentry__mutex_lock(&g_mutex);

    if (sentry_value_is_null(g_modules)
        || sentry_value_get_length(g_modules) == 0) {
        goto done;
    }

    const platform_mach_header *header = (const platform_mach_header *)(mh);
    Dl_info info;
    if (!dladdr(header, &info)) {
        return;
    }

    char ref_addr[100];
    snprintf(ref_addr, sizeof(ref_addr), "0x%llx", (long long)info.dli_fbase);
    sentry_value_t new_modules = sentry_value_new_list();

    for (size_t i = 0; i < sentry_value_get_length(g_modules); i++) {
        sentry_value_t module = sentry_value_get_by_index(g_modules, i);
        const char *addr = sentry_value_as_string(
            sentry_value_get_by_key(module, "image_addr"));
        if (!addr || strcmp(addr, ref_addr) != 0) {
            sentry_value_incref(module);
            sentry_value_append(new_modules, module);
        }
    }

    sentry_value_decref(g_modules);
    sentry_value_freeze(new_modules);
    g_modules = new_modules;

done:
    sentry__mutex_unlock(&g_mutex);
}

sentry_value_t
sentry__darwin_modules_get_list(void)
{
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        _dyld_register_func_for_add_image(add_image);
        _dyld_register_func_for_remove_image(remove_image);
        g_initialized = true;
    }
    sentry__mutex_unlock(&g_mutex);
    return g_modules;
}
