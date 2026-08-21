/*
 * Copyright (C) 2019-2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#include "nvtop/plot.h"
#include "nvtop/common.h"
#include "nvtop/interface_internal_common.h"

#include <assert.h>
#include <math.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool plot_unicode = false;
static bool plot_color = false;

void nvtop_plot_set_unicode(bool use_unicode) { plot_unicode = use_unicode; }
void nvtop_plot_set_color(bool use_color) { plot_color = use_color; }

// Terminal chrome looks best with real gray palette entries; fall back to
// A_DIM on the default foreground when the terminal only has 8/16 colors.
static inline bool plot_ext_colors(void) { return plot_color && COLORS >= 256; }

static void plot_chrome_on(WINDOW *win, short pair) {
  if (plot_color) {
    wattr_set(win, plot_ext_colors() ? A_NORMAL : A_DIM, pair, NULL);
  } else {
    wattron(win, A_DIM);
  }
}

static void plot_chrome_off(WINDOW *win) {
  if (plot_color)
    wattr_set(win, A_NORMAL, 0, NULL);
  else
    wattroff(win, A_DIM);
}

// Sub-row mapping shared by the trace, the grid and the axis labels: an
// inner window of R rows offers R*4 braille sub-rows (top = 100%, bottom =
// 0%). Every consumer MUST go through these helpers so labels and gridlines
// stay glued to the trace at every terminal size.
static inline int value_to_subrow(int rows_inner, double percent) {
  double max_sub = (double)rows_inner * 4. - 1.;
  return (int)lround((100. - percent) / 100. * max_sub);
}

static inline int value_to_cell(int rows_inner, double percent) { return value_to_subrow(rows_inner, percent) / 4; }

int plot_label_row(int rows_inner, unsigned percent) {
  if (plot_unicode)
    return value_to_cell(rows_inner, (double)percent);
  // ASCII trace mapping: rows_inner-1 discrete levels
  double levels = (double)rows_inner - 1.;
  return (int)((double)rows_inner - 1. - round(percent / 100. * levels));
}

// Faint dotted reference lines at 100/75/50/25%, drawn BEFORE the trace so
// the graph always paints over them. The bottom border doubles as the 0%
// line.
static void draw_plot_grid(WINDOW *win, int rows, int cols) {
  if (rows < 3 || cols < 4)
    return;
  plot_chrome_on(win, grid_color);
  static const unsigned grid_levels[4] = {100, 75, 50, 25};
  for (unsigned g = 0; g < ARRAY_SIZE(grid_levels); ++g) {
    int r = plot_label_row(rows, grid_levels[g]);
    if (r < 0 || r > rows - 1)
      continue;
    for (int x = 1; x < cols; x += 3)
      mvwaddch(win, r, x, ACS_BULLET);
  }
  plot_chrome_off(win);
}

/*
 * Unicode (braille) plot.
 *
 * Each terminal cell is a 2x4 dot matrix addressed in "sub-columns" (0 ..
 * 2*cols-1) and "sub-rows" (0 .. rows*4-1). Samples sit on the center
 * sub-column of their cell; consecutive samples are joined by interpolated
 * segments so even slow-moving traces render as a smooth curve. The area
 * under the first (primary) series is shaded with the same series color at
 * a dimmed attribute, which gives the graph its depth. Cells are merged
 * across series: a cell containing line dots inherits the color of the
 * lowest series that drew a line dot there (the primary trace stays visible
 * when series overlap), otherwise the last series that filled it.
 */

// Braille pattern bits (U+2800 | bits). Left column top-to-bottom = dots
// 1,2,3,7; right column = dots 4,5,6,8.
static const unsigned char braille_bits[2][4] = {
    {0x01, 0x02, 0x04, 0x40},
    {0x08, 0x10, 0x20, 0x80},
};

struct plot_cell {
  unsigned char mask;
  short line_series; // lowest series index that drew a line dot here
  short fill_series;
  unsigned char fill_depth; // sub-rows below the curve (primary fill)
};

static void cell_set_dot(struct plot_cell *cells, int rows, int cols, int subcol, int subrow, bool is_line,
                         unsigned series, unsigned depth) {
  int cell_col = subcol / 2;
  if (cell_col < 0 || cell_col >= cols)
    return;
  int cell_row = subrow / 4;
  if (cell_row < 0 || cell_row >= rows)
    return;
  struct plot_cell *cell = &cells[cell_row * cols + cell_col];
  cell->mask |= braille_bits[subcol % 2][subrow % 4];
  if (is_line) {
    if (cell->line_series < 0 || (unsigned)cell->line_series > series)
      cell->line_series = (short)series;
  }
  if (!is_line && (cell->fill_series < 0 || depth < cell->fill_depth))
    cell->fill_depth = (unsigned char)(depth > 255 ? 255 : depth);
  cell->fill_series = (short)series;
}

static void plot_braille(WINDOW *win, int rows, int cols, size_t num_data, const double *data, unsigned num_lines) {
  const int max_sub = rows * 4 - 1;
  struct plot_cell *cells = calloc((size_t)rows * cols, sizeof(*cells));
  int *subs = malloc(num_data * num_lines * sizeof(*subs));
  if (!cells || !subs) {
    free(cells);
    free(subs);
    return;
  }
  // calloc zeroes: set the -1 "no series" sentinels explicitly.
  for (size_t i = 0; i < (size_t)rows * cols; ++i) {
    cells[i].line_series = -1;
    cells[i].fill_series = -1;
    cells[i].fill_depth = 255;
  }
  for (size_t i = 0; i < num_data * num_lines; ++i)
    subs[i] = -1;

  for (unsigned k = 0; k < num_lines; ++k) {
    // The area under the curve is shaded for the primary series only;
    // secondary series stay crisp lines floating above the fill.
    bool primary = (k == 0);
    ssize_t prev = -1;   // column index of the previous valid sample
    ssize_t first = -1;  // first valid sample (extends to the cell's left half)
    for (size_t i = 0; i < num_data; ++i) {
      double v = data[i * num_lines + k];
      if (isnan(v))
        continue;
      int s = value_to_subrow(rows, v);
      if (s < 0)
        s = 0;
      if (s > max_sub)
        s = max_sub;
      subs[i * (ssize_t)num_lines + k] = s;
      int x = 2 * (int)i + 1; // center sub-column of cell i
      if (first < 0) {
        // The curve owns the whole cell: also cover the left half so the
        // leading edge is not notched.
        first = (ssize_t)i;
        cell_set_dot(cells, rows, cols, x - 1, s, true, k, 0);
        if (primary) {
          for (int yy = s + 1; yy <= max_sub; ++yy)
            cell_set_dot(cells, rows, cols, x - 1, yy, false, k, (unsigned)(yy - s));
        }
      }
      cell_set_dot(cells, rows, cols, x, s, true, k, 0);
      if (primary) {
        for (int yy = s + 1; yy <= max_sub; ++yy)
          cell_set_dot(cells, rows, cols, x, yy, false, k, (unsigned)(yy - s));
      }
      // Interpolate from the previous sample, but never bridge more than a
      // single missing sample: longer holes are gaps in the data, not a
      // curve to connect.
      if (prev >= 0 && i - (size_t)prev <= 2) {
        int x0 = 2 * (int)prev + 1, x1 = x;
        int y0 = subs[prev * (ssize_t)num_lines + k], y1 = s;
        for (int xc = x0 + 1; xc < x1; ++xc) {
          double t = (double)(xc - x0) / (double)(x1 - x0);
          int yc = (int)lround(y0 + (y1 - y0) * t);
          if (yc < 0)
            yc = 0;
          if (yc > max_sub)
            yc = max_sub;
          cell_set_dot(cells, rows, cols, xc, yc, true, k, 0);
          if (primary) {
            for (int yy = yc + 1; yy <= max_sub; ++yy)
              cell_set_dot(cells, rows, cols, xc, yy, false, k, (unsigned)(yy - yc));
          }
        }
      }
      prev = (ssize_t)i;
    }
    if (prev >= 0) {
      // Trailing edge: cover the right half of the newest sample's cell so
      // the trace and fill reach the right border of the data.
      int x = 2 * (int)prev + 2;
      int s = subs[prev * (ssize_t)num_lines + k];
      cell_set_dot(cells, rows, cols, x, s, true, k, 0);
      if (primary) {
        for (int yy = s + 1; yy <= max_sub; ++yy)
          cell_set_dot(cells, rows, cols, x, yy, false, k, (unsigned)(yy - s));
      }
    }
  }

  static const short plot_line_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_color, gpu_mem_plot_color,
                                                             gpu_plot_color_3, gpu_plot_color_4};
  static const short plot_mid_fg_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_mid_fg_color,
                                                               gpu_mem_plot_mid_fg_color, gpu_plot_mid_fg_color_3,
                                                               gpu_plot_mid_fg_color_4};
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      struct plot_cell *cell = &cells[r * cols + c];
      if (cell->mask == 0)
        continue;
      bool has_line = cell->line_series >= 0;
      short pair = has_line ? cell->line_series : cell->fill_series;
      if (pair < 0 || pair >= MAX_LINES_PER_PLOT)
        pair = 0;
      // Fill gradient: cells right below the curve use a mid shade of the
      // series color before fading into the dim body.
      bool shallow = !has_line && plot_ext_colors() && cell->fill_depth < 8;
      if (plot_color && shallow)
        wattr_set(win, A_NORMAL, plot_mid_fg_colors[pair], NULL);
      else if (plot_color)
        wattr_set(win, has_line ? A_NORMAL : A_DIM, plot_line_colors[pair], NULL);
      else if (!has_line)
        wattron(win, A_DIM);
      unsigned pattern = 0x2800u | cell->mask;
      char glyph[5];
      glyph[0] = (char)(0xe0 | (pattern >> 12));
      glyph[1] = (char)(0x80 | ((pattern >> 6) & 0x3f));
      glyph[2] = (char)(0x80 | (pattern & 0x3f));
      glyph[3] = '\0';
      mvwaddstr(win, r, c, glyph);
      if (plot_color && shallow)
        wattr_set(win, A_NORMAL, 0, NULL);
      else if (!plot_color && !has_line)
        wattroff(win, A_DIM);
    }
  }
  if (plot_color)
    wattr_set(win, A_NORMAL, 0, NULL);
  free(cells);
  free(subs);
}

/*
 * ASCII fallback: solid block chart. Each sample owns one column and the
 * area from its level down to the baseline is filled with ACS_BLOCK -
 * bright at the curve's edge, dim in the body - so the graph reads exactly
 * like the braille one, just at whole-cell resolution.
 */
struct block_cell {
  short line_series;
  short fill_series;
  unsigned char fill_depth; // rows below the curve edge (primary fill)
};

static void plot_ascii(WINDOW *win, int rows, int cols, size_t num_data, const double *data, unsigned num_lines) {
  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  struct block_cell *cells = malloc((size_t)rows * cols * sizeof(*cells));
  if (!cells)
    return;
  for (size_t i = 0; i < (size_t)rows * cols; ++i) {
    cells[i].line_series = -1;
    cells[i].fill_series = -1;
  }

  for (unsigned k = 0; k < num_lines; ++k) {
    bool primary = (k == 0);
    for (size_t i = 0; i < num_data && (int)i < cols; ++i) {
      double v = data[i * num_lines + k];
      if (isnan(v))
        continue;
      int lvl = plot_label_row(rows, (unsigned)v);
      if (lvl < 0)
        lvl = 0;
      if (lvl > rows - 1)
        lvl = rows - 1;
      struct block_cell *edge = &cells[lvl * cols + i];
      if (edge->line_series < 0 || edge->line_series > (short)k)
        edge->line_series = (short)k;
      if (primary) {
        for (int r = lvl + 1; r < rows; ++r) {
          struct block_cell *c = &cells[r * cols + i];
          if (c->fill_series < 0 || c->fill_series > (short)k) {
            c->fill_series = (short)k;
            c->fill_depth = (unsigned char)(r - lvl);
          }
        }
      }
    }
  }

  static const short plot_line_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_color, gpu_mem_plot_color,
                                                             gpu_plot_color_3, gpu_plot_color_4};
  static const short plot_fill_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_fill_color, gpu_mem_plot_fill_color,
                                                             gpu_plot_fill_color_3, gpu_plot_fill_color_4};
  static const short plot_mid_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_mid_color, gpu_mem_plot_mid_color,
                                                            gpu_plot_mid_color_3, gpu_plot_mid_color_4};
  static const short plot_body_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_body_color, gpu_mem_plot_body_color,
                                                             gpu_plot_body_color_3, gpu_plot_body_color_4};
  // With an extended palette the fill is drawn as colored backgrounds
  // (spaces) in three depth zones - series color at the curve's edge, mid
  // shade below it, dark body towards the baseline. Without one, fall back
  // to ACS_BLOCK glyphs, dimmed for the body.
  bool solid_fill = plot_color && plot_ext_colors();
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      struct block_cell *cell = &cells[r * cols + c];
      bool is_line = cell->line_series >= 0;
      short series = is_line ? cell->line_series : cell->fill_series;
      if (series < 0)
        continue;
      if (solid_fill) {
        short pair;
        if (is_line || cell->fill_depth == 0)
          pair = plot_fill_colors[series];
        else if (cell->fill_depth <= 2)
          pair = plot_mid_colors[series];
        else
          pair = plot_body_colors[series];
        wattr_set(win, A_NORMAL, pair, NULL);
        mvwaddch(win, r, c, ' ');
      } else {
        if (plot_color)
          wattr_set(win, is_line ? A_NORMAL : A_DIM, plot_line_colors[series], NULL);
        else if (!is_line)
          wattron(win, A_DIM);
        mvwaddch(win, r, c, ACS_BLOCK);
        if (!plot_color && !is_line)
          wattroff(win, A_DIM);
      }
    }
  }
  if (plot_color)
    wattr_set(win, A_NORMAL, 0, NULL);
  free(cells);
}

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_lines) {
  if (num_data == 0)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  if (rows < 2 || cols < 1)
    return;

  draw_plot_grid(win, rows, cols);

  if (plot_unicode)
    plot_braille(win, rows, cols, num_data, data, num_lines);
  else
    plot_ascii(win, rows, cols, num_data, data, num_lines);
}

void nvtop_plot_draw_legend(WINDOW *border_win, unsigned border_start_x, unsigned num_lines, bool legend_left,
                            char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_lines == 0)
    return;
  int rows, cols;
  getmaxyx(border_win, rows, cols);
  (void)rows;
  if (border_start_x + 6 > (unsigned)cols)
    return;

  // Redraw the whole top border row: dashes, then the series legends
  // embedded in the frame. Redrawing everything keeps stale text of removed
  // series from lingering. The border spans border_start_x .. cols-1; the
  // columns left of it stay free for the axis labels.
  static const short plot_line_colors[MAX_LINES_PER_PLOT] = {gpu_util_plot_color, gpu_mem_plot_color,
                                                             gpu_plot_color_3, gpu_plot_color_4};
  int box_cols = cols - (int)border_start_x;
  plot_chrome_on(border_win, frame_color);
  if (plot_unicode) {
    mvwaddstr(border_win, 0, border_start_x, "\xe2\x95\xad"); // \u256d
    for (int x = border_start_x + 1; x < cols - 1; ++x)
      mvwaddstr(border_win, 0, x, "\xe2\x94\x80"); // \u2500
    mvwaddstr(border_win, 0, cols - 1, "\xe2\x95\xae"); // \u256e
  } else {
    mvwaddch(border_win, 0, border_start_x, ACS_ULCORNER);
    mvwhline(border_win, 0, border_start_x + 1, 0, cols - border_start_x - 2);
    mvwaddch(border_win, 0, cols - 1, ACS_URCORNER);
  }
  plot_chrome_off(border_win);

  // Measure the legend width first so right-aligned placement can clip it.
  // Unicode entries carry a round swatch; ASCII entries are plain text.
  int swatch = plot_unicode ? 2 : 0;
  int total = 0;
  int widths[MAX_LINES_PER_PLOT];
  for (unsigned i = 0; i < num_lines; ++i) {
    widths[i] = swatch + (int)strlen(legend[i]);
    total += widths[i] + (i ? 2 : 0); // two spaces between entries
  }
  int budget = box_cols - 4; // inside the corners plus one padding cell each side
  if (total > budget) {
    // Drop whole entries from the tail rather than clip mid-glyph.
    unsigned kept = 0;
    int acc = 0;
    for (unsigned i = 0; i < num_lines; ++i) {
      int w = widths[i] + (i ? 2 : 0);
      if (acc + w > budget)
        break;
      acc += w;
      kept = i + 1;
    }
    num_lines = kept;
    total = acc;
    if (num_lines == 0)
      return;
  }
  int pos = legend_left ? (int)border_start_x + 2 : cols - 2 - total;
  for (unsigned i = 0; i < num_lines; ++i) {
    if (i)
      pos += 2;
    wcolor_set(border_win, plot_line_colors[i], NULL);
    if (plot_unicode)
      mvwprintw(border_win, 0, pos, "\xe2\x97\x8f %.*s", widths[i] - 2, legend[i]);
    else
      mvwprintw(border_win, 0, pos, "%.*s", widths[i], legend[i]);
    pos += widths[i];
  }
  wcolor_set(border_win, 0, NULL);
}

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY) {
  plot_chrome_on(win, frame_color);

  if (plot_unicode && sizeX >= 2 && sizeY >= 2) {
    // Rounded unicode frame
    for (unsigned x = startX + 1; x < startX + sizeX - 1; ++x) {
      mvwaddstr(win, startY, x, "\xe2\x94\x80");             // ─
      mvwaddstr(win, startY + sizeY - 1, x, "\xe2\x94\x80"); // ─
    }
    for (unsigned y = startY + 1; y < startY + sizeY - 1; ++y) {
      mvwaddstr(win, y, startX, "\xe2\x94\x82");             // │
      mvwaddstr(win, y, startX + sizeX - 1, "\xe2\x94\x82"); // │
    }
    mvwaddstr(win, startY, startX, "\xe2\x95\xad");                         // ╭
    mvwaddstr(win, startY, startX + sizeX - 1, "\xe2\x95\xae");             // ╮
    mvwaddstr(win, startY + sizeY - 1, startX, "\xe2\x95\xb0");             // ╰
    mvwaddstr(win, startY + sizeY - 1, startX + sizeX - 1, "\xe2\x95\xaf"); // ╯
  } else {
    mvwhline(win, startY, startX + 1, 0, sizeX - 2);
    mvwhline(win, startY + sizeY - 1, startX + 1, 0, sizeX - 2);
    mvwvline(win, startY + 1, startX, 0, sizeY - 2);
    mvwvline(win, startY + 1, startX + sizeX - 1, 0, sizeY - 2);
    mvwaddch(win, startY, startX, ACS_ULCORNER);
    mvwaddch(win, startY, startX + sizeX - 1, ACS_URCORNER);
    mvwaddch(win, startY + sizeY - 1, startX, ACS_LLCORNER);
    mvwaddch(win, startY + sizeY - 1, startX + sizeX - 1, ACS_LRCORNER);
  }
  plot_chrome_off(win);
}
