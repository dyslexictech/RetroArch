/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2017 - Brad Parker
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "menu_animation.h"
#include "menu_driver.h"
#include "menu_cbs.h"
#include "menu_display.h"
#include "menu_event.h"
#include "widgets/menu_dialog.h"
#include "widgets/menu_list.h"
#include "menu_shader.h"

#include "../content.h"
#include "../configuration.h"
#include "../dynamic.h"
#include "../driver.h"
#include "../retroarch.h"
#include "../defaults.h"
#include "../frontend/frontend.h"
#include "../list_special.h"
#include "../tasks/tasks_internal.h"
#include "../ui/ui_companion_driver.h"
#include "../verbosity.h"

#define SCROLL_INDEX_SIZE (2 * (26 + 2) + 1)

static const menu_ctx_driver_t *menu_ctx_drivers[] = {
#if defined(HAVE_XUI)
   &menu_ctx_xui,
#endif
#if defined(HAVE_MATERIALUI)
   &menu_ctx_mui,
#endif
#if defined(HAVE_NUKLEAR)
   &menu_ctx_nuklear,
#endif
#if defined(HAVE_XMB)
   &menu_ctx_xmb,
#endif
#if defined(HAVE_RGUI)
   &menu_ctx_rgui,
#endif
#if defined(HAVE_ZARCH)
   &menu_ctx_zarch,
#endif
   &menu_ctx_null,
   NULL
};

static struct retro_system_info menu_driver_system;
static bool menu_driver_pending_quick_menu      = false;
static bool menu_driver_prevent_populate        = false;
static bool menu_driver_load_no_content         = false;
static bool menu_driver_alive                   = false;
static bool menu_driver_toggled                 = false;
static bool menu_driver_data_own                = false;
static bool menu_driver_pending_quit            = false;
static bool menu_driver_pending_shutdown        = false;
static bool menu_driver_is_binding              = false;
static playlist_t *menu_driver_playlist         = NULL;
static menu_handle_t *menu_driver_data          = NULL;
static const menu_ctx_driver_t *menu_driver_ctx = NULL;
static void *menu_userdata                      = NULL;

/* Quick jumping indices with L/R.
 * Rebuilt when parsing directory. */
static size_t scroll_index_list[SCROLL_INDEX_SIZE];
static unsigned scroll_index_size               = 0;
static unsigned scroll_acceleration             = 0;
static size_t menu_driver_selection_ptr         = 0;

bool menu_driver_is_binding_state(void)
{
   return menu_driver_is_binding;
}

void menu_driver_set_binding_state(bool on)
{
   menu_driver_is_binding = on;
}

/**
 * menu_driver_find_handle:
 * @idx              : index of driver to get handle to.
 *
 * Returns: handle to menu driver at index. Can be NULL
 * if nothing found.
 **/
const void *menu_driver_find_handle(int idx)
{
   const void *drv = menu_ctx_drivers[idx];
   if (!drv)
      return NULL;
   return drv;
}

/**
 * menu_driver_find_ident:
 * @idx              : index of driver to get handle to.
 *
 * Returns: Human-readable identifier of menu driver at index.
 * Can be NULL if nothing found.
 **/
const char *menu_driver_find_ident(int idx)
{
   const menu_ctx_driver_t *drv = menu_ctx_drivers[idx];
   if (!drv)
      return NULL;
   return drv->ident;
}

/**
 * config_get_menu_driver_options:
 *
 * Get an enumerated list of all menu driver names,
 * separated by '|'.
 *
 * Returns: string listing of all menu driver names,
 * separated by '|'.
 **/
const char *config_get_menu_driver_options(void)
{
   return char_list_new_special(STRING_LIST_MENU_DRIVERS, NULL);
}

#ifdef HAVE_COMPRESSION
static void bundle_decompressed(void *task_data,
      void *user_data, const char *err)
{
   settings_t      *settings   = config_get_ptr();
   decompress_task_data_t *dec = (decompress_task_data_t*)task_data;

   if (dec && !err)
      command_event(CMD_EVENT_REINIT, NULL);

   if (err)
      RARCH_ERR("%s", err);

   if (dec)
   {
      /* delete bundle? */
      free(dec->source_file);
      free(dec);
   }

   settings->uints.bundle_assets_extract_last_version =
      settings->uints.bundle_assets_extract_version_current;

   configuration_set_bool(settings, settings->bools.bundle_finished, true);

   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
}
#endif

/**
 * menu_init:
 * @data                     : Menu context handle.
 *
 * Create and initialize menu handle.
 *
 * Returns: menu handle on success, otherwise NULL.
 **/
static bool menu_init(menu_handle_t *menu_data)
{
   settings_t *settings        = config_get_ptr();

   if (!menu_entries_ctl(MENU_ENTRIES_CTL_INIT, NULL))
      return false;

   if (settings->bools.menu_show_start_screen)
   {
      menu_dialog_push_pending(true, MENU_DIALOG_WELCOME);

      configuration_set_bool(settings,
            settings->bools.menu_show_start_screen, false);

      if (settings->bools.config_save_on_exit)
         command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   }

   if (      settings->bools.bundle_assets_extract_enable
         && !string_is_empty(settings->arrays.bundle_assets_src)
         && !string_is_empty(settings->arrays.bundle_assets_dst)
#ifdef IOS
         && menu_dialog_is_push_pending()
#else
         && (settings->uints.bundle_assets_extract_version_current
            != settings->uints.bundle_assets_extract_last_version)
#endif
      )
   {
      menu_dialog_push_pending(true, MENU_DIALOG_HELP_EXTRACT);
#ifdef HAVE_COMPRESSION
      task_push_decompress(settings->arrays.bundle_assets_src,
            settings->arrays.bundle_assets_dst,
            NULL, settings->arrays.bundle_assets_dst_subdir,
            NULL, bundle_decompressed, NULL);
#endif
   }

   menu_shader_manager_init();

   if (!menu_display_init())
      return false;

   return true;
}

static void menu_input_key_event(bool down, unsigned keycode,
      uint32_t character, uint16_t mod)
{
   (void)down;
   (void)keycode;
   (void)mod;

#if 0
   RARCH_LOG("down: %d, keycode: %d, mod: %d, character: %d\n", down, keycode, mod, character);
#endif

   menu_event_kb_set(down, (enum retro_key)keycode);
}

static void menu_driver_toggle(bool on)
{
   retro_keyboard_event_t *key_event          = NULL;
   retro_keyboard_event_t *frontend_key_event = NULL;
   settings_t                 *settings       = config_get_ptr();
   bool pause_libretro                        = settings ? settings->bools.menu_pause_libretro : false;

   menu_driver_toggled = on;

   if (!on)
      menu_display_toggle_set_reason(MENU_TOGGLE_REASON_NONE);

   if (menu_driver_ctx && menu_driver_ctx->toggle)
      menu_driver_ctx->toggle(menu_userdata, on);

   if (on)
      menu_driver_alive = true;
   else
      menu_driver_alive = false;

   rarch_ctl(RARCH_CTL_FRONTEND_KEY_EVENT_GET, &frontend_key_event);
   rarch_ctl(RARCH_CTL_KEY_EVENT_GET,          &key_event);

   if (menu_driver_alive)
   {
      bool refresh = false;
      menu_entries_ctl(MENU_ENTRIES_CTL_SET_REFRESH, &refresh);

      /* Menu should always run with vsync on. */
      command_event(CMD_EVENT_VIDEO_SET_BLOCKING_STATE, NULL);
      /* Stop all rumbling before entering the menu. */
      command_event(CMD_EVENT_RUMBLE_STOP, NULL);

      if (pause_libretro)
         command_event(CMD_EVENT_AUDIO_STOP, NULL);

      /* Override keyboard callback to redirect to menu instead.
       * We'll use this later for something ... */

      if (key_event && frontend_key_event)
      {
         *frontend_key_event        = *key_event;
         *key_event                 = menu_input_key_event;

         rarch_ctl(RARCH_CTL_SET_FRAME_TIME_LAST, NULL);
      }
   }
   else
   {
      if (!rarch_ctl(RARCH_CTL_IS_SHUTDOWN, NULL))
         driver_set_nonblock_state();

      if (pause_libretro)
         command_event(CMD_EVENT_AUDIO_START, NULL);

      /* Restore libretro keyboard callback. */
      if (key_event && frontend_key_event)
         *key_event = *frontend_key_event;
   }
}

const char *menu_driver_ident(void)
{
   if (!menu_driver_alive)
      return NULL;
   if (!menu_driver_ctx || !menu_driver_ctx->ident)
      return NULL;
  return menu_driver_ctx->ident;
}

void menu_driver_frame(video_frame_info_t *video_info)
{
   if (menu_driver_alive && menu_driver_ctx->frame)
      menu_driver_ctx->frame(menu_userdata, video_info);
}

bool menu_driver_render(bool is_idle, bool rarch_is_inited,
      bool rarch_is_dummy_core)
{
   if (!menu_driver_data)
      return false;

   if (BIT64_GET(menu_driver_data->state, MENU_STATE_RENDER_FRAMEBUFFER)
         != BIT64_GET(menu_driver_data->state, MENU_STATE_RENDER_MESSAGEBOX))
      BIT64_SET(menu_driver_data->state, MENU_STATE_RENDER_FRAMEBUFFER);

   if (BIT64_GET(menu_driver_data->state, MENU_STATE_RENDER_FRAMEBUFFER))
      menu_display_set_framebuffer_dirty_flag();

   if (BIT64_GET(menu_driver_data->state, MENU_STATE_RENDER_MESSAGEBOX)
         && !string_is_empty(menu_driver_data->menu_state.msg))
   {
      if (menu_driver_ctx->render_messagebox)
         menu_driver_ctx->render_messagebox(menu_userdata,
               menu_driver_data->menu_state.msg);

      if (ui_companion_is_on_foreground())
      {
         const ui_companion_driver_t *ui = ui_companion_get_ptr();
         if (ui->render_messagebox)
            ui->render_messagebox(menu_driver_data->menu_state.msg);
      }
   }

   if (BIT64_GET(menu_driver_data->state, MENU_STATE_BLIT))
   {
      settings_t *settings = config_get_ptr();
      menu_animation_update_time(settings->bools.menu_timedate_enable);

      if (menu_driver_ctx->render)
         menu_driver_ctx->render(menu_userdata, is_idle);
   }

   if (menu_driver_alive && !is_idle)
      menu_display_libretro(is_idle, rarch_is_inited, rarch_is_dummy_core);

   if (menu_driver_ctx->set_texture)
      menu_driver_ctx->set_texture();

   menu_driver_data->state               = 0;

   return true;
}

bool menu_driver_is_alive(void)
{
   return menu_driver_alive;
}

bool menu_driver_is_texture_set(void)
{
   if (!menu_driver_ctx)
      return false;
   return menu_driver_ctx->set_texture;
}

bool menu_driver_iterate(menu_ctx_iterate_t *iterate)
{
   if (menu_driver_pending_quick_menu)
   {
      menu_driver_pending_quick_menu = false;
      menu_entries_flush_stack(NULL, MENU_SETTINGS);
      menu_display_set_msg_force(true);

      generic_action_ok_displaylist_push("", NULL,
            "", 0, 0, 0, ACTION_OK_DL_CONTENT_SETTINGS);

      if (menu_driver_pending_quit)
      {
         menu_driver_pending_quit     = false;
         return false;
      }

      return true;
   }

   if (menu_driver_pending_quit)
   {
      menu_driver_pending_quit     = false;
      return false;
   }

   if (menu_driver_pending_shutdown)
   {
      menu_driver_pending_shutdown = false;
      if (!command_event(CMD_EVENT_QUIT, NULL))
         return false;
      return true;
   }

   if (!menu_driver_ctx || !menu_driver_ctx->iterate)
      return false;

   if (menu_driver_ctx->iterate(menu_driver_data,
            menu_userdata, iterate->action) == -1)
      return false;

   return true;
}

bool menu_driver_list_clear(void *data)
{
   file_list_t *list = (file_list_t*)data;
   if (!list)
      return false;
   if (menu_driver_ctx->list_clear)
      menu_driver_ctx->list_clear(list);
   return true;
}

static bool menu_driver_init_internal(bool video_is_threaded)
{
   settings_t *settings           = config_get_ptr();

   menu_driver_data               = (menu_handle_t*)
      menu_driver_ctx->init(&menu_userdata, video_is_threaded);

   if (!menu_driver_data || !menu_init(menu_driver_data))
   {
      retroarch_fail(1, "init_menu()");
      return false;
   }

   strlcpy(settings->arrays.menu_driver, menu_driver_ctx->ident,
         sizeof(settings->arrays.menu_driver));

   if (menu_driver_ctx->lists_init)
   {
      if (!menu_driver_ctx->lists_init(menu_driver_data))
      {
         retroarch_fail(1, "init_menu()");
         return false;
      }
   }

   return true;
}

bool menu_driver_init(bool video_is_threaded)
{
   struct retro_system_info *info = &menu_driver_system;

   if (info)
   {
      /* Update menu state which depends on config. */
      command_event(CMD_EVENT_CORE_INFO_INIT, NULL);
      command_event(CMD_EVENT_LOAD_CORE_PERSIST, NULL);
   }

   if (menu_driver_data || menu_driver_init_internal(video_is_threaded))
   {
      if (menu_driver_ctx && menu_driver_ctx->context_reset)
         menu_driver_ctx->context_reset(menu_userdata, video_is_threaded);
   }
   return false;
}

void menu_driver_navigation_set(bool scroll)
{
   if (menu_driver_ctx->navigation_set)
      menu_driver_ctx->navigation_set(menu_userdata, scroll);
}

void menu_driver_populate_entries(menu_displaylist_info_t *info)
{
   if (menu_driver_ctx->populate_entries)
      menu_driver_ctx->populate_entries(
            menu_userdata, info->path,
            info->label, info->type);
}

bool menu_driver_load_image(menu_ctx_load_image_t *load_image_info)
{
   if (menu_driver_ctx && menu_driver_ctx->load_image)
      return menu_driver_ctx->load_image(menu_userdata,
            load_image_info->data, load_image_info->type);
   return false;
}

bool menu_driver_push_list(menu_ctx_displaylist_t *disp_list)
{
   if (menu_driver_ctx->list_push)
      if (menu_driver_ctx->list_push(menu_driver_data,
               menu_userdata, disp_list->info, disp_list->type) == 0)
         return true;
   return false;
}

void menu_driver_set_thumbnail_system(char *s, size_t len)
{
   if (menu_driver_ctx && menu_driver_ctx->set_thumbnail_system)
      menu_driver_ctx->set_thumbnail_system(menu_userdata, s, len);
}

void menu_driver_set_thumbnail_content(char *s, size_t len)
{
   if (menu_driver_ctx && menu_driver_ctx->set_thumbnail_content)
      menu_driver_ctx->set_thumbnail_content(menu_userdata, s, len);
}

bool menu_driver_ctl(enum rarch_menu_ctl_state state, void *data)
{
   switch (state)
   {
      case RARCH_MENU_CTL_DRIVER_DATA_GET:
         {
            menu_handle_t **driver_data = (menu_handle_t**)data;
            if (!driver_data)
               return false;
            *driver_data = menu_driver_data;
         }
         break;
      case RARCH_MENU_CTL_SET_PENDING_QUICK_MENU:
         menu_driver_pending_quick_menu = true;
         break;
      case RARCH_MENU_CTL_SET_PENDING_QUIT:
         menu_driver_pending_quit     = true;
         break;
      case RARCH_MENU_CTL_SET_PENDING_SHUTDOWN:
         menu_driver_pending_shutdown = true;
         break;
      case RARCH_MENU_CTL_DESTROY:
         menu_driver_pending_quick_menu = false;
         menu_driver_pending_quit       = false;
         menu_driver_pending_shutdown   = false;
         menu_driver_prevent_populate   = false;
         menu_driver_load_no_content    = false;
         menu_driver_alive              = false;
         menu_driver_data_own           = false;
         menu_driver_ctx                = NULL;
         menu_userdata                  = NULL;
         break;
      case RARCH_MENU_CTL_PLAYLIST_FREE:
         if (menu_driver_playlist)
            playlist_free(menu_driver_playlist);
         menu_driver_playlist = NULL;
         break;
      case RARCH_MENU_CTL_FIND_DRIVER:
         {
            int i;
            driver_ctx_info_t drv;
            settings_t *settings = config_get_ptr();

            drv.label = "menu_driver";
            drv.s     = settings->arrays.menu_driver;

            driver_ctl(RARCH_DRIVER_CTL_FIND_INDEX, &drv);

            i = (int)drv.len;

            if (i >= 0)
               menu_driver_ctx = (const menu_ctx_driver_t*)
                  menu_driver_find_handle(i);
            else
            {
               unsigned d;
               RARCH_WARN("Couldn't find any menu driver named \"%s\"\n",
                     settings->arrays.menu_driver);
               RARCH_LOG_OUTPUT("Available menu drivers are:\n");
               for (d = 0; menu_driver_find_handle(d); d++)
                  RARCH_LOG_OUTPUT("\t%s\n", menu_driver_find_ident(d));
               RARCH_WARN("Going to default to first menu driver...\n");

               menu_driver_ctx = (const menu_ctx_driver_t*)
                  menu_driver_find_handle(0);

               if (!menu_driver_ctx)
               {
                  retroarch_fail(1, "find_menu_driver()");
                  return false;
               }
            }
         }
         break;
      case RARCH_MENU_CTL_PLAYLIST_INIT:
         {
            const char *path = (const char*)data;
            if (string_is_empty(path))
               return false;
            menu_driver_playlist  = playlist_init(path,
                  COLLECTION_SIZE);
         }
         break;
      case RARCH_MENU_CTL_PLAYLIST_GET:
         {
            playlist_t **playlist = (playlist_t**)data;
            if (!playlist)
               return false;
            *playlist = menu_driver_playlist;
         }
         break;
      case RARCH_MENU_CTL_SYSTEM_INFO_GET:
         {
            struct retro_system_info **system =
               (struct retro_system_info**)data;
            if (!system)
               return false;
            *system = &menu_driver_system;
         }
         break;
      case RARCH_MENU_CTL_SYSTEM_INFO_DEINIT:
#ifndef HAVE_DYNAMIC
         if (frontend_driver_has_fork())
#endif
         {
            libretro_free_system_info(&menu_driver_system);
            memset(&menu_driver_system, 0, sizeof(struct retro_system_info));
         }
         break;
      case RARCH_MENU_CTL_SET_PREVENT_POPULATE:
         menu_driver_prevent_populate = true;
         break;
      case RARCH_MENU_CTL_UNSET_PREVENT_POPULATE:
         menu_driver_prevent_populate = false;
         break;
      case RARCH_MENU_CTL_IS_PREVENT_POPULATE:
         return menu_driver_prevent_populate;
      case RARCH_MENU_CTL_IS_TOGGLE:
         return menu_driver_toggled;
      case RARCH_MENU_CTL_SET_TOGGLE:
         menu_driver_toggle(true);
         break;
      case RARCH_MENU_CTL_UNSET_TOGGLE:
         menu_driver_toggle(false);
         break;
      case RARCH_MENU_CTL_SET_OWN_DRIVER:
         menu_driver_data_own = true;
         break;
      case RARCH_MENU_CTL_UNSET_OWN_DRIVER:
         menu_driver_data_own = false;
         break;
      case RARCH_MENU_CTL_OWNS_DRIVER:
         return menu_driver_data_own;
      case RARCH_MENU_CTL_DEINIT:
         if (menu_driver_ctx && menu_driver_ctx->context_destroy)
            menu_driver_ctx->context_destroy(menu_userdata);

         if (menu_driver_data_own)
            return true;

         menu_driver_ctl(RARCH_MENU_CTL_PLAYLIST_FREE, NULL);
         menu_shader_manager_free();

         if (menu_driver_data)
         {
            unsigned i;

            scroll_acceleration       = 0;
            menu_driver_selection_ptr = 0;
            scroll_index_size         = 0;

            for (i = 0; i < SCROLL_INDEX_SIZE; i++)
               scroll_index_list[i] = 0;

            menu_input_ctl(MENU_INPUT_CTL_DEINIT, NULL);

            if (menu_driver_ctx && menu_driver_ctx->free)
               menu_driver_ctx->free(menu_userdata);

            if (menu_userdata)
               free(menu_userdata);
            menu_userdata = NULL;

            menu_driver_ctl(RARCH_MENU_CTL_SYSTEM_INFO_DEINIT, NULL);
            menu_display_deinit();
            menu_entries_ctl(MENU_ENTRIES_CTL_DEINIT, NULL);

            command_event(CMD_EVENT_HISTORY_DEINIT, NULL);

            menu_dialog_reset();

            free(menu_driver_data);
         }
         menu_driver_data = NULL;
         break;
      case RARCH_MENU_CTL_LOAD_NO_CONTENT_GET:
         {
            bool **ptr = (bool**)data;
            if (!ptr)
               return false;
            *ptr = (bool*)&menu_driver_load_no_content;
         }
         break;
      case RARCH_MENU_CTL_HAS_LOAD_NO_CONTENT:
         return menu_driver_load_no_content;
      case RARCH_MENU_CTL_SET_LOAD_NO_CONTENT:
         menu_driver_load_no_content = true;
         break;
      case RARCH_MENU_CTL_UNSET_LOAD_NO_CONTENT:
         menu_driver_load_no_content = false;
         break;
      case RARCH_MENU_CTL_LIST_GET_ENTRY:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;

            if (!menu_driver_ctx || !menu_driver_ctx->list_get_entry)
            {
               list->entry = NULL;
               return false;
            }
            list->entry = menu_driver_ctx->list_get_entry(menu_userdata,
                  list->type, (unsigned int)list->idx);
         }
         break;
      case RARCH_MENU_CTL_LIST_GET_SIZE:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;
            if (!menu_driver_ctx || !menu_driver_ctx->list_get_size)
            {
               list->size = 0;
               return false;
            }
            list->size = menu_driver_ctx->list_get_size(menu_userdata, list->type);
         }
         break;
      case RARCH_MENU_CTL_LIST_GET_SELECTION:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;

            if (!menu_driver_ctx || !menu_driver_ctx->list_get_selection)
            {
               list->selection = 0;
               return false;
            }
            list->selection = menu_driver_ctx->list_get_selection(menu_userdata);
         }
         break;
      case RARCH_MENU_CTL_LIST_FREE:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;

            if (menu_driver_ctx)
            {
               if (menu_driver_ctx->list_free)
                  menu_driver_ctx->list_free(list->list, list->idx, list->list_size);
            }

            if (list->list)
            {
               file_list_free_userdata  (list->list, list->idx);
               file_list_free_actiondata(list->list, list->idx);
            }
         }
         break;
      case RARCH_MENU_CTL_REFRESH:
         {
#if 0
            bool refresh = false;
            menu_entries_ctl(MENU_ENTRIES_CTL_LIST_DEINIT, NULL);
            menu_entries_ctl(MENU_ENTRIES_CTL_SETTINGS_DEINIT, NULL);
            menu_entries_ctl(MENU_ENTRIES_CTL_INIT, NULL);
            menu_entries_ctl(MENU_ENTRIES_CTL_SET_REFRESH, &refresh);
#endif
         }
         break;
      case RARCH_MENU_CTL_LIST_SET_SELECTION:
         {
            file_list_t *list = (file_list_t*)data;

            if (!list)
               return false;

            if (!menu_driver_ctx || !menu_driver_ctx->list_set_selection)
               return false;

            menu_driver_ctx->list_set_selection(menu_userdata, list);
         }
         break;
      case RARCH_MENU_CTL_LIST_CACHE:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;
            if (!list || !menu_driver_ctx || !menu_driver_ctx->list_cache)
               return false;
            menu_driver_ctx->list_cache(menu_userdata,
                  list->type, list->action);
         }
         break;
      case RARCH_MENU_CTL_LIST_INSERT:
         {
            menu_ctx_list_t *list = (menu_ctx_list_t*)data;
            if (!list || !menu_driver_ctx || !menu_driver_ctx->list_insert)
               return false;
            menu_driver_ctx->list_insert(menu_userdata,
                  list->list, list->path, list->fullpath,
                  list->label, list->idx);
         }
         break;
      case RARCH_MENU_CTL_ENVIRONMENT:
         {
            menu_ctx_environment_t *menu_environ =
               (menu_ctx_environment_t*)data;

            if (menu_driver_ctx->environ_cb)
            {
               if (menu_driver_ctx->environ_cb(menu_environ->type,
                        menu_environ->data, menu_userdata) == 0)
                  return true;
            }
         }
         return false;
      case RARCH_MENU_CTL_POINTER_TAP:
         {
            menu_ctx_pointer_t *point = (menu_ctx_pointer_t*)data;
            if (!menu_driver_ctx || !menu_driver_ctx->pointer_tap)
            {
               point->retcode = 0;
               return false;
            }
            point->retcode = menu_driver_ctx->pointer_tap(menu_userdata,
                  point->x, point->y, point->ptr,
                  point->cbs, point->entry, point->action);
         }
         break;
      case RARCH_MENU_CTL_POINTER_DOWN:
         {
            menu_ctx_pointer_t *point = (menu_ctx_pointer_t*)data;
            if (!menu_driver_ctx || !menu_driver_ctx->pointer_down)
            {
               point->retcode = 0;
               return false;
            }
            point->retcode = menu_driver_ctx->pointer_down(menu_userdata,
                  point->x, point->y, point->ptr,
                  point->cbs, point->entry, point->action);
         }
         break;
      case RARCH_MENU_CTL_POINTER_UP:
         {
            menu_ctx_pointer_t *point = (menu_ctx_pointer_t*)data;
            if (!menu_driver_ctx || !menu_driver_ctx->pointer_up)
            {
               point->retcode = 0;
               return false;
            }
            point->retcode = menu_driver_ctx->pointer_up(menu_userdata,
                  point->x, point->y, point->ptr,
                  point->cbs, point->entry, point->action);
         }
         break;
      case RARCH_MENU_CTL_OSK_PTR_AT_POS:
         {
            unsigned width            = 0;
            unsigned height           = 0;
            menu_ctx_pointer_t *point = (menu_ctx_pointer_t*)data;
            if (!menu_driver_ctx || !menu_driver_ctx->osk_ptr_at_pos)
            {
               point->retcode = 0;
               return false;
            }
            video_driver_get_size(&width, &height);
            point->retcode = menu_driver_ctx->osk_ptr_at_pos(menu_userdata,
                  point->x, point->y, width, height);
         }
         break;
      case RARCH_MENU_CTL_BIND_INIT:
         {
            menu_ctx_bind_t *bind = (menu_ctx_bind_t*)data;

            if (!menu_driver_ctx || !menu_driver_ctx->bind_init)
            {
               bind->retcode = 0;
               return false;
            }
            bind->retcode = menu_driver_ctx->bind_init(
                  bind->cbs,
                  bind->path,
                  bind->label,
                  bind->type,
                  bind->idx);
         }
         break;
      case RARCH_MENU_CTL_UPDATE_THUMBNAIL_PATH:
         {
            size_t selection = menu_navigation_get_selection();

            if (!menu_driver_ctx || !menu_driver_ctx->update_thumbnail_path)
               return false;
            menu_driver_ctx->update_thumbnail_path(menu_userdata, (unsigned)selection);
         }
         break;
      case RARCH_MENU_CTL_UPDATE_THUMBNAIL_IMAGE:
         {
            if (!menu_driver_ctx || !menu_driver_ctx->update_thumbnail_image)
               return false;
            menu_driver_ctx->update_thumbnail_image(menu_userdata);
         }
         break;
      case RARCH_MENU_CTL_UPDATE_SAVESTATE_THUMBNAIL_PATH:
         {
            size_t selection = menu_navigation_get_selection();

            if (!menu_driver_ctx || !menu_driver_ctx->update_savestate_thumbnail_path)
               return false;
            menu_driver_ctx->update_savestate_thumbnail_path(menu_userdata, (unsigned)selection);
         }
         break;
      case RARCH_MENU_CTL_UPDATE_SAVESTATE_THUMBNAIL_IMAGE:
         {
            if (!menu_driver_ctx || !menu_driver_ctx->update_savestate_thumbnail_image)
               return false;
            menu_driver_ctx->update_savestate_thumbnail_image(menu_userdata);
         }
         break;
      case MENU_NAVIGATION_CTL_CLEAR:
         {
            bool *pending_push = (bool*)data;

            menu_navigation_set_selection(0);
            menu_driver_navigation_set(true);

            if (pending_push)
               if (menu_driver_ctx->navigation_clear)
                  menu_driver_ctx->navigation_clear(
                        menu_userdata, pending_push);
         }
         break;
      case MENU_NAVIGATION_CTL_INCREMENT:
         {
            settings_t *settings   = config_get_ptr();
            unsigned scroll_speed  = *((unsigned*)data);
            size_t  menu_list_size = menu_entries_get_size();
            bool wraparound_enable = settings->bools.menu_navigation_wraparound_enable;

            if (menu_driver_selection_ptr >= menu_list_size - 1
                  && !wraparound_enable)
               return false;

            if ((menu_driver_selection_ptr + scroll_speed) < menu_list_size)
            {
               size_t idx  = menu_driver_selection_ptr + scroll_speed;

               menu_navigation_set_selection(idx);
               menu_driver_navigation_set(true);
            }
            else
            {
               if (wraparound_enable)
               {
                  bool pending_push = false;
                  menu_driver_ctl(MENU_NAVIGATION_CTL_CLEAR, &pending_push);
               }
               else if (menu_list_size > 0)
                  menu_driver_ctl(MENU_NAVIGATION_CTL_SET_LAST,  NULL);
            }
            
            if (menu_driver_ctx->navigation_increment)
               menu_driver_ctx->navigation_increment(menu_userdata);
         }
         break;
      case MENU_NAVIGATION_CTL_DECREMENT:
         {
            size_t idx             = 0;
            settings_t *settings   = config_get_ptr();
            unsigned scroll_speed  = *((unsigned*)data);
            size_t  menu_list_size = menu_entries_get_size();
            bool wraparound_enable = settings->bools.menu_navigation_wraparound_enable;

            if (menu_driver_selection_ptr == 0 && !wraparound_enable)
               return false;

            if (menu_driver_selection_ptr >= scroll_speed)
               idx = menu_driver_selection_ptr - scroll_speed;
            else
            {
               idx  = menu_list_size - 1;
               if (!wraparound_enable)
                  idx = 0;
            }

            menu_navigation_set_selection(idx);
            menu_driver_navigation_set(true);

            if (menu_driver_ctx->navigation_decrement)
               menu_driver_ctx->navigation_decrement(menu_userdata);
         }
         break;
      case MENU_NAVIGATION_CTL_SET_LAST:
         {
            size_t menu_list_size = menu_entries_get_size();
            size_t new_selection  = menu_list_size - 1;
            menu_navigation_set_selection(new_selection);

            if (menu_driver_ctx->navigation_set_last)
               menu_driver_ctx->navigation_set_last(menu_userdata);
         }
         break;
      case MENU_NAVIGATION_CTL_ASCEND_ALPHABET:
         {
            size_t i               = 0;
            size_t  menu_list_size = menu_entries_get_size();

            if (!scroll_index_size)
               return false;

            if (menu_driver_selection_ptr == scroll_index_list[scroll_index_size - 1])
               menu_driver_selection_ptr = menu_list_size - 1;
            else
            {
               while (i < scroll_index_size - 1
                     && scroll_index_list[i + 1] <= menu_driver_selection_ptr)
                  i++;
               menu_driver_selection_ptr = scroll_index_list[i + 1];

               if (menu_driver_selection_ptr >= menu_list_size)
                  menu_driver_selection_ptr = menu_list_size - 1;
            }

            if (menu_driver_ctx->navigation_ascend_alphabet)
               menu_driver_ctx->navigation_ascend_alphabet(
                     menu_userdata, &menu_driver_selection_ptr);
         }
         break;
      case MENU_NAVIGATION_CTL_DESCEND_ALPHABET:
         {
            size_t i        = 0;

            if (!scroll_index_size)
               return false;

            if (menu_driver_selection_ptr == 0)
               return false;

            i   = scroll_index_size - 1;

            while (i && scroll_index_list[i - 1] >= menu_driver_selection_ptr)
               i--;

            if (i > 0)
               menu_driver_selection_ptr = scroll_index_list[i - 1];

            if (menu_driver_ctx->navigation_descend_alphabet)
               menu_driver_ctx->navigation_descend_alphabet(
                     menu_userdata, &menu_driver_selection_ptr);
         }
         break;
      case MENU_NAVIGATION_CTL_CLEAR_SCROLL_INDICES:
         scroll_index_size = 0;
         break;
      case MENU_NAVIGATION_CTL_ADD_SCROLL_INDEX:
         {
            size_t *sel = (size_t*)data;
            if (!sel)
               return false;
            scroll_index_list[scroll_index_size++] = *sel;
         }
         break;
      case MENU_NAVIGATION_CTL_GET_SCROLL_ACCEL:
         {
            size_t *sel = (size_t*)data;
            if (!sel)
               return false;
            *sel = scroll_acceleration;
         }
         break;
      case MENU_NAVIGATION_CTL_SET_SCROLL_ACCEL:
         {
            size_t *sel = (size_t*)data;
            if (!sel)
               return false;
            scroll_acceleration = (unsigned)(*sel);
         }
         break;
      default:
      case RARCH_MENU_CTL_NONE:
         break;
   }

   return true;
}

size_t menu_navigation_get_selection(void)
{
   return menu_driver_selection_ptr;
}

void menu_navigation_set_selection(size_t val)
{
   menu_driver_selection_ptr = val;
}
