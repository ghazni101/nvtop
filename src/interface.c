/*
 *
 * Copyright (C) 2017-2022 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/interface.h"
#include "nvtop/interface_common.h"
#include "nvtop/interface_internal_common.h"
#include "nvtop/interface_layout_selection.h"
#include "nvtop/interface_options.h"
#include "nvtop/interface_ring_buffer.h"
#include "nvtop/interface_setup_win.h"
#include "nvtop/plot.h"
#include "nvtop/time.h"
#include "nvtop/version.h"

#include <assert.h>
#include <inttypes.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tgmath.h>
#include <time.h>
#include <unistd.h>

// The unicode look (rounded frames, gradient meters, title bar) is used when
// the locale is UTF-8 and ncurses was built with wide-character support.
static bool interface_unicode = false;
static bool interface_use_color = false;
// True when the terminal palette has the 256-color grays used for chrome.
static bool interface_ext_colors = false;

// Chrome text (labels, frames, axis): palette gray when available, A_DIM on
// the default foreground otherwise. Defined next to the meter code.
static void set_chrome(WINDOW *win, short pair);
static void unset_chrome(WINDOW *win);

static void nvtop_detect_unicode(void) {
#ifdef NVTOP_HAVE_WIDE_CURSES
  const char *force = getenv("NVTOP_UNICODE");
  if (force && strcmp(force, "0") == 0) {
    interface_unicode = false;
    return;
  }
  if (force && strcmp(force, "1") == 0) {
    interface_unicode = true;
    return;
  }
  const char *codeset = nl_langinfo(CODESET);
  interface_unicode = codeset && (strstr(codeset, "UTF-8") || strstr(codeset, "utf8"));
#else
  interface_unicode = false;
#endif
}

static unsigned int sizeof_device_field[device_field_count] = {
    [device_name] = 11,       [device_fan_speed] = 11,   [device_temperature] = 10, [device_power] = 13,
    [device_clock] = 11,      [device_mem_clock] = 11,   [device_pcie] = 46,        [device_shadercores] = 9,
    [device_l2features] = 11, [device_execengines] = 11,
};

static unsigned int sizeof_process_field[process_field_count] = {
    [process_pid] = 7,       [process_user] = 4,          [process_gpu_id] = 3,   [process_type] = 8,
    [process_gpu_rate] = 4,  [process_enc_rate] = 4,      [process_dec_rate] = 4,
    [process_memory] = 14, // 9 for mem 5 for %
    [process_cpu_usage] = 6, [process_cpu_mem_usage] = 9, [process_command] = 0,
};

static void alloc_device_window(unsigned int start_row, unsigned int start_col, unsigned int totalcol,
                                unsigned int totalrow, const nvtop_interface_option *opts,
                                struct device_window *dwin) {

  const unsigned int spacer = 1;
  (void)totalrow;

  // Line 1 = GPU clk | MEM clk | Temp | Fan | Power (each field optional)
  dwin->gpu_clock_info = NULL;
  dwin->mem_clock_info = NULL;
  dwin->temperature = NULL;
  dwin->fan_speed = NULL;
  dwin->power_info = NULL;
  unsigned meter_row = start_row;
  if (opts->show_header_stats) {
    unsigned int offset = start_col;
    if (opts->show_gpu_clock_stat) {
      dwin->gpu_clock_info = newwin(1, sizeof_device_field[device_clock], start_row, offset);
      if (dwin->gpu_clock_info == NULL)
        goto alloc_error;
      offset += spacer + sizeof_device_field[device_clock];
    }
    if (opts->show_mem_clock_stat) {
      dwin->mem_clock_info = newwin(1, sizeof_device_field[device_mem_clock], start_row, offset);
      if (dwin->mem_clock_info == NULL)
        goto alloc_error;
      offset += spacer + sizeof_device_field[device_mem_clock];
    }
    if (opts->show_temp_stat) {
      dwin->temperature = newwin(1, sizeof_device_field[device_temperature], start_row, offset);
      if (dwin->temperature == NULL)
        goto alloc_error;
      offset += spacer + sizeof_device_field[device_temperature];
    }
    if (opts->show_fan_stat) {
      dwin->fan_speed = newwin(1, sizeof_device_field[device_fan_speed], start_row, offset);
      if (dwin->fan_speed == NULL)
        goto alloc_error;
      offset += spacer + sizeof_device_field[device_fan_speed];
    }
    if (opts->show_power_stat) {
      dwin->power_info = newwin(1, sizeof_device_field[device_power], start_row, offset);
      if (dwin->power_info == NULL)
        goto alloc_error;
    }
    meter_row += 1;
  }

  // Line 2 = GPU used [| Encoder | Decoder]
  // Line 3 = MEM used

  int size_enc_dec_pair = totalcol / 3;
  if (size_enc_dec_pair % 2 == 1)
    size_enc_dec_pair += 1;
  if (size_enc_dec_pair / 2 < 14) {
    size_enc_dec_pair = min(totalcol / 2, 28);
    if (size_enc_dec_pair % 2 == 1)
      size_enc_dec_pair += 1;
  }
  int size_encode = size_enc_dec_pair / 2;
  int size_decode = size_encode;
  int size_gpu_with_both = totalcol - spacer * 2 - size_encode - size_decode;
  int size_gpu_with_one = totalcol - spacer - size_decode;
  int size_gpu_alone = totalcol;

  dwin->gpu_util_enc_dec = newwin(1, size_gpu_with_both, meter_row, start_col);
  if (dwin->gpu_util_enc_dec == NULL)
    goto alloc_error;
  dwin->encode_util = newwin(1, size_encode, meter_row, start_col + spacer + size_gpu_with_both);
  if (dwin->encode_util == NULL)
    goto alloc_error;
  dwin->decode_util = newwin(1, size_decode, meter_row, start_col + spacer * 2 + size_gpu_with_both + size_encode);
  if (dwin->decode_util == NULL)
    goto alloc_error;
  dwin->encdec_util = newwin(1, size_encode * 2, meter_row, start_col + spacer + size_gpu_with_both);
  if (dwin->encdec_util == NULL)
    goto alloc_error;
  // For auto-hide encode / decode window
  dwin->gpu_util_no_enc_or_dec = newwin(1, size_gpu_with_one, meter_row, start_col);
  if (dwin->gpu_util_no_enc_or_dec == NULL)
    goto alloc_error;
  dwin->gpu_util_no_enc_and_dec = newwin(1, size_gpu_alone, meter_row, start_col);
  if (dwin->gpu_util_no_enc_and_dec == NULL)
    goto alloc_error;

  dwin->mem_util = newwin(1, totalcol, meter_row + 1, start_col);
  if (dwin->mem_util == NULL)
    goto alloc_error;

  dwin->enc_was_visible = false;
  dwin->dec_was_visible = false;

  // Line 4 = Number of shading cores | L2 Features
  dwin->shader_cores = newwin(1, sizeof_device_field[device_shadercores], meter_row + 2, start_col);
  if (dwin->shader_cores == NULL)
    goto alloc_error;
  dwin->l2_cache_size = newwin(1, sizeof_device_field[device_l2features], meter_row + 2,
                               start_col + spacer + sizeof_device_field[device_shadercores]);
  if (dwin->l2_cache_size == NULL)
    goto alloc_error;
  dwin->exec_engines =
      newwin(1, sizeof_device_field[device_execengines], meter_row + 2,
             start_col + spacer * 2 + sizeof_device_field[device_shadercores] + sizeof_device_field[device_l2features]);
  if (dwin->exec_engines == NULL)
    goto alloc_error;

  return;
alloc_error:
  endwin();
  fprintf(stderr, "Error: Not enough columns to draw device information\n");
  exit(EXIT_FAILURE);
}

static void free_device_windows(struct device_window *dwin) {
  delwin(dwin->gpu_util_enc_dec);
  delwin(dwin->gpu_util_no_enc_or_dec);
  delwin(dwin->gpu_util_no_enc_and_dec);
  delwin(dwin->mem_util);
  delwin(dwin->encode_util);
  delwin(dwin->decode_util);
  delwin(dwin->encdec_util);
  delwin(dwin->gpu_clock_info);
  delwin(dwin->mem_clock_info);
  delwin(dwin->power_info);
  delwin(dwin->temperature);
  delwin(dwin->fan_speed);
  delwin(dwin->shader_cores);
  delwin(dwin->l2_cache_size);
  delwin(dwin->exec_engines);
}

static void alloc_process_with_option(struct nvtop_interface *interface, unsigned posX, unsigned posY, unsigned sizeX,
                                      unsigned sizeY) {
  interface->process.frame_win = NULL;
  if (sizeY > 0) {
    if (interface_unicode && sizeY >= 6 && sizeX > option_window_size + 2) {
      // Card frame around the list; the header row lives inside it.
      interface->process.frame_win = newwin(sizeY, sizeX, posY, posX);
      draw_rectangle(interface->process.frame_win, 0, 0, sizeX, sizeY);
      mvwprintw(interface->process.frame_win, 0, 2, " Processes ");
      wnoutrefresh(interface->process.frame_win);
      posY += 1;
      posX += 1;
      sizeX -= 2;
      sizeY -= 2;
    }
    interface->process.process_win = newwin(sizeY, sizeX, posY, posX);
    interface->process.process_with_option_win =
        newwin(sizeY, sizeX - option_window_size, posY, posX + option_window_size);
  } else {
    interface->process.process_win = NULL;
    interface->process.process_with_option_win = NULL;
  }
  interface->process.selected_row = 0;
  interface->process.selected_pid = -1;
  interface->process.offset_column = 0;
  interface->process.offset = 0;

  interface->process.option_window.option_win = newwin(sizeY, option_window_size, posY, posX);

  interface->process.option_window.state = nvtop_option_state_hidden;
  interface->process.option_window.previous_state = nvtop_option_state_sort_by;
  interface->process.option_window.offset = 0;
  interface->process.option_window.selected_row = 0;
  interface->process.option_window.last_key_was_number = false;
  interface->process.option_window.input_number = 0;
}

static void initialize_gpu_mem_plot(struct plot_window *plot, struct window_position *position,
                                    nvtop_interface_option *options) {
  unsigned rows = position->sizeY;
  unsigned cols = position->sizeX;
  // The left gutter exists only for the percentage axis labels. With the
  // axis hidden the chart frame hugs the plot area edge-to-edge instead of
  // leaving a blank 4-column margin.
  unsigned left = options->show_chart_axis ? 4 : 1;
  cols -= left + 1;
  rows -= 2;
  plot->plot_window = newwin(rows, cols, position->posY + 1, position->posX + left);
  draw_rectangle(plot->win, options->show_chart_axis ? 3 : 0, 0, cols + 2, rows + 2);
  // Axis labels are chrome: keep them dim so the trace stands out. They
  // MUST use the exact same data->row mapping as the trace (plot_label_row
  // over the inner window height) plus one row to go from inner to outer
  // window coordinates. At very small heights two levels can share a row —
  // the later (lower) label wins and the earlier one is skipped instead of
  // overprinting it.
  if (options->show_chart_axis) {
    int last_label_row = -1;
    static const unsigned label_levels[5] = {100, 75, 50, 25, 0};
    for (unsigned lvl = 0; lvl < ARRAY_SIZE(label_levels); ++lvl) {
      int r = plot_label_row(rows, label_levels[lvl]);
      if (r == last_label_row)
        continue;
      last_label_row = r;
      char text[5];
      snprintf(text, sizeof(text), "%3u", label_levels[lvl]);
      mvwprintw(plot->win, r + 1, 0, "%s", text);
    }
  }
  plot->data = calloc((size_t)cols * MAX_LINES_PER_PLOT, sizeof(*plot->data));
  plot->num_data = cols;
  (void)options;

  unset_chrome(plot->win);
  wnoutrefresh(plot->win);
}

static void alloc_plot_window(unsigned devices_count, struct window_position *plot_positions,
                              unsigned map_device_to_plot[devices_count], struct nvtop_interface *interface) {
  if (!interface->num_plots) {
    interface->plots = NULL;
    return;
  }
  interface->plots = malloc(interface->num_plots * sizeof(*interface->plots));
  for (size_t i = 0; i < interface->num_plots; ++i) {
    interface->plots[i].num_devices_to_plot = 0;
    for (unsigned dev_id = 0; dev_id < devices_count; ++dev_id) {
      if (map_device_to_plot[dev_id] == i) {
        interface->plots[i].devices_ids[interface->plots[i].num_devices_to_plot] = dev_id;
        interface->plots[i].num_devices_to_plot++;
      }
    }
    interface->plots[i].win =
        newwin(plot_positions[i].sizeY, plot_positions[i].sizeX, plot_positions[i].posY, plot_positions[i].posX);
    initialize_gpu_mem_plot(&interface->plots[i], &plot_positions[i], &interface->options);
  }
}

static unsigned device_length(const nvtop_interface_option *opts) {
  // Outer card width: the info line (visible fields + spacers) plus the two
  // frame columns. The GPU name lives in the card title and clips instead
  // of stretching every card.
  unsigned width = 0;
  unsigned visible = 0;
  if (opts->show_header_stats) {
    if (opts->show_gpu_clock_stat) {
      width += sizeof_device_field[device_clock];
      visible++;
    }
    if (opts->show_mem_clock_stat) {
      width += sizeof_device_field[device_mem_clock];
      visible++;
    }
    if (opts->show_temp_stat) {
      width += sizeof_device_field[device_temperature];
      visible++;
    }
    if (opts->show_fan_stat) {
      width += sizeof_device_field[device_fan_speed];
      visible++;
    }
    if (opts->show_power_stat) {
      width += sizeof_device_field[device_power];
      visible++;
    }
    width += visible > 0 ? visible - 1 : 0; // one spacer between fields
  }
  return width + 2;
}

static pid_t nvtop_pid;

static void initialize_all_windows(struct nvtop_interface *dwin) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  unsigned int devices_count = dwin->monitored_dev_count;

  struct window_position device_positions[devices_count];
  unsigned map_device_to_plot[devices_count];
  struct window_position process_position;
  struct window_position plot_positions[MAX_CHARTS];
  struct window_position setup_position;

  // The bottom row hosts the shortcut bar when it is shown.
  bool show_shortcut_bar = dwin->options.show_shortcut_bar;
  int layout_rows = rows - (show_shortcut_bar ? 1 : 0);
  if (layout_rows < 1)
    layout_rows = 1;
  compute_sizes_from_layout(devices_count, (dwin->options.has_gpu_info_bar ? 4 : 3) -
                                                (dwin->options.show_header_stats ? 0 : 1),
                            device_length(&dwin->options), (unsigned)layout_rows, cols, dwin->options.gpu_specific_opts,
                            dwin->options.process_fields_displayed, device_positions, &dwin->num_plots,
                            plot_positions, map_device_to_plot, &process_position, &setup_position,
                            dwin->options.hide_processes_list);

  alloc_plot_window(devices_count, plot_positions, map_device_to_plot, dwin);

  // The settings gear floats in the screen's top-right corner and is ALWAYS
  // visible (ASCII fallback when wide glyphs are unavailable).
  dwin->gear_window = NULL;
  dwin->gear_count = 0;
  if (cols >= 8) {
    int gear_width = 3;
    dwin->gear_window = newwin(1, gear_width, 0, cols - gear_width);
    dwin->gear_count = 1;
    dwin->gear_rects[0].y = 0;
    dwin->gear_rects[0].x0 = cols - gear_width;
    dwin->gear_rects[0].x1 = cols - 1;
  }
  for (unsigned int i = 0; i < devices_count; ++i) {
    alloc_device_window(device_positions[i].posY, device_positions[i].posX, device_positions[i].sizeX,
                        device_positions[i].sizeY, &dwin->options, &dwin->devices_win[i]);
  }

  alloc_process_with_option(dwin, process_position.posX, process_position.posY, process_position.sizeX,
                            process_position.sizeY);

  dwin->shortcut_window = show_shortcut_bar ? newwin(1, cols, rows - 1, 0) : NULL;

  alloc_setup_window(&setup_position, &dwin->setup_win);
  nvtop_pid = getpid();
}

static void delete_all_windows(struct nvtop_interface *dwin) {
  for (unsigned int i = 0; i < dwin->monitored_dev_count; ++i) {
    free_device_windows(&dwin->devices_win[i]);
  }
  delwin(dwin->process.process_win);
  delwin(dwin->process.process_with_option_win);
  dwin->process.process_win = NULL;
  dwin->process.process_with_option_win = NULL;
  delwin(dwin->process.frame_win);
  dwin->process.frame_win = NULL;
  delwin(dwin->shortcut_window);
  delwin(dwin->gear_window);
  delwin(dwin->process.option_window.option_win);
  for (size_t i = 0; i < dwin->num_plots; ++i) {
    delwin(dwin->plots[i].win);
    delwin(dwin->plots[i].plot_window);
    free(dwin->plots[i].data);
  }
  free_setup_window(&dwin->setup_win);
  free(dwin->plots);
}

static const NCURSES_COLOR_T plot_terminal_colors[] = {COLOR_RED,  COLOR_CYAN,    COLOR_GREEN, COLOR_YELLOW,
                                                       COLOR_BLUE, COLOR_MAGENTA, COLOR_WHITE};

static void initialize_colors(const unsigned char plot_color_idx[MAX_LINES_PER_PLOT]) {
  start_color();
  short background_color;
#ifdef NCURSES_VERSION
  if (use_default_colors() == OK)
    background_color = -1;
  else
    background_color = COLOR_BLACK;
#else
  background_color = COLOR_BLACK;
#endif
  interface_ext_colors = COLORS >= 256;
  init_pair(cyan_color, COLOR_CYAN, background_color);
  init_pair(red_color, COLOR_RED, background_color);
  init_pair(green_color, COLOR_GREEN, background_color);
  init_pair(yellow_color, COLOR_YELLOW, background_color);
  init_pair(blue_color, COLOR_BLUE, background_color);
  init_pair(magenta_color, COLOR_MAGENTA, background_color);
  // Chrome grays (dim text, frames, grid, meter tracks): palette grays on
  // 256-color terminals, plain white (paired with A_DIM at use sites)
  // otherwise.
  init_pair(dim_color, interface_ext_colors ? 244 : COLOR_WHITE, background_color);
  init_pair(grid_color, interface_ext_colors ? 238 : COLOR_WHITE, background_color);
  init_pair(frame_color, interface_ext_colors ? 239 : COLOR_WHITE, background_color);
  init_pair(label_color, interface_ext_colors ? 243 : COLOR_WHITE, background_color);
  init_pair(track_color, interface_ext_colors ? 236 : COLOR_WHITE, background_color);
  init_pair(value_on_green_color, COLOR_BLACK, COLOR_GREEN);
  init_pair(value_on_yellow_color, COLOR_BLACK, COLOR_YELLOW);
  init_pair(value_on_red_color, COLOR_BLACK, COLOR_RED);
  init_pair(value_on_empty_color, COLOR_BLACK, COLOR_WHITE);
  static const short gpu_plot_pairs[MAX_LINES_PER_PLOT] = {
      gpu_util_plot_color, gpu_mem_plot_color, gpu_plot_color_3, gpu_plot_color_4};
  for (unsigned s = 0; s < MAX_LINES_PER_PLOT; ++s)
    init_pair(gpu_plot_pairs[s], plot_terminal_colors[plot_color_idx[s]], background_color);
  // Chart fill gradient: the edge cells paint the series color as the
  // background, fading through a mid shade into a dark body shade. Solid
  // background cells keep the fill seamless in every terminal. Below 256
  // colors mid/body degrade to the series color (the ASCII renderer then
  // falls back to dimmed block glyphs anyway).
  static const short plot_mid_variants[7] = {88, 30, 28, 100, 19, 90, 238};
  static const short plot_body_variants[7] = {52, 23, 22, 58, 17, 54, 236};
  static const short gpu_plot_fill_pairs[MAX_LINES_PER_PLOT] = {
      gpu_util_plot_fill_color, gpu_mem_plot_fill_color, gpu_plot_fill_color_3, gpu_plot_fill_color_4};
  static const short gpu_plot_mid_pairs[MAX_LINES_PER_PLOT] = {
      gpu_util_plot_mid_color, gpu_mem_plot_mid_color, gpu_plot_mid_color_3, gpu_plot_mid_color_4};
  static const short gpu_plot_body_pairs[MAX_LINES_PER_PLOT] = {
      gpu_util_plot_body_color, gpu_mem_plot_body_color, gpu_plot_body_color_3, gpu_plot_body_color_4};
  static const short gpu_plot_mid_fg_pairs[MAX_LINES_PER_PLOT] = {
      gpu_util_plot_mid_fg_color, gpu_mem_plot_mid_fg_color, gpu_plot_mid_fg_color_3, gpu_plot_mid_fg_color_4};
  for (unsigned s = 0; s < MAX_LINES_PER_PLOT; ++s) {
    short idx = plot_color_idx[s];
    init_pair(gpu_plot_fill_pairs[s], background_color, plot_terminal_colors[idx]);
    init_pair(gpu_plot_mid_pairs[s], background_color,
              interface_ext_colors ? plot_mid_variants[idx] : plot_terminal_colors[idx]);
    init_pair(gpu_plot_body_pairs[s], background_color,
              interface_ext_colors ? plot_body_variants[idx] : plot_terminal_colors[idx]);
    init_pair(gpu_plot_mid_fg_pairs[s],
              interface_ext_colors ? plot_mid_variants[idx] : plot_terminal_colors[idx], background_color);
  }
}

struct nvtop_interface *initialize_curses(unsigned total_devices, unsigned devices_count, unsigned largest_device_name,
                                          nvtop_interface_option options) {
  struct nvtop_interface *interface = calloc(1, sizeof(*interface));
  interface->options = options;
  interface->devices_win = calloc(devices_count, sizeof(*interface->devices_win));
  interface->total_dev_count = total_devices;
  interface->monitored_dev_count = devices_count;
  sizeof_device_field[device_name] = largest_device_name + 11;
  setlocale(LC_CTYPE, "");
  nvtop_detect_unicode();
  interface->use_unicode = interface_unicode;
  initscr();
  refresh();
  if (interface->options.use_color && has_colors() == TRUE) {
    interface_use_color = true;
    initialize_colors(options.gpu_plot_color_idx);
  }
  nvtop_plot_set_unicode(interface_unicode);
  nvtop_plot_set_color(interface_use_color);
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  mousemask(BUTTON1_PRESSED, NULL);
  curs_set(0);

  interface->redraw_all = true;
  interface->devices_dirty = true;
  interface->process_dirty = true;

  // Hide decode and encode if not active for some time
  if (interface->options.encode_decode_hiding_timer > 0.) {
    nvtop_time time_now, some_time_in_past;
    some_time_in_past =
        nvtop_hmns_to_time(0, (unsigned int)(interface->options.encode_decode_hiding_timer / 60.) + 1, 0);

    nvtop_get_current_time(&time_now);
    some_time_in_past = nvtop_substract_time(time_now, some_time_in_past);
    for (size_t i = 0; i < devices_count; ++i) {
      interface->devices_win[i].last_encode_seen = some_time_in_past;
      interface->devices_win[i].last_decode_seen = some_time_in_past;
    }
  }

  interface_alloc_ring_buffer(devices_count, 4, 10 * 60 * 1000, &interface->saved_data_ring);
  initialize_all_windows(interface);
  return interface;
}

void apply_plot_colors(const unsigned char plot_color_idx[MAX_LINES_PER_PLOT]) {
  initialize_colors(plot_color_idx);
}

void clean_ncurses(struct nvtop_interface *interface) {
  endwin();
  delete_all_windows(interface);
  free(interface->options.gpu_specific_opts);
  free(interface->options.config_file_location);
  free(interface->devices_win);
  free(interface->process.cached_processes);
  interface_free_ring_buffer(&interface->saved_data_ring);
  free(interface);
}

// Compact mode: no breathing room around the percentage meter bars. The
// bar starts right after the label and runs to the right edge of the meter
// window. Applied to the unicode and ASCII meter paths.
#define METER_BAR_PAD 0

// Chrome text (labels, frames, axis): palette gray when available, A_DIM on
// the default foreground otherwise.
static void set_chrome(WINDOW *win, short pair) {
  if (interface_use_color)
    wattr_set(win, interface_ext_colors ? A_NORMAL : A_DIM, pair, NULL);
  else
    wattron(win, A_DIM);
}

static void unset_chrome(WINDOW *win) {
  if (interface_use_color)
    wattr_set(win, A_NORMAL, 0, NULL);
  else
    wattroff(win, A_DIM);
}
// Eighth-block glyphs for sub-cell meter resolution: 1/8 .. 8/8
static const char *const meter_blocks[9] = {
    " ",            // 0/8
    "\xe2\x96\x8f", // ▏
    "\xe2\x96\x8e", // ▎
    "\xe2\x96\x8d", // ▍
    "\xe2\x96\x8c", // ▌
    "\xe2\x96\x8b", // ▋
    "\xe2\x96\x8a", // ▊
    "\xe2\x96\x89", // ▉
    "\xe2\x96\x88", // █
};

static short meter_fill_pair(unsigned percentage) {
  if (!interface_use_color)
    return 0;
  if (percentage >= 85)
    return red_color;
  if (percentage >= 60)
    return yellow_color;
  return green_color;
}

// Overlay the meter value right-aligned on top of the bar. Where the value
// sits on the colored fill it is drawn black-on-color (a badge); on the dim
// track it is bold in the default foreground, so an idle meter carries no
// bright patch. yellow_cells marks a leading yellow segment (effective
// load). Right-aligned inside the padded bar area.
static void overlay_meter_value(WINDOW *win, int cols, int bar_start, unsigned percentage, int fill_cells,
                                int yellow_cells, const char *value) {
  int value_len = (int)strlen(value);
  if (value_len <= 0)
    return;
  int overlay_start = cols - METER_BAR_PAD - value_len;
  if (overlay_start < bar_start)
    overlay_start = bar_start;

  short on_fill_pair = value_on_green_color;
  short fill_pair = meter_fill_pair(percentage);
  if (fill_pair == yellow_color)
    on_fill_pair = value_on_yellow_color;
  else if (fill_pair == red_color)
    on_fill_pair = value_on_red_color;

  for (int j = 0; j < value_len; ++j) {
    int c = overlay_start + j;
    if (c >= cols)
      break;
    int bar_cell = c - bar_start;
    short pair = 0;
    attr_t attr = A_BOLD;
    if (interface_use_color && bar_cell >= 0) {
      if (bar_cell < yellow_cells)
        pair = value_on_yellow_color;
      else if (bar_cell < fill_cells)
        pair = on_fill_pair;
      else
        attr = A_BOLD; // on the track: bold default text, no badge
    }
    wattr_set(win, attr, pair, NULL);
    mvwaddch(win, 0, c, (chtype)(unsigned char)value[j]);
  }
  wstandend(win);
}

static void draw_percentage_meter(WINDOW *win, const char *prelude, unsigned int new_percentage,
                                  const char inside_braces_right[1024]) {
  int rows, cols;
  getmaxyx(win, rows, cols);
  (void)rows;

  wmove(win, 0, 0);
  wclrtoeol(win);

  // Label (dimmed so the bar and value stand out)
  set_chrome(win, label_color);
  wprintw(win, "%s", prelude);
  unset_chrome(win);
  int bar_start = getcurx(win) + METER_BAR_PAD;
  int bar_cols = cols - bar_start - METER_BAR_PAD;
  if (bar_cols < 1)
    bar_cols = 1;

  if (interface_unicode) {
    // Smooth meter filling the window width minus a small margin on both
    // sides; the value text is overlaid on the right side of the bar (see
    // overlay_meter_value).
    unsigned long long total_eighths =
        (unsigned long long)llround((double)new_percentage / 100. * (double)bar_cols * 8.);
    int full = (int)(total_eighths / 8);
    int frac = (int)(total_eighths % 8);
    if (full > bar_cols)
      full = bar_cols;

    wcolor_set(win, meter_fill_pair(new_percentage), NULL);
    wmove(win, 0, bar_start);
    for (int i = 0; i < full; ++i)
      waddstr(win, meter_blocks[8]);
    if (full < bar_cols) {
      int empty_start = full;
      if (frac > 0) {
        // Fractional cell only when non-zero: meter_blocks[0] is a space,
        // which would shift the bar one column right of its label.
        waddstr(win, meter_blocks[frac]);
        empty_start = full + 1;
      }
      set_chrome(win, track_color);
      for (int i = empty_start; i < bar_cols; ++i)
        waddstr(win, "\xe2\x96\x91"); // ░
      unset_chrome(win);
    }
    // Value overlaid, right-aligned on the bar
    wstandend(win);
    overlay_meter_value(win, cols, bar_start, new_percentage, full, 0, inside_braces_right);
  } else {
    // Classic ASCII meter: [||||     ] with a small margin on both sides
    for (int pad = 0; pad < METER_BAR_PAD; ++pad)
      waddch(win, ' ');
    waddch(win, '[');
    int curx = getcurx(win);
    int cury = getcury(win);
    int between_sbraces = cols - curx - 1 - METER_BAR_PAD;
    if (between_sbraces < 1)
      between_sbraces = 1;
    float usage = round((float)between_sbraces * new_percentage / 100.f);
    int represent_usage = (int)usage;
    whline(win, '|', represent_usage);
    mvwhline(win, cury, curx + represent_usage, ' ', between_sbraces - represent_usage);
    mvwaddch(win, cury, curx + between_sbraces, ']');
    wmove(win, cury, curx + between_sbraces - (int)strlen(inside_braces_right));
    wprintw(win, "%s", inside_braces_right);
    mvwchgat(win, cury, curx, represent_usage, 0, green_color, NULL);
  }
  wnoutrefresh(win);
}

static const char *memory_prefix[] = {" B", "Ki", "Mi", "Gi", "Ti", "Pi"};

static void draw_temp_color(WINDOW *win, unsigned int temp, unsigned int temp_slowdown, bool celsius) {
  unsigned int temp_convert;
  if (celsius)
    temp_convert = temp;
  else
    temp_convert = (unsigned)(32 + nearbyint(temp * 1.8));
  werase(win);
  set_chrome(win, label_color);
  wprintw(win, "TEMP");
  unset_chrome(win);

  if (temp >= temp_slowdown - 5) {
    if (temp >= temp_slowdown)
      wcolor_set(win, red_color, NULL);
    else
      wcolor_set(win, yellow_color, NULL);
  } else {
    wcolor_set(win, green_color, NULL);
  }
  wattron(win, A_BOLD);
  wprintw(win, " %3u", temp_convert);
  wattroff(win, A_BOLD);
  wstandend(win);

  if (interface_unicode)
    waddstr(win, "\xc2\xb0"); // °
  else
    waddch(win, ACS_DEGREE);
  if (celsius)
    waddch(win, 'C');
  else
    waddch(win, 'F');
  wnoutrefresh(win);
}

static inline void werase_and_wnoutrefresh(WINDOW *w) {
  werase(w);
  wnoutrefresh(w);
}

static bool cleaned_enc_window(struct device_window *dev, double encode_decode_hiding_timer, nvtop_time tnow) {
  if (encode_decode_hiding_timer > 0. && nvtop_difftime(dev->last_encode_seen, tnow) > encode_decode_hiding_timer) {
    if (dev->enc_was_visible) {
      dev->enc_was_visible = false;
      if (dev->dec_was_visible) {
        werase_and_wnoutrefresh(dev->gpu_util_enc_dec);
      } else {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_or_dec);
      }
    }
    return true;
  } else {
    return false;
  }
}

static bool cleaned_dec_window(struct device_window *dev, double encode_decode_hiding_timer, nvtop_time tnow) {
  if (encode_decode_hiding_timer > 0. && nvtop_difftime(dev->last_decode_seen, tnow) > encode_decode_hiding_timer) {
    if (dev->dec_was_visible) {
      dev->dec_was_visible = false;
      if (dev->enc_was_visible) {
        werase_and_wnoutrefresh(dev->gpu_util_enc_dec);
      } else {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_or_dec);
      }
    }
    return true;
  } else {
    return false;
  }
}

static void encode_decode_show_select(struct device_window *dev, bool encode_valid, bool decode_valid,
                                      unsigned encode_rate, unsigned decode_rate, double encode_decode_hiding_timer,
                                      bool encode_decode_shared, bool *display_encode, bool *display_decode) {
  nvtop_time tnow;
  nvtop_get_current_time(&tnow);
  if (encode_valid && encode_rate > 0) {
    *display_encode = true;
    dev->last_encode_seen = tnow;
    if (!dev->enc_was_visible) {
      dev->enc_was_visible = true;
      if (!dev->dec_was_visible) {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_and_dec);
      } else {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_or_dec);
      }
    }
  } else {
    *display_encode = !cleaned_enc_window(dev, encode_decode_hiding_timer, tnow);
  }
  // If shared, rely on decode
  *display_encode = *display_encode && !encode_decode_shared;
  if (decode_valid && decode_rate > 0) {
    *display_decode = true;
    dev->last_decode_seen = tnow;
    if (!dev->dec_was_visible) {
      dev->dec_was_visible = true;
      if (!dev->enc_was_visible) {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_and_dec);
      } else {
        werase_and_wnoutrefresh(dev->gpu_util_no_enc_or_dec);
      }
    }
  } else {
    *display_decode = !cleaned_dec_window(dev, encode_decode_hiding_timer, tnow);
  }
}

static void draw_devices(struct list_head *devices, struct nvtop_interface *interface) {
  struct gpu_info *device;
  unsigned dev_id = 0;
  // Compact mode keeps the standalone corner gear (index 0) alive.
  interface->gear_count = interface->gear_window ? 1 : 0;

  list_for_each_entry(device, devices, list) {
    struct device_window *dev = &interface->devices_win[dev_id];

    bool display_encode = false;
    bool display_decode = false;
    encode_decode_show_select(dev, GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, encoder_rate),
                              GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, decoder_rate),
                              device->dynamic_info.encoder_rate, device->dynamic_info.decoder_rate,
                              interface->options.encode_decode_hiding_timer, device->static_info.encode_decode_shared,
                              &display_encode, &display_decode);

    WINDOW *gpu_util_win;
    WINDOW *encode_win = dev->encode_util;
    WINDOW *decode_win = dev->decode_util;
    if ((display_encode && display_decode) || (display_decode && device->static_info.encode_decode_shared)) {
      gpu_util_win = dev->gpu_util_enc_dec;
      if (device->static_info.encode_decode_shared)
        decode_win = dev->encdec_util;
    } else {
      if (display_encode || display_decode) {
        // If encode only, place at decode location
        encode_win = dev->decode_util;
        gpu_util_win = dev->gpu_util_no_enc_or_dec;
      } else {
        gpu_util_win = dev->gpu_util_no_enc_and_dec;
      }
    }
    // Standalone always-visible gear in the screen's top-right corner.
    if (interface->gear_window) {
      werase(interface->gear_window);
      wattr_set(interface->gear_window, A_BOLD, interface_use_color ? cyan_color : 0, NULL);
      if (interface_unicode)
        mvwaddstr(interface->gear_window, 0, 0, " \xe2\x9a\x99 "); // ⚙
      else
        mvwaddstr(interface->gear_window, 0, 0, "[S]");
      wattr_set(interface->gear_window, A_NORMAL, 0, NULL);
      wnoutrefresh(interface->gear_window);
    }
    char buff[1024];
    if (display_encode) {
      unsigned rate =
          GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, encoder_rate) ? device->dynamic_info.encoder_rate : 0;
      snprintf(buff, 1024, "%u%%", rate);
      draw_percentage_meter(encode_win, "ENC", rate, buff);
    }
    if (display_decode) {
      unsigned rate =
          GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, decoder_rate) ? device->dynamic_info.decoder_rate : 0;
      snprintf(buff, 1024, "%u%%", rate);
      if (device->static_info.encode_decode_shared)
        draw_percentage_meter(decode_win, "ENC/DEC", rate, buff);
      else
        draw_percentage_meter(decode_win, "DEC", rate, buff);
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_util_rate)) {
      if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, effective_load_rate)) {
        // The effective load stays visible in the value badge; the bar itself
        // uses the same threshold coloring as the MEM meter below.
        snprintf(buff, 1024, "%u%%(eff %u%%)", device->dynamic_info.gpu_util_rate,
                 device->dynamic_info.effective_load_rate);
      } else {
        snprintf(buff, 1024, "%u%%", device->dynamic_info.gpu_util_rate);
      }
      draw_percentage_meter(gpu_util_win, "GPU", device->dynamic_info.gpu_util_rate, buff);
    } else {
      snprintf(buff, 1024, "N/A");
      draw_percentage_meter(gpu_util_win, "GPU", 0, buff);
    }

    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, total_memory) &&
        GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, used_memory)) {
      double total_mem = device->dynamic_info.total_memory;
      double used_mem = device->dynamic_info.used_memory;
      double total_prefixed = total_mem, used_prefixed = used_mem;
      size_t prefix_off;
      for (prefix_off = 0; prefix_off < 5 && total_prefixed >= 1000.; ++prefix_off) {
        total_prefixed /= 1024.;
        used_prefixed /= 1024.;
      }
      snprintf(buff, 1024, "%.3f%s/%.3f%s", used_prefixed, memory_prefix[prefix_off], total_prefixed,
               memory_prefix[prefix_off]);
      draw_percentage_meter(dev->mem_util, "MEM", (unsigned int)(100. * used_mem / total_mem), buff);
    } else if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, total_memory)) {
      double total_mem = device->dynamic_info.total_memory;
      double total_prefixed = total_mem;
      size_t prefix_off;
      for (prefix_off = 0; prefix_off < 5 && total_prefixed >= 1000.; ++prefix_off) {
        total_prefixed /= 1024.;
      }
      snprintf(buff, 1024, "N/A/%.3f%s", total_prefixed, memory_prefix[prefix_off]);
      draw_percentage_meter(dev->mem_util, "MEM", 0, buff);
    } else {
      snprintf(buff, 1024, "N/A");
      draw_percentage_meter(dev->mem_util, "MEM", 0, buff);
    }
    if (dev->temperature && GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_temp)) {
      if (!GPUINFO_STATIC_FIELD_VALID(&device->static_info, temperature_slowdown_threshold))
        device->static_info.temperature_slowdown_threshold = 0;
      draw_temp_color(dev->temperature, device->dynamic_info.gpu_temp,
                      device->static_info.temperature_slowdown_threshold,
                      !interface->options.temperature_in_fahrenheit);
    } else if (dev->temperature) {
      werase(dev->temperature);
      set_chrome(dev->temperature, label_color);
      wprintw(dev->temperature, "TEMP N/A");
      unset_chrome(dev->temperature);
      if (interface_unicode)
        waddstr(dev->temperature, "\xc2\xb0"); // °
      else
        waddch(dev->temperature, ACS_DEGREE);
      if (interface->options.temperature_in_fahrenheit)
        waddch(dev->temperature, 'F');
      else
        waddch(dev->temperature, 'C');
      wnoutrefresh(dev->temperature);
    }

    // FAN
    if (dev->fan_speed) {
    werase(dev->fan_speed);
    set_chrome(dev->fan_speed, label_color);
    wprintw(dev->fan_speed, "FAN ");
    unset_chrome(dev->fan_speed);
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, fan_speed)) {
      wattron(dev->fan_speed, A_BOLD);
      wprintw(dev->fan_speed, "%3u%%",
              device->dynamic_info.fan_speed > 100 ? 100 : device->dynamic_info.fan_speed);
      wattroff(dev->fan_speed, A_BOLD);
    } else if (device->static_info.integrated_graphics) {
      wprintw(dev->fan_speed, "CPU");
    } else if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, fan_rpm)) {
      wprintw(dev->fan_speed, "%4uRPM",
              device->dynamic_info.fan_rpm > 9999 ? 9999 : device->dynamic_info.fan_rpm);
    } else {
      wprintw(dev->fan_speed, "N/A");
    }
    wnoutrefresh(dev->fan_speed);
    }

    // GPU CLOCK
    if (dev->gpu_clock_info) {
    werase(dev->gpu_clock_info);
    set_chrome(dev->gpu_clock_info, label_color);
    wprintw(dev->gpu_clock_info, "GPU ");
    unset_chrome(dev->gpu_clock_info);
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_clock_speed))
      wprintw(dev->gpu_clock_info, "%uMHz", device->dynamic_info.gpu_clock_speed);
    else
      wprintw(dev->gpu_clock_info, "N/A");
    wnoutrefresh(dev->gpu_clock_info);
    }

    // MEM CLOCK
    if (dev->mem_clock_info) {
    werase(dev->mem_clock_info);
    set_chrome(dev->mem_clock_info, label_color);
    wprintw(dev->mem_clock_info, "MEM ");
    unset_chrome(dev->mem_clock_info);
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_clock_speed))
      wprintw(dev->mem_clock_info, "%uMHz", device->dynamic_info.mem_clock_speed);
    else
      wprintw(dev->mem_clock_info, "N/A");
    wnoutrefresh(dev->mem_clock_info);
    }

    // POWER — the live draw is bold and colored by headroom, the max stays
    // a plain reference.
    if (dev->power_info) {
    werase(dev->power_info);
    set_chrome(dev->power_info, label_color);
    wprintw(dev->power_info, "POW ");
    unset_chrome(dev->power_info);
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw) &&
        GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw_max)) {
      wattron(dev->power_info, A_BOLD);
      wprintw(dev->power_info, "%3u", device->dynamic_info.power_draw / 1000);
      wattroff(dev->power_info, A_BOLD);
      set_chrome(dev->power_info, label_color);
      wprintw(dev->power_info, "/");
      unset_chrome(dev->power_info);
      wprintw(dev->power_info, "%3uW", device->dynamic_info.power_draw_max / 1000);
      unsigned ratio = device->dynamic_info.power_draw_max > 0
                           ? device->dynamic_info.power_draw * 100 / device->dynamic_info.power_draw_max
                           : 0;
      short pair = ratio >= 85 ? red_color : (ratio >= 60 ? yellow_color : green_color);
      mvwchgat(dev->power_info, 0, 4, 3, A_BOLD, interface_use_color ? pair : 0, NULL);
    } else if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw)) {
      wattron(dev->power_info, A_BOLD);
      wprintw(dev->power_info, "%3uW", device->dynamic_info.power_draw / 1000);
      wattroff(dev->power_info, A_BOLD);
    } else if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw_max)) {
      wprintw(dev->power_info, "N/A/%3uW", device->dynamic_info.power_draw_max / 1000);
    } else {
      wprintw(dev->power_info, "N/A");
    }
    wnoutrefresh(dev->power_info);
    }

    if (interface->options.has_gpu_info_bar) {
      // Number of shader cores
      werase(dev->shader_cores);
      set_chrome(dev->shader_cores, label_color);
      mvwprintw(dev->shader_cores, 0, 0, "NSHC ");
      unset_chrome(dev->shader_cores);
      if (GPUINFO_STATIC_FIELD_VALID(&device->static_info, n_shared_cores))
        wprintw(dev->shader_cores, "%u", device->static_info.n_shared_cores);
      else
        wprintw(dev->shader_cores, "N/A");

      wnoutrefresh(dev->shader_cores);

      // L2 cache information
      werase(dev->l2_cache_size);
      set_chrome(dev->l2_cache_size, label_color);
      mvwprintw(dev->l2_cache_size, 0, 0, "L2CF ");
      unset_chrome(dev->l2_cache_size);
      if (GPUINFO_STATIC_FIELD_VALID(&device->static_info, l2cache_size))
        wprintw(dev->l2_cache_size, "%u", device->static_info.l2cache_size);
      else
        wprintw(dev->l2_cache_size, "N/A");

      wnoutrefresh(dev->l2_cache_size);

      // Number of execution engines
      werase(dev->exec_engines);
      set_chrome(dev->exec_engines, label_color);
      mvwprintw(dev->exec_engines, 0, 0, "NEXC ");
      unset_chrome(dev->exec_engines);
      if (GPUINFO_STATIC_FIELD_VALID(&device->static_info, n_exec_engines))
        wprintw(dev->exec_engines, "%u", device->static_info.n_exec_engines);
      else
        wprintw(dev->exec_engines, "N/A");

      wnoutrefresh(dev->exec_engines);
    }

    dev_id++;
  }
}

typedef struct {
  unsigned processes_count;
  struct gpuid_and_process *processes;
} all_processes;

static all_processes all_processes_array(struct list_head *devices) {
  unsigned total_processes_count = 0;
  struct gpu_info *device;
  unsigned dev_id = 0;

  list_for_each_entry(device, devices, list) { total_processes_count += device->processes_count; }

  all_processes merged_devices_processes;
  merged_devices_processes.processes_count = total_processes_count;
  if (total_processes_count) {
    merged_devices_processes.processes = malloc(total_processes_count * sizeof(*merged_devices_processes.processes));
    if (!merged_devices_processes.processes) {
      perror("Cannot allocate memory: ");
      exit(EXIT_FAILURE);
    }
  } else {
    merged_devices_processes.processes = NULL;
  }

  size_t offset = 0;
  list_for_each_entry(device, devices, list) {
    for (unsigned int j = 0; j < device->processes_count; ++j) {
      merged_devices_processes.processes[offset].gpu_id = dev_id;
      merged_devices_processes.processes[offset++].process = &device->processes[j];
    }

    dev_id++;
  }

  return merged_devices_processes;
}

static int compare_pid_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  return p1->process->pid >= p2->process->pid ? -1 : 1;
}

static int compare_pid_asc(const void *pp1, const void *pp2) { return compare_pid_desc(pp2, pp1); }

static int compare_username_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, user_name) && GPUINFO_PROCESS_FIELD_VALID(p2->process, user_name))
    return -strcmp(p1->process->user_name, p2->process->user_name);
  else
    return 0;
}

static int compare_username_asc(const void *pp1, const void *pp2) { return compare_username_desc(pp2, pp1); }

static int compare_process_name_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, cmdline) && GPUINFO_PROCESS_FIELD_VALID(p2->process, cmdline))
    return -strcmp(p1->process->cmdline, p2->process->cmdline);
  else
    return 0;
}

static int compare_process_name_asc(const void *pp1, const void *pp2) { return compare_process_name_desc(pp2, pp1); }

static int compare_mem_usage_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, gpu_memory_usage) &&
      GPUINFO_PROCESS_FIELD_VALID(p2->process, gpu_memory_usage))
    return p1->process->gpu_memory_usage >= p2->process->gpu_memory_usage ? -1 : 1;
  else
    return 0;
}

static int compare_mem_usage_asc(const void *pp1, const void *pp2) { return compare_mem_usage_desc(pp2, pp1); }

static int compare_cpu_usage_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, cpu_usage) && GPUINFO_PROCESS_FIELD_VALID(p2->process, cpu_usage))
    return p1->process->cpu_usage >= p2->process->cpu_usage ? -1 : 1;
  else
    return 0;
}

static int compare_cpu_usage_asc(const void *pp1, const void *pp2) { return compare_cpu_usage_desc(pp2, pp1); }

static int compare_cpu_mem_usage_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, cpu_memory_res) &&
      GPUINFO_PROCESS_FIELD_VALID(p2->process, cpu_memory_res))
    return p1->process->cpu_memory_res >= p2->process->cpu_memory_res ? -1 : 1;
  else
    return 0;
}

static int compare_cpu_mem_usage_asc(const void *pp1, const void *pp2) { return compare_cpu_mem_usage_desc(pp2, pp1); }

static int compare_gpu_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  return p1->gpu_id >= p2->gpu_id ? -1 : 1;
}

static int compare_gpu_asc(const void *pp1, const void *pp2) { return -compare_gpu_desc(pp1, pp2); }

static int compare_process_type_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  return (p1->process->type == gpu_process_graphical) != (p2->process->type == gpu_process_graphical);
}

static int compare_process_type_asc(const void *pp1, const void *pp2) { return -compare_process_type_desc(pp1, pp2); }

static int compare_process_gpu_rate_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, gpu_usage) && GPUINFO_PROCESS_FIELD_VALID(p2->process, gpu_usage)) {
    return p1->process->gpu_usage > p2->process->gpu_usage ? -1 : 1;
  } else {
    if (GPUINFO_PROCESS_FIELD_VALID(p1->process, gpu_usage)) {
      return p1->process->gpu_usage > 0 ? -1 : 0;
    } else if (GPUINFO_PROCESS_FIELD_VALID(p2->process, gpu_usage)) {
      return p2->process->gpu_usage > 0 ? 1 : 0;
    } else {
      return 0;
    }
  }
}

static int compare_process_gpu_rate_asc(const void *pp1, const void *pp2) {
  return -compare_process_gpu_rate_desc(pp1, pp2);
}

static int compare_process_enc_rate_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, encode_usage) &&
      GPUINFO_PROCESS_FIELD_VALID(p2->process, encode_usage)) {
    return p1->process->encode_usage >= p2->process->encode_usage ? -1 : 1;
  } else {
    if (GPUINFO_PROCESS_FIELD_VALID(p1->process, encode_usage)) {
      return p1->process->encode_usage > 0 ? -1 : 0;
    } else if (GPUINFO_PROCESS_FIELD_VALID(p2->process, encode_usage)) {
      return p2->process->encode_usage > 0 ? 1 : 0;
    } else {
      return 0;
    }
  }
}

static int compare_process_enc_rate_asc(const void *pp1, const void *pp2) {
  return -compare_process_enc_rate_desc(pp1, pp2);
}

static int compare_process_dec_rate_desc(const void *pp1, const void *pp2) {
  const struct gpuid_and_process *p1 = (const struct gpuid_and_process *)pp1;
  const struct gpuid_and_process *p2 = (const struct gpuid_and_process *)pp2;
  if (GPUINFO_PROCESS_FIELD_VALID(p1->process, decode_usage) &&
      GPUINFO_PROCESS_FIELD_VALID(p2->process, decode_usage)) {
    return p1->process->decode_usage >= p2->process->decode_usage ? -1 : 1;
  } else {
    if (GPUINFO_PROCESS_FIELD_VALID(p1->process, decode_usage)) {
      return p1->process->decode_usage > 0 ? -1 : 0;
    } else if (GPUINFO_PROCESS_FIELD_VALID(p2->process, decode_usage)) {
      return p2->process->decode_usage > 0 ? 1 : 0;
    } else {
      return 0;
    }
  }
}
static int compare_process_dec_rate_asc(const void *pp1, const void *pp2) {
  return -compare_process_dec_rate_desc(pp1, pp2);
}

static void sort_process(all_processes all_procs, enum process_field criterion, bool asc_sort) {
  if (all_procs.processes_count == 0 || !all_procs.processes)
    return;
  int (*sort_fun)(const void *, const void *);
  switch (criterion) {
  case process_pid:
    if (asc_sort)
      sort_fun = compare_pid_asc;
    else
      sort_fun = compare_pid_desc;
    break;
  case process_user:
    if (asc_sort)
      sort_fun = compare_username_asc;
    else
      sort_fun = compare_username_desc;
    break;
  case process_gpu_id:
    if (asc_sort)
      sort_fun = compare_gpu_asc;
    else
      sort_fun = compare_gpu_desc;
    break;
  case process_type:
    if (asc_sort)
      sort_fun = compare_process_type_asc;
    else
      sort_fun = compare_process_type_desc;
    break;
  case process_memory:
    if (asc_sort)
      sort_fun = compare_mem_usage_asc;
    else
      sort_fun = compare_mem_usage_desc;
    break;
  case process_command:
    if (asc_sort)
      sort_fun = compare_process_name_asc;
    else
      sort_fun = compare_process_name_desc;
    break;
  case process_cpu_usage:
    if (asc_sort)
      sort_fun = compare_cpu_usage_asc;
    else
      sort_fun = compare_cpu_usage_desc;
    break;
  case process_cpu_mem_usage:
    if (asc_sort)
      sort_fun = compare_cpu_mem_usage_asc;
    else
      sort_fun = compare_cpu_mem_usage_desc;
    break;
  case process_gpu_rate:
    if (asc_sort)
      sort_fun = compare_process_gpu_rate_asc;
    else
      sort_fun = compare_process_gpu_rate_desc;
    break;
  case process_enc_rate:
    if (asc_sort)
      sort_fun = compare_process_enc_rate_asc;
    else
      sort_fun = compare_process_enc_rate_desc;
    break;
  case process_dec_rate:
    if (asc_sort)
      sort_fun = compare_process_dec_rate_asc;
    else
      sort_fun = compare_process_dec_rate_desc;
    break;
  case process_field_count:
    return;
  }
  qsort(all_procs.processes, all_procs.processes_count, sizeof(*all_procs.processes), sort_fun);
}

static void filter_out_nvtop_pid(all_processes *all_procs, struct nvtop_interface *interface) {
  if (interface->options.filter_nvtop_pid) {
    for (unsigned procId = 0; procId < all_procs->processes_count; ++procId) {
      if (all_procs->processes[procId].process->pid == nvtop_pid) {
        memmove(&all_procs->processes[procId], &all_procs->processes[procId + 1],
                (all_procs->processes_count - procId - 1) * sizeof(*all_procs->processes));
        all_procs->processes_count = all_procs->processes_count - 1;
        break;
      }
    }
  }
}

static const char *columnName[process_field_count] = {
    "PID", "USER", "DEV", "TYPE", "GPU", "ENC", "DEC", "GPU MEM", "CPU", "HOST MEM", "Command",
};

static void update_selected_offset_with_window_size(unsigned int *selected_row, unsigned int *offset,
                                                    unsigned int row_available_to_draw, unsigned int num_to_draw) {

  if (!num_to_draw)
    return;

  if (*selected_row > num_to_draw - 1)
    *selected_row = num_to_draw - 1;

  if (*offset > *selected_row)
    *offset = *selected_row;

  if (*offset + row_available_to_draw - 1 < *selected_row)
    *offset = *selected_row - row_available_to_draw + 1;

  while (row_available_to_draw > num_to_draw - *offset && *offset != 0)
    *offset -= 1;
}

#define process_buffer_line_size 8192
static char process_print_buffer[process_buffer_line_size];

static void print_processes_on_screen(all_processes all_procs, struct process_window *process,
                                      enum process_field sort_criterion, process_field_displayed fields_to_display,
                                      bool sort_descending) {
  WINDOW *win = process->option_window.state == nvtop_option_state_hidden ? process->process_win
                                                                          : process->process_with_option_win;
  struct gpuid_and_process *processes = all_procs.processes;

  unsigned int rows, cols;
  getmaxyx(win, rows, cols);
  rows -= 1;

  update_selected_offset_with_window_size(&process->selected_row, &process->offset, rows, all_procs.processes_count);
  if (process->offset_column + cols >= process_buffer_line_size)
    process->offset_column = process_buffer_line_size - cols - 1;

  size_t special_row = process->selected_row;

  char pid_str[sizeof_process_field[process_pid] + 1];
  char guid_str[sizeof_process_field[process_gpu_id] + 1];
  char memory[sizeof_process_field[process_memory] + 1];
  char cpu_percent[sizeof_process_field[process_cpu_usage] + 1];
  char cpu_mem[sizeof_process_field[process_cpu_mem_usage] + 1];

  unsigned int start_at_process = process->offset;
  unsigned int end_at_process = start_at_process + rows;

  int printed = 0;
  int column_sort_start = 0, column_sort_end = sizeof_process_field[0];
  const char *sort_arrow =
      interface_unicode ? (sort_descending ? "\xe2\x96\xbc" : "\xe2\x96\xb2") : (sort_descending ? "v" : "^");
  int sort_arrow_col = -1;
  for (enum process_field i = process_pid; i < process_field_count; ++i) {
    if (i == sort_criterion) {
      column_sort_start = printed;
      column_sort_end =
          i == process_command ? process_buffer_line_size - 4 : column_sort_start + sizeof_process_field[i];
    }
    if (!process_is_field_displayed(i, fields_to_display))
      continue;
    if (i == sort_criterion && i != process_command) {
      // Sort direction indicator glued to the sorted column name, kept
      // inside the field width so data columns stay aligned.
      char with_arrow[64];
      int width = sizeof_process_field[i];
      int name_len = (int)strlen(columnName[i]);
      int arrow_len = (int)strlen(sort_arrow);
      if (name_len + 1 + arrow_len <= width) {
        snprintf(with_arrow, sizeof(with_arrow), "%s %s", columnName[i], sort_arrow);
        sort_arrow_col = printed + name_len + 1;
      } else if (name_len + arrow_len <= width) {
        snprintf(with_arrow, sizeof(with_arrow), "%s%s", columnName[i], sort_arrow);
        sort_arrow_col = printed + name_len;
      } else {
        snprintf(with_arrow, sizeof(with_arrow), "%s", columnName[i]);
      }
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ", width,
                          with_arrow);
    } else {
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[i], columnName[i]);
    }
  }

  mvwprintw(win, 0, 0, "%.*s", cols, &process_print_buffer[process->offset_column]);
  wclrtoeol(win);
  // Quiet header: bold gray titles, bright arrow on the sorted column.
  mvwchgat(win, 0, 0, -1, A_BOLD, interface_use_color ? label_color : 0, NULL);
  if (sort_arrow_col >= 0)
    set_attribute_between(win, 0, sort_arrow_col - (int)process->offset_column,
                          sort_arrow_col + (int)strlen(sort_arrow) - (int)process->offset_column, A_BOLD,
                          cyan_color);

  int start_col_process_type = 0;
  for (enum process_field i = process_pid; i < process_type; ++i) {
    if (process_is_field_displayed(i, fields_to_display))
      start_col_process_type += sizeof_process_field[i] + 1;
  }
  int end_col_process_type = start_col_process_type + sizeof_process_field[process_type];

  static unsigned printed_last_call = 0;
  unsigned last_line_printed = 0;
  for (unsigned int i = start_at_process; i < end_at_process && i < all_procs.processes_count; ++i) {
    // The buffer is rebuilt from scratch every line and every snprintf into
    // it null-terminates: no need to zero the 8 KiB first.

    printed = 0;
    if (process_is_field_displayed(process_pid, fields_to_display)) {
      size_t size =
          snprintf(pid_str, sizeof_process_field[process_pid] + 1, "%" PRIdMAX, (intmax_t)processes[i].process->pid);
      if (size == sizeof_process_field[process_pid] + 1)
        pid_str[sizeof_process_field[process_pid]] = '\0';
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size, "%*s ",
                          sizeof_process_field[process_pid], pid_str);
    }

    if (process_is_field_displayed(process_user, fields_to_display)) {
      const char *username;
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, user_name)) {
        username = processes[i].process->user_name;
      } else {
        username = "N/A";
      }

      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[process_user], username);
    }

    if (process_is_field_displayed(process_gpu_id, fields_to_display)) {
      size_t size = snprintf(guid_str, sizeof_process_field[process_gpu_id] + 1, "%u", processes[i].gpu_id);
      if (size >= sizeof_process_field[process_gpu_id] + 1)
        guid_str[sizeof_process_field[process_gpu_id]] = '\0';
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[process_gpu_id], guid_str);
    }

    if (process_is_field_displayed(process_type, fields_to_display)) {
      if (processes[i].process->type == gpu_process_graphical_compute) {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                            sizeof_process_field[process_type], "Both G+C");
      } else if (processes[i].process->type == gpu_process_graphical) {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                            sizeof_process_field[process_type], "Graphic");
      } else {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                            sizeof_process_field[process_type], "Compute");
      }
    }

    if (process_is_field_displayed(process_gpu_rate, fields_to_display)) {
      unsigned gpu_usage = 0;
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, gpu_usage)) {
        gpu_usage = processes[i].process->gpu_usage;
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%3u%% ", gpu_usage);
      } else {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "N/A  ");
      }
    }

    if (process_is_field_displayed(process_enc_rate, fields_to_display)) {
      unsigned encoder_rate = 0;
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, encode_usage)) {
        encoder_rate = processes[i].process->encode_usage;
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%3u%% ", encoder_rate);
      } else {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "N/A  ");
      }
    }

    if (process_is_field_displayed(process_dec_rate, fields_to_display)) {
      unsigned decode_rate = 0;
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, decode_usage)) {
        decode_rate = processes[i].process->decode_usage;
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%3u%% ", decode_rate);
      } else {
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "N/A  ");
      }
    }

    if (process_is_field_displayed(process_memory, fields_to_display)) {
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, gpu_memory_usage)) {
        if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, gpu_memory_percentage)) {
          snprintf(memory, 9 + 1, "%6uMiB", (unsigned)(processes[i].process->gpu_memory_usage / 1048576));
          snprintf(memory + 9, sizeof_process_field[process_memory] - 9 + 1, " %3u%%",
                   processes[i].process->gpu_memory_percentage);
        } else {
          snprintf(memory, sizeof_process_field[process_memory], "%6uMiB",
                   (unsigned)(processes[i].process->gpu_memory_usage / 1048576));
        }
      } else {
        snprintf(memory, sizeof_process_field[process_memory], "%s", "N/A");
      }
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[process_memory], memory);
    }

    if (process_is_field_displayed(process_cpu_usage, fields_to_display)) {
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, cpu_usage))
        snprintf(cpu_percent, sizeof_process_field[process_cpu_usage] + 1, "%u%%", processes[i].process->cpu_usage);
      else
        snprintf(cpu_percent, sizeof_process_field[process_cpu_usage] + 1, "   N/A");
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[process_cpu_usage], cpu_percent);
    }

    if (process_is_field_displayed(process_cpu_mem_usage, fields_to_display)) {
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, cpu_memory_res))
        snprintf(cpu_mem, sizeof_process_field[process_cpu_mem_usage] + 1, "%zuMiB",
                 processes[i].process->cpu_memory_res / 1048576);
      else
        snprintf(cpu_mem, sizeof_process_field[process_cpu_mem_usage] + 1, "N/A");
      printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%*s ",
                          sizeof_process_field[process_cpu_mem_usage], cpu_mem);
    }

    if (process_is_field_displayed(process_command, fields_to_display)) {
      if (GPUINFO_PROCESS_FIELD_VALID(processes[i].process, cmdline))
        printed += snprintf(&process_print_buffer[printed], process_buffer_line_size - printed, "%.*s",
                            process_buffer_line_size - printed, processes[i].process->cmdline);
    }

    unsigned int write_at = i - start_at_process + 1;
    mvwprintw(win, write_at, 0, "%.*s", cols, &process_print_buffer[process->offset_column]);
    unsigned row, col;
    getyx(win, row, col);
    (void)col;
    if (row == write_at)
      wclrtoeol(win);
    last_line_printed = write_at;
    if (i == special_row) {
      mvwchgat(win, write_at, 0, -1, A_STANDOUT, cyan_color, NULL);
    } else {
      if (process_is_field_displayed(process_type, fields_to_display)) {
        if (processes[i].process->type == gpu_process_graphical_compute) {
          set_attribute_between(win, write_at, start_col_process_type - (int)process->offset_column,
                                start_col_process_type - (int)process->offset_column + 4, 0, cyan_color);
          set_attribute_between(win, write_at, end_col_process_type - (int)process->offset_column - 3,
                                end_col_process_type - (int)process->offset_column - 2, 0, yellow_color);
          set_attribute_between(win, write_at, end_col_process_type - (int)process->offset_column - 1,
                                end_col_process_type - (int)process->offset_column, 0, magenta_color);
        } else if (processes[i].process->type == gpu_process_graphical) {
          set_attribute_between(win, write_at, start_col_process_type - (int)process->offset_column,
                                end_col_process_type - (int)process->offset_column, 0, yellow_color);
        } else {
          set_attribute_between(win, write_at, start_col_process_type - (int)process->offset_column,
                                end_col_process_type - (int)process->offset_column, 0, magenta_color);
        }
      }
    }
  }
  if (printed_last_call > last_line_printed) {
    for (unsigned i = last_line_printed + 1; i <= rows && i <= printed_last_call; ++i) {
      wmove(win, i, 0);
      wclrtoeol(win);
    }
  }
  printed_last_call = last_line_printed;
  if (all_procs.processes_count == 0) {
    set_chrome(win, label_color);
    mvwprintw(win, 1, 1, "No GPU processes");
    unset_chrome(win);
  }
  wnoutrefresh(win);
}

static void update_process_option_win(struct nvtop_interface *interface);

static void draw_processes(struct list_head *devices, struct nvtop_interface *interface) {
  if (interface->options.hide_processes_list)
    return;

  if (interface->process.process_win == NULL)
    return;

  if (interface->process.option_window.state != interface->process.option_window.previous_state) {
    werase(interface->process.option_window.option_win);
    wclear(interface->process.process_win);
    wclear(interface->process.process_with_option_win);
    wnoutrefresh(interface->process.option_window.option_win);
  }
  if (interface->process.option_window.state != nvtop_option_state_hidden)
    update_process_option_win(interface);

  // Rebuild the process array only when new data arrived; re-sort only when
  // the sort criterion or order changed. Navigation keys reuse the cache.
  if (!interface->process.cache_valid) {
    all_processes all_procs = all_processes_array(devices);
    filter_out_nvtop_pid(&all_procs, interface);

    unsigned largest_username = 4;
    for (unsigned i = 0; i < all_procs.processes_count; ++i) {
      if (GPUINFO_PROCESS_FIELD_VALID(all_procs.processes[i].process, user_name)) {
        unsigned length = strlen(all_procs.processes[i].process->user_name);
        if (length > largest_username)
          largest_username = length;
      }
    }
    sizeof_process_field[process_user] = largest_username;

    free(interface->process.cached_processes);
    interface->process.cached_processes = all_procs.processes;
    interface->process.cached_count = all_procs.processes_count;
    interface->process.cache_valid = true;
    interface->process.sort_valid = false;
  }

  if (!interface->process.sort_valid ||
      interface->process.cached_sort_by != interface->options.sort_processes_by ||
      interface->process.cached_sort_desc != interface->options.sort_descending_order) {
    all_processes cached = {interface->process.cached_count, interface->process.cached_processes};
    sort_process(cached, interface->options.sort_processes_by, !interface->options.sort_descending_order);
    interface->process.sort_valid = true;
    interface->process.cached_sort_by = interface->options.sort_processes_by;
    interface->process.cached_sort_desc = interface->options.sort_descending_order;
  }

  all_processes all_procs = {interface->process.cached_count, interface->process.cached_processes};

  if (all_procs.processes_count > 0) {
    if (interface->process.selected_row >= all_procs.processes_count)
      interface->process.selected_row = all_procs.processes_count - 1;
    interface->process.selected_pid = all_procs.processes[interface->process.selected_row].process->pid;
  } else {
    interface->process.selected_row = 0;
    interface->process.selected_pid = -1;
  }

  print_processes_on_screen(all_procs, &interface->process, interface->options.sort_processes_by,
                            interface->options.process_fields_displayed, interface->options.sort_descending_order);
}

static const char *signalNames[] = {
    "Cancel",  "SIGHUP",    "SIGINT",  "SIGQUIT",  "SIGILL",  "SIGTRAP", "SIGABRT", "SIGBUS",
    "SIGFPE",  "SIGKILL",   "SIGUSR1", "SIGSEGV",  "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM",
    "SIGCHLD", "SIGCONT",   "SIGSTOP", "SIGTSTP",  "SIGTTIN", "SIGTTOU", "SIGURG",  "SIGXCPU",
    "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO",   "SIGPWR",  "SIGSYS",
};

// SIGPWR does not exist on FreeBSD or Apple, while it is a synonym for SIGINFO on Linux
#if defined(__FreeBSD__) || defined(__APPLE__)
#define SIGPWR SIGINFO
#endif

static const int signalValues[ARRAY_SIZE(signalNames)] = {
    -1,      SIGHUP,  SIGINT,  SIGQUIT,   SIGILL,  SIGTRAP,  SIGABRT, SIGBUS,  SIGFPE,  SIGKILL, SIGUSR1,
    SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM,   SIGTERM, SIGCHLD,  SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU,
    SIGURG,  SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO,   SIGPWR,  SIGSYS,
};

static const size_t nvtop_num_signals = ARRAY_SIZE(signalNames) - 1;

static void draw_kill_option(struct nvtop_interface *interface) {
  WINDOW *win = interface->process.option_window.option_win;
  wattr_set(win, A_REVERSE, green_color, NULL);
  mvwprintw(win, 0, 0, "Send signal:");
  wstandend(win);
  wprintw(win, " ");
  int rows, cols;
  getmaxyx(win, rows, cols);

  size_t start_at_option = interface->process.option_window.offset;
  size_t end_at_option = start_at_option + rows - 1;

  for (size_t i = start_at_option; i < end_at_option && i <= nvtop_num_signals; ++i) {
    if (i == interface->process.option_window.selected_row) {
      wattr_set(win, A_STANDOUT, cyan_color, NULL);
    }
    wprintw(win, "%*zu %s", 2, i, signalNames[i]);
    getyx(win, rows, cols);

    for (unsigned int j = cols; j < option_window_size; ++j)
      wprintw(win, " ");
    if (i == interface->process.option_window.selected_row) {
      wstandend(win);
      mvwprintw(win, rows, option_window_size - 1, " ");
    }
  }
  wnoutrefresh(win);
}

static void draw_sort_option(struct nvtop_interface *interface) {
  WINDOW *win = interface->process.option_window.option_win;
  wattr_set(win, A_REVERSE, green_color, NULL);
  mvwprintw(win, 0, 0, "Sort by     ");
  wstandend(win);
  wprintw(win, " ");
  int rows, cols;
  if (interface->process.option_window.offset == 0) {
    if (interface->process.option_window.selected_row == 0) {
      wattr_set(win, A_STANDOUT, cyan_color, NULL);
    }
    wprintw(win, "Cancel");
    getyx(win, rows, cols);
    for (unsigned int j = cols; j < option_window_size; ++j)
      wprintw(win, " ");
    if (interface->process.option_window.selected_row == 0) {
      wstandend(win);
      mvwprintw(win, rows, option_window_size - 1, " ");
    }
  }
  getmaxyx(win, rows, cols);

  size_t start_at_option = interface->process.option_window.offset == 0 ? interface->process.option_window.offset
                                                                        : interface->process.option_window.offset - 1;
  size_t end_at_option =
      interface->process.option_window.offset == 0 ? start_at_option + rows - 2 : start_at_option + rows - 1;

  unsigned option_index = 0;
  for (enum process_field field = process_pid; field < process_field_count; ++field) {
    if (process_is_field_displayed(field, interface->options.process_fields_displayed)) {
      if (option_index >= start_at_option && option_index < end_at_option) {
        if (option_index + 1 == interface->process.option_window.selected_row) {
          wattr_set(win, A_STANDOUT, cyan_color, NULL);
        }
        wprintw(win, "%s", columnName[field]);
        getyx(win, rows, cols);
        for (unsigned int j = cols; j < option_window_size; ++j)
          wprintw(win, " ");

        if (option_index + 1 == interface->process.option_window.selected_row) {
          wstandend(win);
          mvwprintw(win, rows, option_window_size - 1, " ");
        }
      }
      option_index++;
    }
  }
  wnoutrefresh(win);
}

static void update_process_option_win(struct nvtop_interface *interface) {
  unsigned int rows, cols;
  getmaxyx(interface->process.option_window.option_win, rows, cols);
  rows -= 1;
  (void)cols;
  unsigned int num_options = 0;
  switch (interface->process.option_window.state) {
  case nvtop_option_state_kill:
    num_options = nvtop_num_signals + 1; // Option + Cancel
    break;
  case nvtop_option_state_sort_by:
    num_options = process_field_displayed_count(interface->options.process_fields_displayed) + 1; // Option + Cancel
    break;
  case nvtop_option_state_hidden:
  default:
    break;
  }

  update_selected_offset_with_window_size(&interface->process.option_window.selected_row,
                                          &interface->process.option_window.offset, rows, num_options);

  switch (interface->process.option_window.state) {
  case nvtop_option_state_kill:
    draw_kill_option(interface);
    break;
  case nvtop_option_state_sort_by:
    draw_sort_option(interface);
    break;
  case nvtop_option_state_hidden:
  default:
    break;
  }
}

static const char *option_selection_hidden[] = {
    "Setup", "Sort", "Kill", "Quit", "Save Config",
};

static const char *option_selection_hidden_num[] = {
    "2", "6", "9", "10", "12",
};

static const char *option_selection_sort[][2] = {
    {"Enter", "Sort"},
    {"ESC", "Cancel"},
    {"+", "Ascending"},
    {"-", "Descending"},
};

static const char *option_selection_kill[][2] = {
    {"Enter", "Send"},
    {"ESC", "Cancel"},
};

// One shortcut entry: bold cyan key, then dim label. Entries are separated
// by a dim middot; the sequence restarts on every full bar redraw.
static bool shortcut_bar_first_entry = true;
void nvtop_print_shortcut(WINDOW *win, const char *key, const char *label) {
  if (!shortcut_bar_first_entry) {
    if (interface_use_color)
      wcolor_set(win, track_color, NULL);
    else
      wattron(win, A_DIM);
    waddstr(win, " \xc2\xb7 "); // ·
    wstandend(win);
  }
  shortcut_bar_first_entry = false;
  if (interface_use_color)
    wcolor_set(win, cyan_color, NULL);
  wattron(win, A_BOLD);
  wprintw(win, "%s", key);
  wattroff(win, A_BOLD);
  wstandend(win);
  waddch(win, ' ');
  if (interface_use_color)
    wcolor_set(win, dim_color, NULL);
  else
    wattron(win, A_DIM);
  wprintw(win, "%s", label);
  if (!interface_use_color)
    wattroff(win, A_DIM);
  wstandend(win);
}

static void draw_process_shortcuts(struct nvtop_interface *interface) {
  if (interface->process.option_window.state == interface->process.option_window.previous_state)
    return;
  WINDOW *win = interface->shortcut_window;
  enum nvtop_option_window_state current_state = interface->process.option_window.state;
  wmove(win, 0, 0);
  switch (current_state) {
  case nvtop_option_state_hidden:
    for (size_t i = 0; i < ARRAY_SIZE(option_selection_hidden); ++i) {
      if (interface->options.hide_processes_list &&
          (strcmp(option_selection_hidden_num[i], "6") == 0 || strcmp(option_selection_hidden_num[i], "9") == 0))
        continue;

      if (process_field_displayed_count(interface->options.process_fields_displayed) > 0 || (i != 1 && i != 2)) {
        char key[8];
        snprintf(key, sizeof(key), "F%s", option_selection_hidden_num[i]);
        nvtop_print_shortcut(win, key, option_selection_hidden[i]);
      }
    }
    break;
  case nvtop_option_state_kill:
    for (size_t i = 0; i < ARRAY_SIZE(option_selection_kill); ++i)
      nvtop_print_shortcut(win, option_selection_kill[i][0], option_selection_kill[i][1]);
    break;
  case nvtop_option_state_sort_by:
    for (size_t i = 0; i < ARRAY_SIZE(option_selection_sort); ++i)
      nvtop_print_shortcut(win, option_selection_sort[i][0], option_selection_sort[i][1]);
    break;
  default:
    break;
  }
  wclrtoeol(win);
  wnoutrefresh(win);
  interface->process.option_window.previous_state = current_state;
}

static void draw_shortcuts(struct nvtop_interface *interface) {
  if (!interface->options.show_shortcut_bar || interface->shortcut_window == NULL) {
    interface->process.option_window.previous_state = interface->process.option_window.state;
    return;
  }
  shortcut_bar_first_entry = true;
  if (interface->setup_win.visible) {
    draw_setup_window_shortcuts(interface);
  } else {
    draw_process_shortcuts(interface);
  }
}

void save_current_data_to_ring(struct list_head *devices, struct nvtop_interface *interface) {
  struct gpu_info *device;
  unsigned dev_id = 0;

  // Fresh data just landed: the data-driven sections need a repaint and the
  // cached process list is stale.
  interface->devices_dirty = true;
  interface->process_dirty = true;
  interface->process.cache_valid = false;

  list_for_each_entry(device, devices, list) {
    unsigned data_index = 0;
    for (enum plot_information info = plot_gpu_rate; info < plot_information_count; ++info) {
      if (plot_isset_draw_info(info, interface->options.gpu_specific_opts[dev_id].to_draw)) {
        unsigned data_val = 0;
        switch (info) {
        case plot_gpu_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_util_rate))
            data_val = device->dynamic_info.gpu_util_rate;
          break;
        case plot_gpu_mem_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_util_rate))
            data_val = device->dynamic_info.mem_util_rate;
          break;
        case plot_encoder_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, encoder_rate))
            data_val = device->dynamic_info.encoder_rate;
          break;
        case plot_decoder_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, decoder_rate))
            data_val = device->dynamic_info.decoder_rate;
          break;
        case plot_gpu_temperature:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_temp)) {
            data_val = device->dynamic_info.gpu_temp;
            if (data_val > 100)
              data_val = 100u;
          }
          break;
        case plot_gpu_power_draw_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw) &&
              GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw_max)) {
            data_val = device->dynamic_info.power_draw * 100 / device->dynamic_info.power_draw_max;
            if (data_val > 100)
              data_val = 100u;
          }
          break;
        case plot_fan_speed:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, fan_speed)) {
            data_val = device->dynamic_info.fan_speed;
          }
          break;
        case plot_gpu_clock_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_clock_speed) &&
              GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_clock_speed_max)) {
            data_val = device->dynamic_info.gpu_clock_speed * 100 / device->dynamic_info.gpu_clock_speed_max;
          }
          break;
        case plot_gpu_mem_clock_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_clock_speed) &&
              GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_clock_speed_max)) {
            data_val = device->dynamic_info.mem_clock_speed * 100 / device->dynamic_info.mem_clock_speed_max;
          }
          break;
        case plot_effective_load_rate:
          if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, effective_load_rate)) {
            data_val = device->dynamic_info.effective_load_rate;
          }
          break;
        case plot_information_count:
          break;
        }
        interface_ring_buffer_push(&interface->saved_data_ring, dev_id, data_index, data_val);
        data_index++;
      }
    }

    dev_id++;
  }
}

static unsigned populate_plot_data_from_ring_buffer(const struct nvtop_interface *interface,
                                                    struct plot_window *plot_win, unsigned size_data_buff,
                                                    double data[size_data_buff],
                                                    char plot_legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {

  unsigned total_to_draw = 0;
  for (unsigned i = 0; i < plot_win->num_devices_to_plot; ++i) {
    unsigned dev_id = plot_win->devices_ids[i];
    plot_info_to_draw to_draw = interface->options.gpu_specific_opts[dev_id].to_draw;
    total_to_draw += plot_count_draw_info(to_draw);
  }

  assert(total_to_draw > 0);
  // size_data_buff holds num_samples * MAX_LINES_PER_PLOT values; every
  // plotted sample occupies total_to_draw consecutive slots (the renderers
  // index data[sample * num_lines + series]). Dividing by the series count
  // here halved the plotted history for multi-series graphs and left the
  // right half of the chart as uninitialized zeros.
  assert(size_data_buff >= total_to_draw);
  unsigned max_data_to_copy = size_data_buff / MAX_LINES_PER_PLOT;
  double (*data_split)[total_to_draw] = (double (*)[total_to_draw])data;

  unsigned in_processing = 0;
  for (unsigned i = 0; i < plot_win->num_devices_to_plot; ++i) {
    unsigned dev_id = plot_win->devices_ids[i];
    plot_info_to_draw to_draw = interface->options.gpu_specific_opts[dev_id].to_draw;
    unsigned data_ring_index = 0;
    for (enum plot_information info = plot_gpu_rate; info < plot_information_count; ++info) {
      if (plot_isset_draw_info(info, to_draw)) {
        // Populate the legend
        switch (info) {
        case plot_gpu_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u %%", dev_id);
          break;
        case plot_gpu_mem_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u mem%%", dev_id);
          break;
        case plot_encoder_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u encode%%", dev_id);
          break;
        case plot_decoder_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u decode%%", dev_id);
          break;
        case plot_gpu_temperature:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u temp(c)", dev_id);
          break;
        case plot_gpu_power_draw_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u power%%", dev_id);
          break;
        case plot_fan_speed:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u fan%%", dev_id);
          break;
        case plot_gpu_clock_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u clock%%", dev_id);
          break;
        case plot_gpu_mem_clock_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u mem clock%%", dev_id);
          break;
        case plot_effective_load_rate:
          snprintf(plot_legend[in_processing], PLOT_MAX_LEGEND_SIZE, "GPU%u eff. load%%", dev_id);
          break;
        case plot_information_count:
          break;
        }
        // Unsampled history is NaN so the plot skips it instead of drawing
        // a misleading zero line for data that was never recorded.
        for (unsigned j = 0; j < max_data_to_copy; ++j)
          data_split[j][in_processing] = NAN;
        // Copy the data
        unsigned data_in_ring = interface_ring_buffer_data_stored(&interface->saved_data_ring, dev_id, data_ring_index);
        if (interface->options.plot_left_to_right) {
          for (unsigned j = 0; j < data_in_ring && j < max_data_to_copy; ++j) {
            data_split[j][in_processing] =
                interface_ring_buffer_get(&interface->saved_data_ring, dev_id, data_ring_index, data_in_ring - j - 1);
          }
        } else {
          for (unsigned j = 0; j < data_in_ring && j < max_data_to_copy; ++j) {
            data_split[max_data_to_copy - j - 1][in_processing] =
                interface_ring_buffer_get(&interface->saved_data_ring, dev_id, data_ring_index, data_in_ring - j - 1);
          }
        }
        data_ring_index++;
        in_processing++;
      }
    }
  }
  return total_to_draw;
}

static void draw_plots(struct nvtop_interface *interface) {
  for (unsigned plot_id = 0; plot_id < interface->num_plots; ++plot_id) {
    werase(interface->plots[plot_id].plot_window);

    char plot_legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];

    unsigned num_lines = populate_plot_data_from_ring_buffer(
        interface, &interface->plots[plot_id], interface->plots[plot_id].num_data * MAX_LINES_PER_PLOT,
        interface->plots[plot_id].data, plot_legend);

    nvtop_line_plot(interface->plots[plot_id].plot_window, interface->plots[plot_id].num_data,
                    interface->plots[plot_id].data, num_lines);
    // Unicode mode carries the legend on the frame's top border, keeping
    // every inner row for the trace itself.
    nvtop_plot_draw_legend(interface->plots[plot_id].win, 3,
                           interface->options.show_chart_legend ? num_lines : 0,
                           !interface->options.plot_left_to_right, plot_legend);

    wnoutrefresh(interface->plots[plot_id].win);

    wnoutrefresh(interface->plots[plot_id].plot_window);
  }
}

void draw_gpu_info_ncurses(unsigned devices_count, struct list_head *devices, struct nvtop_interface *interface) {
  // Full redraws happen on startup, resize and explicit refresh.
  // Otherwise sections repaint only when their data changed: the meters,
  // plots and process list repaint once per data update (once per update
  // interval); between updates key presses redraw nothing and ncurses'
  // diff-based doupdate() emits no output at all.
  if (interface->redraw_all || interface->devices_dirty) {
    draw_devices(devices, interface);
  }

  if (!interface->setup_win.visible) {
    if (interface->redraw_all || interface->devices_dirty) {
      draw_plots(interface);
    }
    if (interface->redraw_all || interface->process_dirty) {
      draw_processes(devices, interface);
    }
  } else {
    if (interface->redraw_all || interface->setup_dirty)
      draw_setup_window(devices_count, devices, interface);
  }

  if (interface->redraw_all || interface->process_dirty || interface->setup_dirty)
    draw_shortcuts(interface);

  interface->redraw_all = false;
  interface->devices_dirty = false;
  interface->process_dirty = false;
  interface->setup_dirty = false;
  doupdate();
}

void update_window_size_to_terminal_size(struct nvtop_interface *inter) {
  // Re-allocating the windows resets the setup window state; keep an open
  // setup open across option toggles that need a re-layout.
  bool setup_was_visible = inter->setup_win.visible;
  endwin();
  erase();
  refresh();
  refresh();
  delete_all_windows(inter);
  initialize_all_windows(inter);
  inter->setup_win.visible = setup_was_visible;
  inter->redraw_all = true;
  inter->devices_dirty = true;
  inter->process_dirty = true;
}

void interface_handle_mouse(int y, int x, struct nvtop_interface *interface) {
  for (unsigned i = 0; i < interface->gear_count; ++i) {
    if (interface->gear_rects[i].y == y && x >= interface->gear_rects[i].x0 &&
        x <= interface->gear_rects[i].x1) {
      if (interface->setup_win.visible) {
        interface->setup_win.visible = false;
        update_window_size_to_terminal_size(interface);
      } else {
        show_setup_window(interface);
      }
      interface->setup_dirty = true;
      return;
    }
  }
}

bool is_escape_for_quit(struct nvtop_interface *interface) {
  if (interface->process.option_window.state == nvtop_option_state_hidden && !interface->setup_win.visible)
    return true;
  else
    return false;
}

static void option_do_kill(struct nvtop_interface *interface) {
  if (interface->process.option_window.selected_row == 0)
    return;
  pid_t pid = interface->process.selected_pid;
  int sig = signalValues[interface->process.option_window.selected_row];
  if (pid > 0) {
    kill(pid, sig);
  }
}

static void option_change_sort(struct nvtop_interface *interface) {
  if (interface->process.option_window.selected_row == 0)
    return;
  unsigned index = 0;
  for (enum process_field i = process_pid; i < process_field_count; ++i) {
    if (process_is_field_displayed(i, interface->options.process_fields_displayed)) {
      if (index == interface->process.option_window.selected_row - 1) {
        interface->options.sort_processes_by = i;
        return;
      }
      index++;
    }
  }
}

void interface_key(int keyId, struct nvtop_interface *interface) {
  if (interface->setup_win.visible) {
    handle_setup_win_keypress(keyId, interface);
    interface->setup_dirty = true;
    return;
  }
  switch (keyId) {
  case KEY_F(2):
    if (interface->process.option_window.state == nvtop_option_state_hidden && !interface->setup_win.visible) {
      show_setup_window(interface);
      interface->setup_dirty = true;
    }
    break;
  case KEY_F(12):
    save_interface_options_to_config_file(interface->total_dev_count, &interface->options);
    break;
  case KEY_F(9):
    if (process_field_displayed_count(interface->options.process_fields_displayed) > 0 &&
        interface->process.option_window.state == nvtop_option_state_hidden) {
      interface->process.option_window.state = nvtop_option_state_kill;
      interface->process.option_window.selected_row = 0;
      interface->process.option_window.last_key_was_number = false;
      interface->process.option_window.input_number = 0;
    }
    break;
  case KEY_F(6):
    if (process_field_displayed_count(interface->options.process_fields_displayed) > 0 &&
        interface->process.option_window.state == nvtop_option_state_hidden) {
      interface->process.option_window.state = nvtop_option_state_sort_by;
      interface->process.option_window.selected_row = 0;
    }
    break;
  case 'l':
  case KEY_RIGHT:
    if (interface->process.option_window.state == nvtop_option_state_hidden)
      interface->process.offset_column += 4;
    break;
  case 'h':
  case KEY_LEFT:
    if (interface->process.option_window.state == nvtop_option_state_hidden && interface->process.offset_column >= 4)
      interface->process.offset_column -= 4;
    break;
  case 'k':
  case KEY_UP:
    switch (interface->process.option_window.state) {
    case nvtop_option_state_kill:
    case nvtop_option_state_sort_by:
      if (interface->process.option_window.selected_row != 0)
        interface->process.option_window.selected_row--;
      break;
    case nvtop_option_state_hidden:
      if (interface->process.selected_row != 0)
        interface->process.selected_row--;
      break;
    default:
      break;
    }
    break;
  case 'j':
  case KEY_DOWN:
    switch (interface->process.option_window.state) {
    case nvtop_option_state_kill:
    case nvtop_option_state_sort_by:
      interface->process.option_window.selected_row++;
      break;
    case nvtop_option_state_hidden:
      interface->process.selected_row++;
      break;
    default:
      break;
    }
    break;
  case '+':
    interface->options.sort_descending_order = false;
    break;
  case '-':
    interface->options.sort_descending_order = true;
    break;
  case '\n':
  case KEY_ENTER:
    switch (interface->process.option_window.state) {
    case nvtop_option_state_kill:
      option_do_kill(interface);
      interface->process.option_window.state = nvtop_option_state_hidden;
      break;
    case nvtop_option_state_sort_by:
      option_change_sort(interface);
      interface->process.option_window.state = nvtop_option_state_hidden;
      break;
    case nvtop_option_state_hidden:
    default:
      break;
    }
    break;
  case 27:
    interface->process.option_window.state = nvtop_option_state_hidden;
    break;
  case KEY_F(5):
  case 12: // Ctrl+L
    update_window_size_to_terminal_size(interface);
    break;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    if (interface->process.option_window.state == nvtop_option_state_kill) {
      unsigned int val = keyId - '0';
      if (interface->process.option_window.last_key_was_number) {
        unsigned int new_val = interface->process.option_window.input_number * 10 + val;
        if (new_val <= nvtop_num_signals) {
          interface->process.option_window.input_number = new_val;
          interface->process.option_window.selected_row = new_val;
        } else {
          interface->process.option_window.input_number = val;
          interface->process.option_window.selected_row = val;
        }
      } else {
        interface->process.option_window.input_number = val;
        interface->process.option_window.selected_row = val;
      }
    }
    break;
  default:
    break;
  }
  interface->process.option_window.last_key_was_number = (keyId >= '0' && keyId <= '9');
  // Every key press may have moved the selection, toggled the option popup
  // or scrolled the list: repaint the process section on the next frame.
  interface->process_dirty = true;
}

bool interface_freeze_processes(struct nvtop_interface *interface) {
  return interface->process.option_window.state == nvtop_option_state_kill;
}

extern inline void set_attribute_between(WINDOW *win, int startY, int startX, int endX, attr_t attr, short pair);

int interface_update_interval(const struct nvtop_interface *interface) { return interface->options.update_interval; }

unsigned interface_largest_gpu_name(struct list_head *devices) {
  struct gpu_info *gpuinfo;
  unsigned max_size = 4;
  list_for_each_entry(gpuinfo, devices, list) {
    if (GPUINFO_STATIC_FIELD_VALID(&gpuinfo->static_info, device_name)) {
      unsigned name_len = strlen(gpuinfo->static_info.device_name);
      max_size = name_len > max_size ? name_len : max_size;
    }
  }
  return max_size;
}

void interface_check_monitored_gpu_change(struct nvtop_interface **interface, unsigned allDevCount,
                                          unsigned *num_monitored_gpus, struct list_head *monitoredGpus,
                                          struct list_head *nonMonitoredGpus) {
  if (!(*interface)->setup_win.visible && (*interface)->options.has_monitored_set_changed) {
    nvtop_interface_option options_copy = (*interface)->options;
    options_copy.has_monitored_set_changed = false;
    memset(&(*interface)->options, 0, sizeof(options_copy));
    *num_monitored_gpus =
        interface_check_and_fix_monitored_gpus(allDevCount, monitoredGpus, nonMonitoredGpus, &options_copy);
    clean_ncurses(*interface);
    *interface =
        initialize_curses(allDevCount, *num_monitored_gpus, interface_largest_gpu_name(monitoredGpus), options_copy);
    timeout(interface_update_interval(*interface));
  }
}

static char dontShowAgain[] = "<Don't Show Again>";
static char okay[] = "<Ok>";
static char interactKeys[] = "Press Enter to select, arrows \">\" and \"<\" to switch options";

static unsigned message_lines(unsigned message_size, unsigned cols) { return (message_size + cols - 1) / cols; }

bool show_information_messages(unsigned num_messages, const char **messages) {
  if (!num_messages)
    return false;
  bool exit = false;
  bool dontShowAgainOption = false;

  while (!exit) {
    initscr();
    clear();
    refresh();
    static const unsigned char default_plot_colors[MAX_LINES_PER_PLOT] = {1, 3, 2, 4};
    initialize_colors(default_plot_colors);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    unsigned messages_lines = num_messages / 2;
    for (unsigned i = 0; i < num_messages; ++i) {
      messages_lines += message_lines(strlen(messages[i]), cols) + 1;
    }
    int row = (rows - messages_lines + 1) / 2;
    for (unsigned i = 0; i < num_messages; ++i) {
      int col = (cols - strlen(messages[i]) - 1) / 2;
      col = col < 0 ? 0 : col;
      mvprintw(row, col, "%s", messages[i]);
      row += message_lines(strlen(messages[i]), cols) + 1;
    }
    size_t sizeQuitOptions = sizeof(dontShowAgain) + sizeof(okay);
    int quitOptionsRow = row;
    int quitOptionsCol = (cols - sizeQuitOptions) / 2;
    quitOptionsCol = quitOptionsCol < 0 ? 0 : quitOptionsCol;
    mvprintw(quitOptionsRow, quitOptionsCol, "%s %s", dontShowAgain, okay);
    refresh();
    if (dontShowAgainOption) {
      mvchgat(quitOptionsRow, quitOptionsCol, sizeof(dontShowAgain) - 1, 0, green_color, NULL);
      mvchgat(quitOptionsRow, quitOptionsCol + sizeof(dontShowAgain), sizeof(okay) - 1, 0, 0, NULL);
    } else {
      mvchgat(quitOptionsRow, quitOptionsCol, sizeof(dontShowAgain) - 1, 0, 0, NULL);
      mvchgat(quitOptionsRow, quitOptionsCol + sizeof(dontShowAgain), sizeof(okay) - 1, 0, green_color, NULL);
    }
    int interactKeyCol = (cols - sizeof(interactKeys) - 1) / 2;
    interactKeyCol = interactKeyCol < 0 ? 0 : interactKeyCol;
    mvprintw(quitOptionsRow + 1, interactKeyCol, "%s", interactKeys);

    int input_char = getch();
    switch (input_char) {
    case 27: // ESC
    case 'q':
    case KEY_ENTER:
    case '\n':
      exit = true;
      break;
    case KEY_RIGHT:
      dontShowAgainOption = false;
      break;
    case KEY_LEFT:
      dontShowAgainOption = true;
      break;
    default:
      break;
    }
    endwin();
  }
  return dontShowAgainOption;
}

void print_snapshot(struct list_head *devices, bool use_fahrenheit_option, bool hide_processes_option) {
  struct gpu_info *device;

  printf("[\n");
  list_for_each_entry(device, devices, list) {
    const char *indent_level_two = "  ";
    const char *indent_level_four = "   ";
    const char *indent_level_six = "     ";
    const char *indent_level_eight = "       ";

    const char *device_name_field = "device_name";
    const char *gpu_clock_field = "gpu_clock";
    const char *mem_clock_field = "mem_clock";
    const char *temp_field = "temp";
    const char *fan_field = "fan_speed";
    const char *power_field = "power_draw";
    const char *gpu_util_field = "gpu_util";
    const char *mem_util_field = "mem_util";
    const char *mem_total_field = "mem_total";
    const char *mem_used_field = "mem_used";
    const char *mem_free_field = "mem_free";

    printf("%s{\n", indent_level_two);

    // Device Name
    if (GPUINFO_STATIC_FIELD_VALID(&device->static_info, device_name))
      printf("%s\"%s\": \"%s\",\n", indent_level_four, device_name_field, device->static_info.device_name);
    else
      printf("%s\"%s\": null,\n", indent_level_four, device_name_field);

    // GPU Clock Speed
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_clock_speed))
      printf("%s\"%s\": \"%uMHz\",\n", indent_level_four, gpu_clock_field, device->dynamic_info.gpu_clock_speed);
    else
      printf("%s\"%s\": null,\n", indent_level_four, gpu_clock_field);

    // MEM Clock Speed
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_clock_speed))
      printf("%s\"%s\": \"%uMHz\",\n", indent_level_four, mem_clock_field, device->dynamic_info.mem_clock_speed);
    else
      printf("%s\"%s\": null,\n", indent_level_four, mem_clock_field);

    // GPU Temperature
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_temp)) {
      unsigned int temp_convert;
      if (!use_fahrenheit_option)
        temp_convert = device->dynamic_info.gpu_temp;
      else
        temp_convert = (unsigned)(32 + nearbyint(device->dynamic_info.gpu_temp * 1.8));

      printf("%s\"%s\": \"%u%s\",\n", indent_level_four, temp_field, temp_convert, use_fahrenheit_option ? "F" : "C");
    } else {
      printf("%s\"%s\": null,\n", indent_level_four, temp_field);
    }

    // Fan speed
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, fan_speed))
      printf("%s\"%s\": \"%u%%\",\n", indent_level_four, fan_field,
             device->dynamic_info.fan_speed > 100 ? 100 : device->dynamic_info.fan_speed);
    else if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, fan_rpm))
      printf("%s\"%s\": \"%uRPM\",\n", indent_level_four, fan_field,
             device->dynamic_info.fan_rpm > 9999 ? 9999 : device->dynamic_info.fan_rpm);
    else if (device->static_info.integrated_graphics)
      printf("%s\"%s\": \"CPU Fan\",\n", indent_level_four, fan_field);
    else
      printf("%s\"%s\": null,\n", indent_level_four, fan_field);

    // Power draw
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, power_draw))
      printf("%s\"%s\": \"%uW\",\n", indent_level_four, power_field, device->dynamic_info.power_draw / 1000);
    else
      printf("%s\"%s\": null,\n", indent_level_four, power_field);

    // GPU Utilization
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, gpu_util_rate))
      printf("%s\"%s\": \"%u%%\",\n", indent_level_four, gpu_util_field, device->dynamic_info.gpu_util_rate);
    else
      printf("%s\"%s\": null,\n", indent_level_four, gpu_util_field);

    // Encode / Decode
    if (device->static_info.encode_decode_shared) {
      printf("%s\"encode_decode\": ", indent_level_four);
      if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, decoder_rate))
        printf("\"%u%%\",\n", device->dynamic_info.decoder_rate);
      else
        printf("null,\n");
    } else {
      printf("%s\"encode\": ", indent_level_four);
      if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, encoder_rate))
        printf("\"%u%%\",\n", device->dynamic_info.encoder_rate);
      else
        printf("null,\n");
      printf("%s\"decode\": ", indent_level_four);
      if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, decoder_rate))
        printf("\"%u%%\",\n", device->dynamic_info.decoder_rate);
      else
        printf("null,\n");
    }

    // Memory Utilization
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, mem_util_rate))
      printf("%s\"%s\": \"%u%%\",\n", indent_level_four, mem_util_field, device->dynamic_info.mem_util_rate);
    else
      printf("%s\"%s\": null,\n", indent_level_four, mem_util_field);
    // Memory Total
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, total_memory))
      printf("%s\"%s\": \"%llu\",\n", indent_level_four, mem_total_field, device->dynamic_info.total_memory);
    else
      printf("%s\"%s\": null,\n", indent_level_four, mem_total_field);
    // Memory Used
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, used_memory))
      printf("%s\"%s\": \"%llu\",\n", indent_level_four, mem_used_field, device->dynamic_info.used_memory);
    else
      printf("%s\"%s\": null,\n", indent_level_four, mem_used_field);
    // Memory Available
    if (GPUINFO_DYNAMIC_FIELD_VALID(&device->dynamic_info, free_memory))
      printf("%s\"%s\": \"%llu\"", indent_level_four, mem_free_field, device->dynamic_info.free_memory);
    else
      printf("%s\"%s\": null", indent_level_four, mem_free_field);

    // Processes
    if (hide_processes_option) {
      // (Notice: no comma at the end as it's the last field here)
      printf("\n");
    } else {
      printf(",\n%s\"processes\" : [\n", indent_level_four);
      for (unsigned i = 0; i < device->processes_count; ++i) {
        struct gpu_process *proc = &device->processes[i];
        printf("%s{\n", indent_level_six);

        // PID
        printf("%s\"pid\": \"%d\",\n", indent_level_eight, proc->pid);

        if (GPUINFO_PROCESS_FIELD_VALID(proc, cmdline) && proc->cmdline) {
          printf("%s\"cmdline\": \"", indent_level_eight);
          for (char *li = proc->cmdline; *li != '\0'; li++) {
            // We need to escape some characters for for json strings
            if (*li == '\n') {
              printf("\\n");
              continue;
            } else if (*li == '\b') {
              printf("\\b");
              continue;
            } else if (*li == '\f') {
              printf("\\f");
              continue;
            } else if (*li == '\r') {
              printf("\\r");
              continue;
            } else if (*li == '\t') {
              printf("\\t");
              continue;
            }
            // escaping backslash and quotes
            if (*li == '\\' || *li == '"')
              printf("\\");
            printf("%c", *li);
          }
          printf("\",\n");
        } else {
          printf("%s\"cmdline\": null,\n", indent_level_eight);
        }

        printf("%s\"kind\": ", indent_level_eight);
        if (proc->type != gpu_process_unknown) {
          printf("\"");
          switch (proc->type) {
          case gpu_process_graphical:
            printf("graphic");
            break;
          case gpu_process_compute:
            printf("compute");
            break;
          case gpu_process_graphical_compute:
            printf("graphic & compute");
            break;
          default:
            printf("N/A");
            break;
          }
          printf("\"");
        } else {
          printf("null");
        }
        printf(",\n");

        // GPU memory usage
        printf("%s\"user\": ", indent_level_eight);
        if (GPUINFO_PROCESS_FIELD_VALID(proc, user_name))
          printf("\"%s\",\n", proc->user_name);
        else
          printf("null,\n");

        // GPU usage
        printf("%s\"gpu_usage\": ", indent_level_eight);
        if (GPUINFO_PROCESS_FIELD_VALID(proc, gpu_usage))
          printf("\"%u%%\",\n", proc->gpu_usage);
        else
          printf("null,\n");

        // GPU memory usage
        printf("%s\"gpu_mem_bytes_alloc\": ", indent_level_eight);
        if (GPUINFO_PROCESS_FIELD_VALID(proc, gpu_memory_usage))
          printf("\"%llu\",\n", proc->gpu_memory_usage);
        else
          printf("null,\n");

        // GPU memory usage
        printf("%s\"gpu_mem_usage\": ", indent_level_eight);
        if (GPUINFO_PROCESS_FIELD_VALID(proc, gpu_memory_percentage))
          printf("\"%u%%\",\n", proc->gpu_memory_percentage);
        else
          printf("null,\n");

        // Encode usage
        if (device->static_info.encode_decode_shared) {
          // (Notice: no comma at the end as it's the last field here)
          printf("%s\"encode_decode\": ", indent_level_eight);
          if (GPUINFO_PROCESS_FIELD_VALID(proc, decode_usage))
            printf("\"%u%%\"\n", proc->decode_usage);
          else
            printf("null\n");
        } else {
          printf("%s\"encode\": ", indent_level_eight);
          if (GPUINFO_PROCESS_FIELD_VALID(proc, encode_usage))
            printf("\"%u%%\",\n", proc->encode_usage);
          else
            printf("null,\n");
          // (Notice: no comma at the end as it's the last field here)
          printf("%s\"decode\": ", indent_level_eight);
          if (GPUINFO_PROCESS_FIELD_VALID(proc, decode_usage))
            printf("\"%u%%\"\n", proc->decode_usage);
          else
            printf("null\n");
        }

        printf("%s}", indent_level_six);
        if (i != device->processes_count - 1)
          printf(",");
        printf("\n");
      }
      // (Notice: no comma at the end as it's the last field here)
      printf("%s]\n", indent_level_four);
    }

    if (device->list.next == devices)
      printf("%s}\n", indent_level_two);
    else
      printf("%s},\n", indent_level_two);
  }
  printf("]\n");
  fflush(stdout);
}
