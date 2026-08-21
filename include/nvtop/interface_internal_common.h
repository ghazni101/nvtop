/*
 *
 * Copyright (C) 2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef INTERFACE_INTERNAL_COMMON_H__
#define INTERFACE_INTERNAL_COMMON_H__

#include "nvtop/common.h"
#include "nvtop/interface_options.h"
#include "nvtop/interface_ring_buffer.h"
#include "nvtop/time.h"

#include <ncurses.h>
#include <stdbool.h>

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

enum nvtop_option_window_state {
  nvtop_option_state_hidden,
  nvtop_option_state_kill,
  nvtop_option_state_sort_by,
};

enum interface_color {
  cyan_color = 1,
  yellow_color,
  magenta_color,
  green_color,
  red_color,
  blue_color,
  gpu_util_plot_color,
  gpu_mem_plot_color,
  gpu_plot_color_3,
  gpu_plot_color_4,
  // Chart fill gradient: solid background fill fading from the series
  // color at the curve's edge through a mid shade to a dark body shade.
  gpu_util_plot_fill_color,
  gpu_mem_plot_fill_color,
  gpu_plot_fill_color_3,
  gpu_plot_fill_color_4,
  gpu_util_plot_mid_color,
  gpu_mem_plot_mid_color,
  gpu_plot_mid_color_3,
  gpu_plot_mid_color_4,
  gpu_util_plot_body_color,
  gpu_mem_plot_body_color,
  gpu_plot_body_color_3,
  gpu_plot_body_color_4,
  // Braille fill gradient: the cells right below the curve use a mid
  // foreground shade before the dim body.
  gpu_util_plot_mid_fg_color,
  gpu_mem_plot_mid_fg_color,
  gpu_plot_mid_fg_color_3,
  gpu_plot_mid_fg_color_4,
  dim_color,
  grid_color,
  // Chrome grays: card/plot frames, dimmed labels, meter tracks. When the
  // terminal offers 256 colors these become palette grays, otherwise they
  // degrade to the default foreground with A_DIM.
  frame_color,
  label_color,
  track_color,
  value_on_green_color,
  value_on_yellow_color,
  value_on_red_color,
  value_on_empty_color,
};

struct device_window {
  WINDOW *frame_win; // Card chrome: rounded frame carrying the GPU name
  WINDOW *gpu_util_enc_dec;
  WINDOW *gpu_util_no_enc_or_dec;
  WINDOW *gpu_util_no_enc_and_dec;
  WINDOW *mem_util;
  WINDOW *encode_util;
  WINDOW *decode_util;
  WINDOW *encdec_util;
  WINDOW *fan_speed;
  WINDOW *temperature;
  WINDOW *power_info;
  WINDOW *gpu_clock_info;
  WINDOW *mem_clock_info;
  WINDOW *shader_cores;
  WINDOW *l2_cache_size;
  WINDOW *exec_engines;
  bool enc_was_visible;
  bool dec_was_visible;
  nvtop_time last_decode_seen;
  nvtop_time last_encode_seen;
};

static const unsigned int option_window_size = 13;
struct option_window {
  enum nvtop_option_window_state state;
  enum nvtop_option_window_state previous_state;
  unsigned int selected_row;
  unsigned int offset;
  WINDOW *option_win;
  bool last_key_was_number;
  unsigned int input_number;
};

struct gpuid_and_process {
  unsigned gpu_id;
  struct gpu_process *process;
};

struct process_window {
  unsigned offset;
  unsigned offset_column;
  WINDOW *frame_win; // Card chrome around the list (unicode, tall enough)
  WINDOW *process_win;
  WINDOW *process_with_option_win;
  unsigned selected_row;
  pid_t selected_pid;
  struct option_window option_window;
  // Cached process array: rebuilt only when fresh data arrives, re-sorted
  // only when the sort criterion/order changes. Key navigation reuses it.
  struct gpuid_and_process *cached_processes;
  unsigned cached_count;
  bool cache_valid;
  bool sort_valid;
  enum process_field cached_sort_by;
  bool cached_sort_desc;
};

struct plot_window {
  size_t num_data;
  double *data;
  WINDOW *win;
  WINDOW *plot_window;
  unsigned num_devices_to_plot;
  unsigned devices_ids[MAX_LINES_PER_PLOT];
};

enum setup_window_section {
  setup_general_selected,
  setup_header_selected,
  setup_chart_selected,
  setup_process_list_selected,
  setup_monitored_gpu_list_selected,
  setup_window_selection_count
};

struct setup_window {
  unsigned indentation_level;
  enum setup_window_section selected_section;
  bool visible;
  WINDOW *clean_space;
  WINDOW *setup;
  WINDOW *single;
  WINDOW *split[2];
  unsigned options_selected[2];
};

// Keep gpu information every 1 second for 10 minutes
struct nvtop_interface {
  nvtop_interface_option options;
  unsigned total_dev_count;
  unsigned monitored_dev_count;
  struct device_window *devices_win;
  struct process_window process;
  WINDOW *shortcut_window;
  unsigned num_plots;
  struct plot_window *plots;
  interface_ring_buffer saved_data_ring;
  struct setup_window setup_win;
  // Dirty flags: sections are only re-rendered when their inputs changed
  // (new data landed once per update interval) or an interaction demands it.
  // Between data updates, idle frames redraw nothing.
  bool redraw_all;
  bool devices_dirty;
  bool process_dirty;
  bool setup_dirty;
  bool use_unicode;
};

enum device_field {
  device_name = 0,
  device_fan_speed,
  device_temperature,
  device_power,
  device_pcie,
  device_clock,
  device_mem_clock,
  device_shadercores,
  device_l2features,
  device_execengines,
  device_field_count,
};

inline void set_attribute_between(WINDOW *win, int startY, int startX, int endX, attr_t attr, short pair) {
  int rows, cols;
  getmaxyx(win, rows, cols);
  (void)rows;
  if (startX >= cols || endX < 0)
    return;
  startX = startX < 0 ? 0 : startX;
  endX = endX > cols ? cols : endX;
  int size = endX - startX;
  mvwchgat(win, startY, startX, size, attr, pair, NULL);
}

// Style for the shortcut bars: bold colored key, dim label. Implemented in
// interface.c where the color state lives.
void nvtop_print_shortcut(WINDOW *win, const char *key, const char *label);

#endif // INTERFACE_INTERNAL_COMMON_H__
