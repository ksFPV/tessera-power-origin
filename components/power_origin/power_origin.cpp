#include "power_origin.h"
#include "visual_logic.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::power_origin {
using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

static void place(lv_obj_t *o, int x, int y, int w, int h) {
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, std::max(1, w), std::max(1, h));
}
// Only what changes: LVGL redraws a part on every show and every style set, even to what it already has, and the
// tiles set everything each second, so the whole tile was drawn again every tick (80-180 ms, the screen lagged).
static void show(lv_obj_t *o, bool yes) {
  if (yes != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  if (yes) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void put(lv_obj_t *o, lv_style_prop_t prop, int32_t n, lv_style_selector_t at) {
  lv_style_value_t v{};
  if (lv_obj_get_local_style_prop(o, prop, &v, at) == LV_STYLE_RES_FOUND && v.num == n) return;
  v.num = n;
  lv_obj_set_local_style_prop(o, prop, v, at);
}
static void put(lv_obj_t *o, lv_style_prop_t prop, lv_color_t c, lv_style_selector_t at) {
  lv_style_value_t v{};
  if (lv_obj_get_local_style_prop(o, prop, &v, at) == LV_STYLE_RES_FOUND && lv_color_eq(v.color, c)) return;
  v = {};
  v.color = c;
  lv_obj_set_local_style_prop(o, prop, v, at);
}
static void angles(lv_obj_t *a, int start, int end) {
  if (static_cast<int>(lv_arc_get_angle_start(a)) == start && static_cast<int>(lv_arc_get_angle_end(a)) == end) return;
  lv_arc_set_angles(a, start, end);
}
// The screen's words: translations/<language>.json "screen", built in the screen's language (English for what a
// language lacks). A language whose decimal sign is a comma says so as "decimal": ",".
static PowerOrigin *self_ = nullptr;  // ponytail: one plugin object per screen, set in setup()
static const char *T(const char *key) {
  const char *t = self_ ? self_->text(key) : "";
  return *t ? t : key;
}
static void say(lv_obj_t *o, std::string text) {
  if (*T("decimal") == ',') std::replace(text.begin(), text.end(), '.', ',');
  ui::set_text(o, text);
}
static std::string power(float v) {
  if (!std::isfinite(v)) return "--";
  char buf[32];
  v = std::fabs(v);
  if (v < 1000) snprintf(buf, sizeof(buf), "%.0f W", v);
  else snprintf(buf, sizeof(buf), "%.2f kW", v / 1000);
  return buf;
}

// Geometry of power-origin-card v0.35's flow view (src/power-origin-card.ts, _renderFlowView, no clock): a viewBox
// 320 wide, the house at (160,146) r 42, PV at (160,44), grid and store at x 46 and 274, r 32; strokes 4, the store's
// charge 6 wide at r 38, the store's own rim 2.5. Everything below is in those units, scaled to the tile.
constexpr float VB_W = 320, VB_LEFT = 5, VB_SPAN = 310, VB_TOP = 8, VB_H = 184, CX = 160, PY = 44, HY = 146, GX = 46, BX = 274, SR = 32, HR = 42;
constexpr float LIVE_W = 30;  // LIVE_KW: below it a power stands still, no dots, no arrow, the node is quiet

// A hold is a tap: LVGL calls a press of 400 ms a long press, and Tessera hands a plugin only the short one. The hold
// is sent on to the card as the short tap it is, so Tessera's own guard and tap point still decide (short or
// long, the same). LVGL sends no short tap after a long press, so one touch stays one tap.
static void hold_is_tap(lv_obj_t *parent) {
  static const lv_event_cb_t send = [](lv_event_t *e) {
    lv_obj_send_event(static_cast<lv_obj_t *>(lv_event_get_current_target(e)), LV_EVENT_SHORT_CLICKED, nullptr);
  };
  lv_obj_t *card = parent;
  while (card && !lv_obj_has_flag(card, LV_OBJ_FLAG_CLICKABLE)) card = lv_obj_get_parent(card);
  if (!card) return;
  lv_obj_remove_event_cb(card, send);  // a tile made again on the same card: still one
  lv_obj_add_event_cb(card, send, LV_EVENT_LONG_PRESSED, nullptr);
}
static lv_obj_t *ring_arc(lv_obj_t *parent, int cx, int cy, int r, int width, lv_part_t part) {
  auto *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  place(a, cx - r - width / 2, cy - r - width / 2, 2 * r + width, 2 * r + width);
  lv_obj_set_style_arc_width(a, width, part);
  lv_arc_set_rotation(a, 270);  // from the top, clockwise, as the SVG's rotate(-90)
  lv_arc_set_bg_angles(a, 0, 360);
  angles(a, 0, 0);
  return a;
}
static lv_obj_t *stroke(lv_obj_t *parent, int width) {
  auto *l = lv_line_create(parent);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}
// A participant's colour: a fixed role, or Home Assistant's red for the grid while it imports (import_red).
struct Paint { Tone tone; theme::Role role; };
static lv_color_t color(Paint p) { return ui::tone_color(p.tone, p.role); }

static float entity_value(JsonObjectConst data, const char *name, Unit kind) {
  JsonObjectConst e = data["entities"][name];
  return value(e["state"] | static_cast<const char *>(nullptr),
               e["attributes"]["unit_of_measurement"] | static_cast<const char *>(nullptr), kind);
}

class Flow : public tessera::Tile {
 public:
  explicit Flow(PowerOrigin *p) : plugin_(p) {}
  ~Flow() override { if (plugin_->card_from == this) plugin_->card_from = nullptr; }
  void create(const tessera::TileContext &c) override {
    hold_is_tap(c.parent);
    // One choice for both signs (the tile's 12 options are full); the two toggles of 0.8.1 and before still count.
    const char *inv = c.options["invert"] | "";
    invert_grid_ = !strcmp(inv, "grid") || !strcmp(inv, "both") || (!*inv && (c.options["invert_grid"] | false));
    invert_battery_ = !strcmp(inv, "battery") || !strcmp(inv, "both") || (!*inv && (c.options["invert_battery"] | false));
    const char *hours = c.options["hours"] | "dots";
    hour_dots_ = strcmp(hours, "bars") != 0;
    hours_off_ = !strcmp(hours, "off");
    home_entity_ = c.options["home"] | "";
    grid_entity_ = c.options["grid"] | "";
    battery_entity_ = c.options["battery"] | "";
    tile_ = c.tile;
    compact_ = compact_layout(c.width, c.height);
    draw(c.parent, c.width, c.height);
  }
  void draw(lv_obj_t *parent, int w, int h) {
    if (compact_) {
      int ih = ui::line_height(Font::ICON_SMALL), vh = ui::line_height(Font::BODY);
      int top = (h - ih - 2 * vh) / 2;
      compact_icon_ = ui::label(parent, Font::ICON_SMALL, theme::INK);
      compact_value_ = ui::label(parent, Font::BODY, theme::INK);
      compact_soc_ = ui::label(parent, Font::BODY, theme::MUTED);
      for (auto *label : {compact_icon_, compact_value_, compact_soc_})
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      place(compact_icon_, 4, top, w - 8, ih);
      place(compact_value_, 4, top + ih, w - 8, vh);
      place(compact_soc_, 4, top + ih + vh, w - 8, vh);
      on_tick(0);
      return;
    }
    // Two views in one tile, a tap on the middle swaps them: the four rings, and the original's one ring.
    four_ = box(parent, w, h);
    one_ = box(parent, w, h);
    parent = four_;
    // The one ring (power-origin-card _renderRing, viewBox 200): r 76, the segments 11 wide over a track.
    {
      // viewBox 200 as the original's, with its outer band (r 93, 5 wide): the house's day in the same colours.
      const int m = std::min(w, h), cx = w / 2, cy = h / 2, R = m * 76 / 200, sw = std::max(3, m * 11 / 200);
      one_at_[0] = cx, one_at_[1] = cy, one_at_[2] = R + sw;
      const int dr = m * 93 / 200, dw = std::max(2, m * 5 / 200);
      one_day_track_ = ring_arc(one_, cx, cy, dr, dw, LV_PART_MAIN);
      put(one_day_track_, LV_STYLE_ARC_OPA, LV_OPA_50, LV_PART_MAIN);
      for (auto *&a : one_day_) a = ring_arc(one_, cx, cy, dr, dw, LV_PART_INDICATOR);
      one_track_ = ring_arc(one_, cx, cy, R, sw, LV_PART_MAIN);
      for (auto *&a : one_seg_) a = ring_arc(one_, cx, cy, R, sw, LV_PART_INDICATOR);
      one_value_ = ui::label(one_, m >= 200 ? Font::HEADLINE : Font::TITLE, theme::INK);
      one_unit_ = ui::label(one_, Font::BODY, theme::MUTED);
      one_caption_ = ui::label(one_, Font::BODY, theme::MUTED);
      for (auto *l : {one_value_, one_unit_}) lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_style_text_align(one_caption_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_letter_space(one_caption_, 2, 0);
      const int vh = ui::line_height(m >= 200 ? Font::HEADLINE : Font::TITLE), lh = ui::line_height(Font::BODY);
      one_y_ = cy - (vh + lh) / 2;
      place(one_caption_, cx - R + sw, one_y_ + vh, 2 * (R - sw), lh);
    }
    // The parts span x 5..315 (grid rim to the charge ring) and y 8..192 of the viewBox.
    const float s = std::min(w / VB_SPAN, h / VB_H);
    const float ox = (w - VB_SPAN * s) / 2 - VB_LEFT * s, oy = (h - VB_H * s) / 2 - VB_TOP * s;
    auto X = [&](float u) { return static_cast<int>(std::lround(ox + u * s)); };
    auto Y = [&](float u) { return static_cast<int>(std::lround(oy + u * s)); };
    auto S = [&](float u) { return std::max(1, static_cast<int>(std::lround(u * s))); };
    // Lines first, behind the circles: PV down to just above the house, grid and store sideways to its rim.
    const float side = CX - HR - 4;
    const float ends[3][4] = {{CX, PY + SR, CX, 100}, {GX + SR, HY, side, HY}, {BX - SR, HY, VB_W - side, HY}};
    for (int i = 0; i < 3; ++i) {
      auto &k = links_[i];
      const float *e = ends[i];
      k.track_pts[0] = {static_cast<lv_value_precise_t>(X(e[0])), static_cast<lv_value_precise_t>(Y(e[1]))};
      k.track_pts[1] = {static_cast<lv_value_precise_t>(X(e[2])), static_cast<lv_value_precise_t>(Y(e[3]))};
      k.track = stroke(parent, S(2));
      lv_line_set_points(k.track, k.track_pts, 2);
      // stroke-dasharray 0.1 7 with round caps: dots as wide as the line (3), every 7 units from the outer end.
      const float len = std::fabs(e[2] - e[0]) + std::fabs(e[3] - e[1]);
      k.dots = std::min(MAX_DOTS, static_cast<int>(len / 7) + 1);
      const int d = S(3);
      for (int j = 0; j < k.dots; ++j) {
        const float t = 7.0f * j / len;
        k.dot[j] = ui::block(parent, theme::TRACK);
        lv_obj_set_style_radius(k.dot[j], LV_RADIUS_CIRCLE, 0);
        place(k.dot[j], X(e[0] + (e[2] - e[0]) * t) - d / 2, Y(e[1] + (e[3] - e[1]) * t) - d / 2, d, d);
      }
      // The arrowhead, 6 long and 8 wide in the original. An lv_line has no fill, so a triangle shrunk about its
      // incentre and drawn with a round stroke 3 wide covers about the original's outline, its corners rounded.
      for (int out = 0; out < 2; ++out) {
        const float tx = out ? e[0] : e[2], ty = out ? e[1] : e[3], dir = out ? -1.0f : 1.0f;
        const float sx = i == 0 ? 0 : dir * (e[2] > e[0] ? 1 : -1), sy = i == 0 ? dir : 0;  // unit vector to the tip
        const float tip = 1.8f, base = 5.2f, half = 2.4f;
        auto *pts = k.head_pts[out];
        auto at = [&](float along, float across) {
          return lv_point_precise_t{static_cast<lv_value_precise_t>(ox + (tx - sx * along - sy * across) * s),
                                    static_cast<lv_value_precise_t>(oy + (ty - sy * along + sx * across) * s)};
        };
        pts[0] = at(tip, 0);
        pts[1] = at(base, -half);
        pts[2] = at(base, half);
        pts[3] = pts[0];
      }
      k.head = stroke(parent, S(3));
      lv_line_set_points(k.head, k.head_pts[0], 4);
    }
    const float cxs[4] = {CX, GX, CX, BX}, cys[4] = {PY, HY, HY, HY}, rs[4] = {SR, SR, HR, SR};
    const bool roomy = s >= 1.2f;
    for (int i = 0; i < 4; ++i) {
      auto &n = nodes_[i];
      const int cx = X(cxs[i]), cy = Y(cys[i]), r = S(rs[i]), sw = S(4);
      if (i == 2) house_at_[0] = cx, house_at_[1] = cy, house_at_[2] = r;
      n.disc = ui::block(parent, theme::TRACK);  // fv-disc: the inset behind the figure
      lv_obj_set_style_radius(n.disc, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_pad_all(n.disc, 0, 0);
      lv_obj_remove_flag(n.disc, LV_OBJ_FLAG_SCROLLABLE);
      place(n.disc, cx - r, cy - r, 2 * r, 2 * r);
      if (i == 3) {
        // flow_battery: charge. The charge outside at r 38, 6 wide; the store's own rim thin in its colour.
        n.soc = ring_arc(parent, cx, cy, S(SR + 6), S(6), LV_PART_MAIN);
        lv_obj_set_style_arc_width(n.soc, S(6), LV_PART_INDICATOR);
        put(n.soc, LV_STYLE_ARC_OPA, LV_OPA_60, LV_PART_MAIN);
        n.track = ring_arc(parent, cx, cy, r, S(2.5f), LV_PART_MAIN);
      } else {
        n.track = ring_arc(parent, cx, cy, r, sw, LV_PART_MAIN);
      }
      for (int j = 0; j < (i == 2 ? 3 : i == 1 ? 2 : 0); ++j) n.segments[j] = ring_arc(parent, cx, cy, r, sw, LV_PART_INDICATOR);
      // The figure bold in the participant's colour, the house's larger; "kW" small and grey under it.
      const Font vf = i == 2 ? (roomy ? Font::HEADLINE : Font::TITLE) : (roomy ? Font::TITLE : Font::BODY);
      n.icon = ui::label(n.disc, Font::ICON_SMALL, theme::MUTED);
      n.value = ui::label(n.disc, vf, theme::INK);
      n.unit = ui::label(n.disc, Font::BODY, theme::MUTED);
      for (auto *label : {n.icon, n.value, n.unit}) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      // Placed as the original: the icon (24 * r/54 units) centred 0.56 r above the centre, the figure's baseline
      // 6 units below it, the unit under the figure. What a small circle cannot hold goes: icon first, then unit.
      const int lw = std::max(1, 2 * (r - sw)), inside = r - sw;
      const int ih = ui::line_height(Font::ICON_SMALL), vh = ui::line_height(vf), uh = ui::line_height(Font::BODY);
      const lv_font_t *face = ui::font(vf);
      const int vy = cy + S(6) - (face ? face->line_height - face->base_line : vh * 4 / 5);
      const int icon_px = S(24 * rs[i] / 54), iy = std::min(cy - static_cast<int>(0.56f * r) - ih / 2, vy - ih);
      const int dx = r - lw / 2, dy = cy - r;  // the labels are the disc's children
      place(n.icon, dx, iy - dy, lw, ih);
      place(n.value, dx, vy - dy, lw, vh);
      place(n.unit, dx, vy + vh - dy, lw, uh);
      show(n.icon, iy + ih / 2 - icon_px / 2 >= cy - inside);
      show(n.unit, vy + vh + uh <= cy + inside + S(4));
      // No transform on the icons: LVGL draws each transformed label through a layer of its own, and the four made the
      // pressed tile's redraw take 440 ms, so a tap became a long press and the tile did not open.
    }
    // The day's hours (the original's hour strip) in the room beside PV, above grid and store: 0-12 left, 12-24 right.
    hours_shown_ = s >= 1.1f && !hours_off_;
    if (hours_shown_) {
      const int lh = ui::line_height(Font::BODY), top = Y(10), bottom = Y(HY - SR - 10) - lh - 2;
      const float x0[2] = {10, CX + SR + 12}, span = CX - SR - 22;
      for (int h = 0; h < 24; ++h) {
        const float slot = span / 12, x = x0[h / 12] + (h % 12) * slot;
        hour_x_[h] = X(x + slot * 0.2f);
        hour_w_ = std::max(2, S(slot * 0.6f));
        make_hour(parent, h);
      }
      hour_top_ = top, hour_base_ = bottom;
      const int at[5] = {X(x0[0]), X(x0[0] + span / 2), X(x0[1]), X(x0[1] + span / 2), X(x0[1] + span)};
      const lv_text_align_t align[5] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT};
      for (int k = 0; k < 5; ++k) {
        hour_label_[k] = ui::label(parent, Font::BODY, theme::MUTED);
        lv_obj_set_style_text_align(hour_label_[k], align[k], 0);
        const int lw = 28;
        place(hour_label_[k], align[k] == LV_TEXT_ALIGN_LEFT ? at[k] : align[k] == LV_TEXT_ALIGN_RIGHT ? at[k] - lw : at[k] - lw / 2, bottom + 2, lw, lh);
        say(hour_label_[k], std::to_string(k * 6));
      }
    }
    on_tick(0);
  }
  // The card's view: only today's hours, as bars across the whole width, the times 0..24 under them.
  void draw_hours(lv_obj_t *parent, int x, int y, int w, int h) {
    hours_only_ = hours_shown_ = true, hour_dots_ = false;
    const int lh = ui::line_height(Font::BODY);
    const float slot = static_cast<float>(w) / 24;
    for (int k = 0; k < 24; ++k) {
      hour_x_[k] = x + static_cast<int>(k * slot + slot * 0.15f);
      make_hour(parent, k);
    }
    hour_left_ = x, hour_span_ = w;
    hour_w_ = std::max(2, static_cast<int>(slot * 0.7f));
    hour_top_ = y, hour_base_ = y + h - lh - 2;
    for (int k = 0; k < 5; ++k) {
      hour_label_[k] = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(hour_label_[k], k ? k < 4 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
      const int at = x + w * k / 4, lw = 28;
      place(hour_label_[k], k ? k < 4 ? at - lw / 2 : at - lw : at, hour_base_ + 2, lw, lh);
      say(hour_label_[k], std::to_string(k * 6));
    }
    on_tick(0);
  }
  // A card's Flow takes the tile's entities, to keep asking for the hours while it is open.
  void seed(const tessera::Tile *from) {
    if (!from) return;
    const auto *f = static_cast<const Flow *>(from);
    home_entity_ = f->home_entity_, grid_entity_ = f->grid_entity_, battery_entity_ = f->battery_entity_;
  }
  void on_state(JsonObjectConst data) override {
    const char *names[] = {"solar", "grid", "home", "battery", "soc", "import_today", "export_today", "home_today"};
    for (int i = 0; i < 8; ++i) {
      JsonObjectConst entity = data["entities"][names[i]];
      values_[i] = value(entity["state"] | static_cast<const char *>(nullptr),
                         entity["attributes"]["unit_of_measurement"] | static_cast<const char *>(nullptr),
                         i == 4 ? Unit::SOC : i > 4 ? Unit::ENERGY : Unit::POWER);
      if (i < 4) unit_w_[i] = value("1", entity["attributes"]["unit_of_measurement"] | static_cast<const char *>(nullptr), Unit::POWER);
    }
    for (int i = 0; i < 2; ++i)
      price_[i] = entity_value(data, i ? "export_price" : "import_price", Unit::PRICE);
    on_tick(0);
  }
  void on_tick(uint32_t) override {
    if (compact_) {
      say(compact_icon_, ui::icon(0xF02DC));
      ui::set_color(compact_icon_, theme::INK);
      say(compact_value_, power(values_[2]));
      ui::set_color(compact_value_, theme::INK);
      char buf[24];
      if (std::isfinite(values_[4])) snprintf(buf, sizeof(buf), T("soc_short"), values_[4]);
      else snprintf(buf, sizeof(buf), "%s", T("soc_none"));
      say(compact_soc_, buf);
      ui::set_color(compact_soc_, theme::MUTED);
      return;
    }
    float v[4] = {values_[0], values_[1], values_[2], values_[3]};
    if (invert_grid_) v[1] = -v[1];
    if (invert_battery_) v[3] = -v[3];
    if (hours_only_) return hour_tick(v);
    const bool importing = std::isfinite(v[1]) && v[1] > LIVE_W;
    // The original's colours: sun, grid (red while importing), the house in ink, the store's leaf.
    const Paint sun{Tone::NORMAL, theme::ENERGY_SOLAR}, leaf{Tone::NORMAL, theme::ENERGY_BATTERY_OUT};
    const Paint grid{importing ? Tone::ALERT : Tone::NORMAL, theme::ENERGY_GRID};
    const Paint paints[] = {sun, grid, {Tone::NORMAL, theme::INK}, leaf};
    const lv_color_t track = ui::color(theme::RAISED_LINE);
    const uint32_t icons[] = {0xF0599, 0xF0D3E, 0xF02DC, 0xF008E};
    // fv-node.quiet: PV and grid at 45 % while they stand still; on the light look that would be too faint to read.
    const bool quiet[] = {!(v[0] >= LIVE_W), !importing && !(v[1] < -LIVE_W), false, false};
    for (int i = 0; i < 4; ++i) {
      auto &n = nodes_[i];
      ui::set_fill(n.disc, Tone::NORMAL, theme::TRACK);
      put(n.track, LV_STYLE_ARC_COLOR, i == 3 && std::isfinite(v[3]) ? color(leaf) : track, LV_PART_MAIN);
      say(n.icon, ui::icon(icons[i]));
      char buf[16];
      kw_text(v[i], buf, sizeof(buf));
      say(n.value, buf);
      say(n.unit, "kW");
      // On the light look the sun's and the leaf's own colours are too faint for words: ink there.
      const Paint text = theme::dark || (i != 0 && i != 3) ? paints[i] : Paint{Tone::NORMAL, theme::INK};
      ui::set_tone(n.value, text.tone, text.role);
      ui::set_tone(n.icon, text.tone, i == 2 ? theme::MUTED : text.role);
      ui::set_color(n.unit, theme::MUTED);
      const lv_opa_t opa = quiet[i] && theme::dark ? static_cast<lv_opa_t>(LV_OPA_COVER * 45 / 100) : LV_OPA_COVER;
      // Each part faded on its own: a whole-object opacity draws the disc through a layer, slow on every press.
      put(n.disc, LV_STYLE_BG_OPA, opa, 0);
      put(n.track, LV_STYLE_ARC_OPA, opa, LV_PART_MAIN);
      for (auto *l : {n.icon, n.value, n.unit}) put(l, LV_STYLE_TEXT_OPA, opa, 0);
    }
    // flow_house: gauge. Where the house's power comes from, from the top clockwise: sun, store, grid.
    auto shares = house_shares(v[2], v[1], v[3], v[0]);
    const float parts[] = {shares.solar, shares.battery, shares.grid};
    const Paint hues[] = {sun, leaf, grid};
    float offset = 0;
    for (int i = 0; i < 3; ++i) {
      auto *a = nodes_[2].segments[i];
      show(a, shares.known && parts[i] * v[2] > 5);
      put(a, LV_STYLE_ARC_COLOR, color(hues[i]), LV_PART_INDICATOR);
      int start = int(offset * 360); offset += parts[i];
      angles(a, start, int(offset * 360));
    }
    // flow_grid: day. Export clockwise from the top in the sun's colour, import back from the top in the grid's.
    float imp=0, exp=0;
    bool day = day_shares(values_[5], values_[6], imp, exp);
    const float dayparts[] = {exp, imp};
    for (int i=0; i<2; ++i) {
      auto *a = nodes_[1].segments[i]; float part = dayparts[i];
      show(a, day && part > 0.005f);
      put(a, LV_STYLE_ARC_COLOR, color(i ? grid : sun), LV_PART_INDICATOR);
      angles(a, i ? int(360-part*360) : 0, i ? 360 : int(part*360));
    }
    float soc = values_[4];
    auto *arc = nodes_[3].soc;
    put(arc, LV_STYLE_ARC_COLOR, track, LV_PART_MAIN);
    put(arc, LV_STYLE_ARC_COLOR, color(leaf), LV_PART_INDICATOR);
    angles(arc, 0, std::max(0, soc_angle(soc)));
    // Grid + = import; battery + = charge. Solar positive = production. The arrow at the end the power flows to.
    const float flow[] = {v[0], v[1], v[3]};
    const bool out[] = {false, v[1] < 0, v[3] > 0};
    const Paint colors[] = {sun, grid, leaf};
    for (int i=0; i<3; ++i) {
      auto &k = links_[i];
      bool active = std::isfinite(flow[i]) && std::fabs(flow[i]) > LIVE_W && (i || flow[i] > 0);
      show(k.track, !active);
      put(k.track, LV_STYLE_LINE_COLOR, track, 0);
      for (int j = 0; j < k.dots; ++j) {
        show(k.dot[j], active);
        ui::set_fill(k.dot[j], colors[i].tone, colors[i].role);
      }
      show(k.head, active);
      if (lv_line_get_points(k.head) != k.head_pts[out[i] ? 1 : 0]) lv_line_set_points(k.head, k.head_pts[out[i] ? 1 : 0], 4);
      put(k.head, LV_STYLE_LINE_COLOR, color(colors[i]), 0);
    }
    if (hours_shown_) hour_tick(v);
    show(four_, !one_ring_);
    show(one_, one_ring_);
    if (one_ring_) one_tick(v, shares, sun, leaf, grid);
  }
  // The one ring as the original's centre modes power and surplus: by day what is free (to the store and the grid,
  // "FREI"), the house's own sun receding; otherwise where the house's power comes from, the house in the middle.
  // ponytail: the sun from PV > 150 W, no hysteresis, no outer day band, no cycling centre; add them if wanted.
  void one_tick(const float (&v)[4], const Shares &sh, Paint sun, Paint leaf, Paint grid) {
    const bool producing = std::isfinite(v[0]) && v[0] > 150;
    const float home = sh.known ? v[2] : NAN;
    float p[3], centre;
    Paint hue[3] = {sun, leaf, grid};
    const char *caption;
    const float per_h = centre_ == 2 ? cost_per_hour(v[1], price_[0], price_[1]) : NAN;
    if (std::isfinite(per_h)) {  // the original's centre mode money: what the grid costs (or brings) this hour
      p[0] = home * sh.solar, p[1] = home * sh.battery, p[2] = home * sh.grid;
      centre = std::fabs(per_h);
      caption = T(per_h >= 0 ? "cost_buy" : "cost_sell");
    } else if (producing && centre_ == 0) {
      p[0] = home * sh.solar;
      p[1] = std::isfinite(v[3]) ? std::max(0.0f, v[3]) : NAN;
      p[2] = std::isfinite(v[1]) ? std::max(0.0f, -v[1]) : NAN;
      hue[2] = {Tone::NORMAL, theme::ENERGY_GRID};
      centre = (std::isfinite(p[1]) ? p[1] : 0) + (std::isfinite(p[2]) ? p[2] : 0);
      caption = T(centre > LIVE_W ? "free" : "none_free");
    } else {
      p[0] = home * sh.solar, p[1] = home * sh.battery, p[2] = home * sh.grid;
      centre = v[2];
      caption = T("house");
    }
    float st[3], ln[3];
    const int used = ring_parts(p, st, ln);
    if (!std::isfinite(per_h) && !(producing && centre_ == 0) && used == 1) caption = T(ln[0] > 0 ? "src_solar" : ln[1] > 0 ? "src_battery" : "src_grid");
    put(one_track_, LV_STYLE_ARC_COLOR, ui::color(theme::RAISED_LINE), LV_PART_MAIN);
    for (int i = 0; i < 3; ++i) {
      show(one_seg_[i], ln[i] > 0);
      put(one_seg_[i], LV_STYLE_ARC_COLOR, color(hue[i]), LV_PART_INDICATOR);
      put(one_seg_[i], LV_STYLE_ARC_OPA, producing && i == 0 ? LV_OPA_COVER * 45 / 100 : LV_OPA_COVER, LV_PART_INDICATOR);
      angles(one_seg_[i], static_cast<int>(st[i] * 360), static_cast<int>((st[i] + ln[i]) * 360));
    }
    char buf[16];
    if (std::isfinite(per_h)) snprintf(buf, sizeof(buf), "%.2f", centre);
    else kw_text(centre, buf, sizeof(buf));
    say(one_value_, buf);
    say(one_unit_, std::isfinite(per_h) ? "€/h" : "kW");
    // The outer band: the house's day so far, grid (today's import), store (today's discharge), the rest the sun.
    float d[3] = {NAN, NAN, NAN};
    const float day_home = values_[7], day_grid = values_[5];
    if (std::isfinite(day_home) && std::isfinite(day_grid) && day_home > 0) {
      d[2] = std::min(day_grid, day_home);
      d[1] = std::isfinite(plugin_->store_out) ? std::clamp(plugin_->store_out, 0.0f, day_home - d[2]) : 0;
      d[0] = day_home - d[2] - d[1];
    }
    float dst[3], dln[3];
    const bool day = std::isfinite(d[0]) && ring_parts(d, dst, dln) > 0;
    show(one_day_track_, day);
    put(one_day_track_, LV_STYLE_ARC_COLOR, ui::color(theme::RAISED_LINE), LV_PART_MAIN);
    const Paint day_hue[3] = {sun, leaf, {Tone::NORMAL, theme::ENERGY_GRID}};
    for (int i = 0; i < 3; ++i) {
      show(one_day_[i], day && dln[i] > 0);
      if (!day || dln[i] <= 0) continue;
      put(one_day_[i], LV_STYLE_ARC_COLOR, color(day_hue[i]), LV_PART_INDICATOR);
      angles(one_day_[i], static_cast<int>(dst[i] * 360), static_cast<int>((dst[i] + dln[i]) * 360));
    }
    say(one_caption_, caption);
    ui::set_color(one_value_, theme::INK);
    ui::set_color(one_unit_, theme::MUTED);
    ui::set_color(one_caption_, theme::MUTED);
    // "3,87 kW" centred as one: the figure, then the unit small on its baseline.
    lv_obj_update_layout(one_value_);
    lv_obj_update_layout(one_unit_);
    const int vw = lv_obj_get_width(one_value_), uw = lv_obj_get_width(one_unit_), x = one_at_[0] - (vw + 3 + uw) / 2;
    lv_obj_set_pos(one_value_, x, one_y_);
    lv_obj_set_pos(one_unit_, x + vw + 3, one_y_ + lv_obj_get_height(one_value_) - lv_obj_get_height(one_unit_) - 4);
  }
  // Each hour as the source that carried most of it (sun orange, store teal, grid red), its height the house's mean;
  // the hour under way from the live values, the hours to come a stub of track.
  void hour_tick(const float (&v)[4]) {
    if (tessera::epoch() >= 86400) {
      const char *const ids[3] = {home_entity_.c_str(), grid_entity_.c_str(), battery_entity_.c_str()};
      plugin_->want_hours(ids, tessera::epoch());
    }
    const uint32_t now = tessera::epoch(), from = plugin_->hours_from;
    const int current = from && now >= from ? static_cast<int>((now - from) / 3600) : -1;
    int src[24];
    float top = 1;
    for (int h = 0; h < 24; ++h) {
      float hv = NAN, gv = NAN, bv = NAN;
      if (h == current) hv = v[2], gv = v[1], bv = v[3];
      else if (h < current) {
        hv = plugin_->hour_mean[0][h] * unit_w_[2];
        gv = plugin_->hour_mean[1][h] * unit_w_[1] * (invert_grid_ ? -1 : 1);
        bv = plugin_->hour_mean[2][h] * unit_w_[3] * (invert_battery_ ? -1 : 1);
      }
      hour_home_[h] = hv;
      src[h] = dominant_source(hv, gv, bv);
      const Shares s = house_shares(hv, std::isfinite(gv) ? gv : 0, std::isfinite(bv) ? bv : 0, 0);
      hour_share_[h][0] = s.known ? s.solar : NAN, hour_share_[h][1] = s.battery, hour_share_[h][2] = s.grid;
      if (std::isfinite(hv)) top = std::max(top, hv);
    }
    hour_now_ = current;
    const Paint paints[3] = {{Tone::NORMAL, theme::ENERGY_SOLAR}, {Tone::NORMAL, theme::ENERGY_BATTERY_OUT}, {Tone::ALERT, theme::ENERGY_GRID}};
    for (int h = 0; h < 24; ++h) {
      const lv_opa_t opa = h == current || src[h] < 0 ? LV_OPA_COVER : LV_OPA_COVER * 70 / 100;
      const int bh = src[h] < 0 ? 3 : std::max(3, static_cast<int>((hour_base_ - hour_top_) * hour_home_[h] / top));
      int sh[3];
      for (int k = 0; k < 3; ++k) sh[k] = src[h] < 0 || !hours_only_ ? 0 : static_cast<int>(bh * hour_share_[h][k] + 0.5f);
      // Restyling ~100 parts every second held the main loop up for 200 ms, and taps got lost: only what changed.
      const uint32_t key = ((src[h] + 1) | (opa == LV_OPA_COVER) << 3 | (hour_dots_ ? 0 : bh) << 4) ^ (sh[0] << 13 | sh[1] << 19 | sh[2] << 25);
      if (hour_key_[h] == key + 1) continue;
      hour_key_[h] = key + 1;
      // The tile: one part an hour in the source that carried it most. 72 more parts there made the pressed tile's
      // redraw so slow that every tap lasted over 400 ms, a long press, and the tile never opened. Split on the card.
      if (hour_dots_ || !hours_only_) {
        const int d = std::min(hour_w_, 8);
        if (hour_dots_) place(hour_bar_[h], hour_x_[h] + (hour_w_ - d) / 2, hour_base_ - d, d, d);
        else place(hour_bar_[h], hour_x_[h], hour_base_ - bh, hour_w_, bh);
        if (src[h] < 0) ui::set_fill(hour_bar_[h], Tone::NORMAL, theme::TRACK);
        else ui::set_fill(hour_bar_[h], paints[src[h]].tone, paints[src[h]].role);
        put(hour_bar_[h], LV_STYLE_BG_OPA, opa, 0);
        continue;
      }
      // Bars as the original's: the hour's height the house's mean, split bottom-up into sun, store and grid.
      place(hour_bar_[h], hour_x_[h], hour_base_ - bh, hour_w_, bh);
      ui::set_fill(hour_bar_[h], Tone::NORMAL, theme::TRACK);
      show(hour_bar_[h], src[h] < 0);
      int y = hour_base_;
      for (int k = 0; k < 3; ++k) {
        show(hour_seg_[h][k], sh[k] > 0);
        if (sh[k] <= 0) continue;
        y -= sh[k];
        place(hour_seg_[h][k], hour_x_[h], y, hour_w_, sh[k]);
        ui::set_fill(hour_seg_[h][k], paints[k].tone, paints[k].role);
        put(hour_seg_[h][k], LV_STYLE_BG_OPA, opa, 0);
      }
    }
  }
  // The card: which hour a tap hit (-1 none), and what that hour was.
  int hour_at(tessera::Point p) const {
    if (!hours_only_ || p.x < hour_left_ || p.x >= hour_left_ + hour_span_ || p.y < hour_top_ - 8 || p.y > hour_base_ + 24) return -1;
    return std::clamp((p.x - hour_left_) * 24 / std::max(1, hour_span_), 0, 23);
  }
  std::string hour_text(int h) const {
    char buf[96];
    const float *s = hour_share_[h];
    if (h > hour_now_ || !std::isfinite(hour_home_[h]) || !std::isfinite(s[0]))
      snprintf(buf, sizeof(buf), T("hour_none"), h);
    else {
      char kw[16];
      kw_text(hour_home_[h], kw, sizeof(kw));
      snprintf(buf, sizeof(buf), T("hour_split"), h, kw, s[0] * 100, s[1] * 100, s[2] * 100);
    }
    return buf;
  }
  float house_mean_w() const {  // the house's mean over today's known hours, W
    float sum = 0;
    int got = 0;
    for (int h = 0; h <= hour_now_ && h < 24; ++h) if (std::isfinite(hour_home_[h])) sum += hour_home_[h], ++got;
    return got ? sum / got : NAN;
  }
  float grid_now() const { return invert_grid_ ? -values_[1] : values_[1]; }
  // A tap inside the middle ring (the house, or the one ring) swaps the view; anywhere else opens the card.
  bool swaps(tessera::Point p) const {
    if (compact_ || p.x < 0) return false;
    const int *at = one_ring_ ? one_at_ : house_at_;
    const int dx = p.x - at[0], dy = p.y - at[1];
    return dx * dx + dy * dy <= at[2] * at[2];
  }
  void on_theme() override {
    for (auto &k : hour_key_) k = 0;  // a new look: every hour again
    for (auto *l : hour_label_) if (l) ui::set_color(l, theme::MUTED);
    on_tick(0);
  }
  void on_tap() override;
  void set_inversion(bool grid, bool battery) { invert_grid_ = grid; invert_battery_ = battery; }
 private:
  void make_hour(lv_obj_t *parent, int h) {
    hour_bar_[h] = ui::block(parent, theme::TRACK);
    lv_obj_set_style_radius(hour_bar_[h], hour_dots_ ? LV_RADIUS_CIRCLE : 2, 0);
    if (hours_only_)
      for (auto *&s : hour_seg_[h]) s = ui::block(parent, theme::TRACK), lv_obj_set_style_radius(s, 1, 0);
  }
  static lv_obj_t *box(lv_obj_t *parent, int w, int h) {
    auto *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    place(b, 0, 0, w, h);
    return b;
  }
  lv_obj_t *four_{}, *one_{}, *one_track_{}, *one_seg_[3]{}, *one_value_{}, *one_unit_{}, *one_caption_{};
  lv_obj_t *one_day_track_{}, *one_day_[3]{};
  int centre_{0};  // the one ring's middle: 0 as the moment wants (free by day, else the house), 1 the house, 2 cost
  int one_at_[3]{}, house_at_[3]{}, one_y_{0};
  lv_obj_t *hour_bar_[24]{}, *hour_label_[5]{};
  lv_obj_t *hour_seg_[24][3]{};
  int hour_x_[24]{}, hour_w_{2}, hour_top_{0}, hour_base_{1}, hour_left_{0}, hour_span_{1}, hour_now_{-1};
  float hour_home_[24]{}, hour_share_[24][3]{};
  uint32_t hour_key_[24]{};  // what each hour shows now, +1 (0: not drawn yet)
  bool hours_shown_{false}, hour_dots_{true}, hours_off_{false}, hours_only_{false};
  float unit_w_[4]{1, 1, 1, 1};
  bool one_ring_{false};
  static constexpr int MAX_DOTS = 6;
  struct Node { lv_obj_t *disc{}, *track{}, *icon{}, *value{}, *unit{}, *soc{}, *segments[3]{}; } nodes_[4];
  struct Link {
    lv_obj_t *track{}, *head{}, *dot[MAX_DOTS]{};
    int dots{0};
    lv_point_precise_t track_pts[2]{}, head_pts[2][4]{};
  } links_[3];
  PowerOrigin *plugin_;
  float values_[8]{NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  float price_[2]{NAN, NAN};  // import, export per kWh
  lv_obj_t *compact_icon_{}, *compact_value_{}, *compact_soc_{};
  bool compact_{false};
  bool invert_grid_{false}, invert_battery_{false};
  tessera::Text home_entity_, grid_entity_, battery_entity_;
  int tile_{-1};
};

// The 12-hour SoC strip shared by both cards: a muted title, one bar per hourly mean, the times under the base line.
struct SocChart {
  explicit SocChart(PowerOrigin *p) : plugin_(p) {}
  // From row `y` (the title) down to the card's foot `height`.
  void build(lv_obj_t *parent, const std::string &entity, int y, int width, int height) {
    entity_ = entity;
    const int lh = ui::line_height(Font::BODY);
    title_ = ui::label(parent, Font::BODY, theme::MUTED);
    place(title_, 8, y, width - 16, lh);
    top_ = y + lh + 2;
    bottom_ = std::max(top_ + 8, height - lh - 4);
    base_ = ui::block(parent, theme::TRACK);
    place(base_, 8, bottom_, width - 16, 1);
    for (int i = 0; i < 2; ++i) {
      times_[i] = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(times_[i], i ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
      const int half = (width - 16) / 2;
      place(times_[i], 8 + i * half, bottom_ + 3, half, lh);
    }
    const int gap = 2, usable = std::max(1, width - 16);
    bar_w_ = std::max(2, (usable - (HISTORY_SLOTS - 1) * gap) / HISTORY_SLOTS);
    const int used = HISTORY_SLOTS * bar_w_ + (HISTORY_SLOTS - 1) * gap;
    const int x0 = 8 + std::max(0, (usable - used) / 2);
    for (int i = 0; i < HISTORY_SLOTS; ++i) {
      bars_[i] = ui::block(parent, theme::ACCENT);
      bar_x_[i] = x0 + i * (bar_w_ + gap);
      place(bars_[i], bar_x_[i], bottom_, bar_w_, 1);
      show(bars_[i], false);
    }
    update();
    request(tessera::epoch());
  }
  void set_unit(const char *unit) { unit_ = unit ? unit : ""; }
  void tick(uint32_t epoch) {
    plugin_->expire_history(epoch);
    request(epoch);
    update();
  }
  void theme() {
    if (title_) ui::set_color(title_, theme::MUTED);
    for (auto *t : times_) if (t) ui::set_color(t, theme::MUTED);
    if (base_) ui::set_fill(base_, Tone::NORMAL, theme::TRACK);
    update();
  }
  // Once on opening, then again when the hour turns while the card stays open. A failed send waits for the next hour.
  void request(uint32_t epoch) {
    if (entity_.empty() || epoch < 13 * 3600 || hour_floor(epoch) == asked_hour_) return;
    asked_hour_ = hour_floor(epoch);
    requested_ = true;
    plugin_->request_history(std::string(entity_.c_str()), epoch);
  }
  void update() {
    if (!title_ || !base_) return;
    const HistoryWindow &w = plugin_->history();
    const bool unit_ok = unit_ == "%";
    const bool ready = unit_ok && requested_ && plugin_->history_ready();
    int known = 0;
    float first = NAN;
    for (const auto &slot : w.slots)
      if (slot.known) {
        if (!known) first = slot.mean;
        ++known;
      }
    char text[80];
    if (entity_.empty()) snprintf(text, sizeof(text), "%s", T("hist_none"));
    else if (!unit_.empty() && !unit_ok) snprintf(text, sizeof(text), "%s", T("hist_unit"));
    else if (!requested_ || plugin_->history_pending() || unit_.empty())
      snprintf(text, sizeof(text), "%s", T("hist_loading"));
    else if (!ready) snprintf(text, sizeof(text), "%s", T("hist_unavailable"));
    else if (!known) snprintf(text, sizeof(text), "%s", T("hist_nodata"));
    else if (known < HISTORY_SLOTS)
      snprintf(text, sizeof(text), T("hist_missing"), HISTORY_SLOTS - known);
    else snprintf(text, sizeof(text), "%s", T("hist_ok"));
    say(title_, text);
    const bool drawn = ready && known > 0;
    if (drawn) {
      std::string left = tessera::clock_text(w.from);
      if (std::isfinite(first)) {
        char pct[16];
        snprintf(pct, sizeof(pct), " · %.0f %%", first);  // the first known hour, as the original card labels it
        left += pct;
      }
      say(times_[0], left);
      say(times_[1], tessera::clock_text(w.from + HISTORY_SLOTS * 3600));
    } else {
      say(times_[0], "");
      say(times_[1], "");
    }
    const int room = std::max(1, bottom_ - top_);
    for (int i = 0; i < HISTORY_SLOTS; ++i) {
      const auto &slot = w.slots[i];
      if (!drawn || !slot.known) {  // a gap stays empty: nothing is drawn across it
        show(bars_[i], false);
        continue;
      }
      const float pct = std::min(100.0f, std::max(0.0f, slot.mean));
      const int h = std::max(1, static_cast<int>(pct / 100.0f * room + 0.5f));
      place(bars_[i], bar_x_[i], bottom_ - h, bar_w_, h);
      show(bars_[i], true);
      // In the store's colours: the sun's orange where it rose, the grid's red where it gave, the leaf where still.
      const int trend = soc_trend(i && w.slots[i - 1].known ? w.slots[i - 1].mean : NAN, slot.mean);
      if (trend == 1) ui::set_fill(bars_[i], Tone::NORMAL, theme::ENERGY_SOLAR);
      else if (trend == 2) ui::set_fill(bars_[i], Tone::ALERT, theme::ENERGY_GRID);
      else ui::set_fill(bars_[i], Tone::NORMAL, theme::ENERGY_BATTERY_OUT);
    }
  }

 private:
  PowerOrigin *plugin_;
  lv_obj_t *title_{}, *base_{}, *times_[2]{}, *bars_[HISTORY_SLOTS]{};
  int bar_x_[HISTORY_SLOTS]{}, bar_w_{2}, top_{0}, bottom_{0};
  tessera::Text entity_, unit_;
  uint32_t asked_hour_{0};
  bool requested_{false};
};


// "History & today" after the original's hour view: the house's hourly means for the last 6 h up to now, Solcast's PV
// forecast for the next 6 h, today's use and the PV still expected.
struct DayChart {
  static constexpr int AREAS = 13, SPAN = 6 * 3600;
  explicit DayChart(PowerOrigin *p) : plugin_(p) {}
  void build(lv_obj_t *parent, const std::string &home, int y, int width, int height) {
    home_ = home;
    left_ = 8;
    span_w_ = width - 16;
    const int lh = ui::line_height(Font::BODY);
    head_ = ui::label(parent, Font::BODY, theme::INK);
    badge_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(badge_, LV_TEXT_ALIGN_RIGHT, 0);
    place(head_, 8, y, width / 2 - 8, lh);
    place(badge_, width / 2, y, width / 2 - 8, lh);
    top_ = y + lh + 4;
    bottom_ = std::max(top_ + 16, height - 2 * lh - 6);
    grid_ = ui::block(parent, theme::TRACK);
    place(grid_, 8, top_, span_w_, 1);
    base_ = ui::block(parent, theme::TRACK);
    place(base_, 8, bottom_, span_w_, 1);
    scale_ = ui::label(parent, Font::BODY, theme::MUTED);
    place(scale_, 8, top_ + 2, 80, lh);
    for (auto *&a : areas_) {
      a = ui::block(parent, theme::ENERGY_SOLAR);
      put(a, LV_STYLE_BG_OPA, LV_OPA_30, 0);
      lv_obj_set_style_radius(a, 0, 0);
      show(a, false);
    }
    pv_ = stroke(parent, 2);
    house_ = stroke(parent, 2);
    now_ = stroke(parent, 1);
    lv_obj_set_style_line_dash_width(now_, 2, 0);
    lv_obj_set_style_line_dash_gap(now_, 3, 0);
    const int mid = 8 + span_w_ / 2;
    now_pts_[0] = {static_cast<lv_value_precise_t>(mid), static_cast<lv_value_precise_t>(top_)};
    now_pts_[1] = {static_cast<lv_value_precise_t>(mid), static_cast<lv_value_precise_t>(bottom_)};
    lv_line_set_points(now_, now_pts_, 2);
    const lv_text_align_t align[] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT};
    for (int i = 0; i < 3; ++i) {
      times_[i] = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(times_[i], align[i], 0);
      place(times_[i], 8 + i * span_w_ / 3, bottom_ + 3, span_w_ / 3, lh);
    }
    sum_ = ui::label(parent, Font::BODY, theme::MUTED);
    place(sum_, 8, bottom_ + 3 + lh, span_w_, lh);
    theme();
    tick(tessera::epoch());
  }
  void on_state(JsonObjectConst data, bool invert_grid) {
    home_w_ = entity_value(data, "home", Unit::POWER);
    solar_w_ = entity_value(data, "solar", Unit::POWER);
    grid_w_ = entity_value(data, "grid", Unit::POWER);
    if (invert_grid) grid_w_ = -grid_w_;
    used_ = entity_value(data, "home_today", Unit::ENERGY);
    // The statistic's means are in the sensor's own unit: W per unit, as value() reads it ("kW" -> 1000).
    unit_w_ = value("1", data["entities"]["home"]["attributes"]["unit_of_measurement"] | static_cast<const char *>(nullptr), Unit::POWER);
    update();
  }
  void tick(uint32_t epoch) {
    plugin_->expire_history(epoch);
    if (!home_.empty() && epoch >= 13 * 3600 && hour_floor(epoch) != asked_hour_) {
      asked_hour_ = hour_floor(epoch);
      plugin_->request_history(std::string(home_.c_str()), epoch);
    }
    plugin_->want_forecast(epoch);
    update();
  }
  void theme() {
    for (auto *l : {scale_, times_[0], times_[1], times_[2], sum_}) if (l) ui::set_color(l, theme::MUTED);
    if (!grid_) return;
    ui::set_color(head_, theme::INK);
    ui::set_fill(grid_, Tone::NORMAL, theme::TRACK);
    ui::set_fill(base_, Tone::NORMAL, theme::TRACK);
    for (auto *a : areas_) ui::set_fill(a, Tone::NORMAL, theme::ENERGY_SOLAR);
    put(pv_, LV_STYLE_LINE_COLOR, ui::color(theme::ENERGY_SOLAR), 0);
    put(house_, LV_STYLE_LINE_COLOR, ui::color(theme::INK), 0);
    put(now_, LV_STYLE_LINE_COLOR, ui::color(theme::MUTED), 0);
    update();
  }
  void update() {
    if (!grid_) return;
    const uint32_t now = tessera::epoch();
    char buf[96], kw[16];
    kw_text(solar_w_, kw, sizeof(kw));
    snprintf(buf, sizeof(buf), T("pv_now"), kw);
    say(head_, buf);
    // The badge as the original's: the grid's word while it moves power, red while it imports.
    const bool importing = std::isfinite(grid_w_) && grid_w_ > LIVE_W, exporting = std::isfinite(grid_w_) && grid_w_ < -LIVE_W;
    say(badge_, importing ? T("grid_import") : exporting ? T("grid_export") : "");
    ui::set_tone(badge_, importing ? Tone::ALERT : Tone::NORMAL, exporting ? theme::ENERGY_SOLAR : theme::MUTED);
    if (now < 86400) return;
    const uint32_t from = now - SPAN;
    // The house: the trailing run of known hourly means within the window, then the live value at now. A gap ends it
    // (nothing is drawn across a gap, nothing is invented before it).
    const HistoryWindow &w = plugin_->history();
    const bool hist = plugin_->history_ready() && std::isfinite(unit_w_);
    int n = 0;
    uint32_t at[HISTORY_SLOTS + 1];
    float watt[HISTORY_SLOTS + 1];
    for (int i = HISTORY_SLOTS - 1; hist && i >= 0; --i) {
      const uint32_t mid = w.from + i * 3600 + 1800;
      if (!w.slots[i].known || mid < from) break;
      at[n] = mid;
      watt[n++] = w.slots[i].mean * unit_w_;
    }
    std::reverse(at, at + n);
    std::reverse(watt, watt + n);
    if (std::isfinite(home_w_)) { at[n] = now; watt[n++] = home_w_; }
    // PV ahead: Solcast's half hours that end after now and start before now + 6 h; the rest of today for the sum.
    const PvForecast &f = plugin_->forecast();
    float top = 0, left_kwh = 0;
    int k = 0;
    const tessera::LocalTime lt = tessera::local_time(now);
    const uint32_t tonight = now + 86400 - (lt.hour * 3600 + lt.minute * 60 + lt.second);
    uint32_t pv_at[AREAS];
    float pv_w[AREAS];
    for (int i = 0; i < f.n; ++i) {
      const uint32_t s = f.from + i * 1800;
      if (s >= tonight) break;
      if (s + 1800 <= now || !std::isfinite(f.kw[i])) continue;
      left_kwh += f.kw[i] / 2;  // ponytail: the half hour under way counts whole
      if (s < now + SPAN && k < AREAS) { pv_at[k] = std::max(s, now); pv_w[k++] = f.kw[i] * 1000; }
    }
    for (int i = 0; i < n; ++i) top = std::max(top, watt[i]);
    for (int i = 0; i < k; ++i) top = std::max(top, pv_w[i]);
    top = chart_top_w(top);
    snprintf(buf, sizeof(buf), "%.0f kW", top / 1000);
    say(scale_, buf);
    auto X = [&](uint32_t t) { return static_cast<lv_value_precise_t>(left_ + static_cast<float>(t - from) * span_w_ / (2 * SPAN)); };
    auto Y = [&](float v) { return static_cast<lv_value_precise_t>(bottom_ - std::clamp(v / top, 0.0f, 1.0f) * (bottom_ - top_)); };
    for (int i = 0; i < n; ++i) house_pts_[i] = {X(at[i]), Y(watt[i])};
    lv_line_set_points(house_, house_pts_, n);
    show(house_, n > 1);
    for (int i = 0; i < AREAS; ++i) {
      show(areas_[i], i < k);
      if (i >= k) continue;
      const uint32_t end = std::min<uint32_t>(i + 1 < k ? pv_at[i + 1] : pv_at[i] + 1800, now + SPAN);
      const int y = Y(pv_w[i]);
      place(areas_[i], X(pv_at[i]), y, std::max(1, static_cast<int>(X(end) - X(pv_at[i]))), std::max(1, bottom_ - y));
      pv_pts_[i] = {static_cast<lv_value_precise_t>((X(pv_at[i]) + X(end)) / 2), static_cast<lv_value_precise_t>(y)};
    }
    lv_line_set_points(pv_, pv_pts_, k);
    show(pv_, k > 1);
    say(times_[0], tessera::clock_text(from));
    say(times_[1], T("now"));
    say(times_[2], tessera::clock_text(now + SPAN));
    char a[24], b[24];
    if (std::isfinite(used_)) snprintf(a, sizeof(a), "%.1f", used_); else snprintf(a, sizeof(a), "--");
    if (f.n) snprintf(b, sizeof(b), "%.1f", left_kwh); else snprintf(b, sizeof(b), "--");
    snprintf(buf, sizeof(buf), T("day_sum"), a, b);
    say(sum_, buf);
  }

 private:
  PowerOrigin *plugin_;
  lv_obj_t *head_{}, *badge_{}, *grid_{}, *base_{}, *scale_{}, *areas_[AREAS]{}, *pv_{}, *house_{}, *now_{}, *times_[3]{}, *sum_{};
  lv_point_precise_t house_pts_[HISTORY_SLOTS + 1]{}, pv_pts_[AREAS]{}, now_pts_[2]{};
  int left_{8}, span_w_{1}, top_{0}, bottom_{1};
  float home_w_{NAN}, solar_w_{NAN}, grid_w_{NAN}, used_{NAN}, unit_w_{NAN};
  tessera::Text home_;
  uint32_t asked_hour_{0};
};

class FlowCard : public tessera::Card {
 public:
  static constexpr int ROWS = 6;
  FlowCard(PowerOrigin *p, bool grid, bool battery) : plugin_(p), flow_(p), chart_(p), invert_grid_(grid) {
    flow_.set_inversion(grid, battery);
    flow_.seed(p->card_from);
    p->card_from = nullptr;
  }
  void open(const tessera::CardContext &c) override {
    const int lh = ui::line_height(Font::BODY);
    // The four rings are on the tile already: today's hours large (a tap on one names it), the day in figures,
    // then the day chart at the foot.
    const int chart_h = std::clamp(c.height / 6, 48, 100);
    const int chart_block = 3 * lh + chart_h + 10;
    const int hours_h = std::max(1, c.height - ROWS * lh - 8 - chart_block);
    head_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    place(head_, 8, 4, c.width - 16, lh);
    flow_.draw_hours(c.parent, 8, 4 + lh + 4, c.width - 16, hours_h - lh - 12);
    for (int i = 0; i < ROWS; ++i) {
      rows_[i] = ui::label(c.parent, Font::BODY, i ? theme::INK : theme::MUTED);
      place(rows_[i], 8, hours_h + 4 + i * lh, c.width - 16, lh);
    }
    update();
    chart_.build(c.parent, c.entity ? c.entity : "", hours_h + 4 + ROWS * lh, c.width, c.height);
  }
  void on_state(JsonObjectConst data) override {
    flow_.on_state(data);
    const char *names[] = {"import_today", "export_today", "import_price", "export_price", "home_today"};
    for (int i = 0; i < 5; ++i) values_[i] = entity_value(data, names[i], i == 2 || i == 3 ? Unit::PRICE : Unit::ENERGY);
    chart_.on_state(data, invert_grid_);
    update();
  }
  void on_tick(uint32_t epoch) override {
    flow_.on_tick(epoch);
    chart_.tick(epoch);
    plugin_->want_forecast(epoch);
    if (epoch / 60 != shown_) update();
  }
  void on_tap() override {
    const int h = flow_.hour_at(tessera::tap_point());
    picked_ = h == picked_ ? -1 : h;  // the same hour again: back to the heading
    update();
  }
  void on_theme() override {
    flow_.on_theme();
    update();
    chart_.theme();
  }

 private:
  void update() {
    const uint32_t now = tessera::epoch();
    shown_ = now / 60;
    char text[112];
    say(head_, picked_ >= 0 ? flow_.hour_text(picked_) : std::string(T("hours_today")));
    ui::set_color(head_, picked_ >= 0 ? theme::INK : theme::MUTED);
    say(rows_[0], T("today_estimate"));
    for (int i = 0; i < 2; ++i) {
      float cost = estimate(values_[i], values_[i+2]);
      if (std::isfinite(values_[i]) && std::isfinite(cost))
        snprintf(text, sizeof(text), "%s: %.2f kWh  /  %.2f EUR", T(i ? "export" : "import"), values_[i], cost);
      else if (std::isfinite(values_[i]))
        snprintf(text, sizeof(text), "%s: %.2f kWh  /  -- EUR", T(i ? "export" : "import"), values_[i]);
      else snprintf(text, sizeof(text), "%s: -- kWh  /  -- EUR", T(i ? "export" : "import"));
      say(rows_[i+1], text);
    }
    float a = estimate(values_[0], values_[2]), b = estimate(values_[1], values_[3]);
    if (std::isfinite(a) && std::isfinite(b) && std::isfinite(a-b)) snprintf(text, sizeof(text), T("net"), a-b);
    else snprintf(text, sizeof(text), "%s", T("net_none"));
    // The grid's cost of this hour at today's power, as the original's centre mode cost: buying, or earned selling.
    const float per_h = cost_per_hour(flow_.grid_now(), values_[2], values_[3]);
    if (std::isfinite(per_h)) {
      const size_t n = strlen(text);
      snprintf(text + n, sizeof(text) - n, T(per_h >= 0 ? "per_hour_buy" : "per_hour_sell"), std::fabs(per_h));
    }
    say(rows_[3], text);
    const float aut = autarky(values_[0], values_[4]), own = self_use(values_[0], values_[1], values_[4], plugin_->store_in - plugin_->store_out);
    if (std::isfinite(aut) && std::isfinite(own)) snprintf(text, sizeof(text), T("autarky"), aut * 100, own * 100);
    else snprintf(text, sizeof(text), "%s", T("autarky_none"));
    say(rows_[4], text);
    say(rows_[5], best_text(now));
    for (int i = 0; i < ROWS; ++i) ui::set_color(rows_[i], i ? theme::INK : theme::MUTED);
  }
  // When the roof is expected to give 1 kW more than the house takes on average today: the best run of hours,
  // still today, else tomorrow.
  std::string best_text(uint32_t now) const {
    if (now < 86400 || !plugin_->forecast().n) return T("best_none");
    const float base = flow_.house_mean_w() / 1000;
    if (!std::isfinite(base)) return T("best_none");
    const tessera::LocalTime t = tessera::local_time(now);
    const uint32_t midnight = now - (t.hour * 3600 + t.minute * 60 + t.second);
    float pv[24];
    int first, last;
    float peak;
    char buf[96];
    for (int day = 0; day < 2; ++day) {
      forecast_hours(plugin_->forecast(), midnight + day * 86400, pv);
      if (!best_window(pv, base, day ? 0 : t.hour, 1.0f, first, last, peak)) continue;
      snprintf(buf, sizeof(buf), T(day ? "best_tomorrow" : "best_today"), first, last + 1, peak);
      return buf;
    }
    return T("best_nothing");
  }
  PowerOrigin *plugin_;
  Flow flow_;
  DayChart chart_;
  bool invert_grid_;
  int picked_{-1};
  uint32_t shown_{0};
  lv_obj_t *rows_[ROWS]{}, *head_{};
  float values_[5]{NAN, NAN, NAN, NAN, NAN};
};

// The original's Speicher block: the charge big, what the battery does, the cells (one per kWh), energy left.
class Store : public tessera::Tile {
 public:
  explicit Store(PowerOrigin *p) : plugin_(p) {}
  void create(const tessera::TileContext &c) override {
    hold_is_tap(c.parent);
    invert_ = c.options["invert_battery"] | false;
    soc_entity_ = c.options["soc"] | "";
    tile_ = c.tile;
    draw(c.parent, c.width, c.height);
  }
  void draw(lv_obj_t *parent, int w, int h) {
    compact_ = compact_layout(w, h);
    const int lh = ui::line_height(Font::BODY), pad = compact_ ? 8 : 10;
    if (compact_) {
      // The figure, the battery under it, what it does under that: centred as one.
      const int vh = ui::line_height(Font::TITLE), bar = std::clamp(h / 4, 18, 34);
      const int top = std::max(2, (h - vh - bar - lh - 8) / 2);
      soc_ = ui::label(parent, Font::TITLE, theme::INK);
      state_ = ui::label(parent, Font::BODY, theme::MUTED);
      for (auto *l : {soc_, state_}) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      place(soc_, 4, top, w - 8, vh);
      battery(parent, pad, top + vh + 4, w - 2 * pad, bar);
      place(state_, 4, top + vh + bar + 8, w - 8, lh);
      on_tick(0);
      return;
    }
    const Font big = h >= 200 ? Font::VALUE : Font::HEADLINE;
    const int bh = ui::line_height(big);
    const int bar = std::clamp(h / 4, 24, 64);
    const int block = bh + 6 + bar + 6 + lh;  // figure, battery, energy left: centred as one
    const int y0 = std::max(pad, (h - block) / 2);
    soc_ = ui::label(parent, big, theme::INK);
    place(soc_, pad, y0, w / 2 - pad, bh);
    state_ = ui::label(parent, Font::BODY, theme::MUTED);
    power_ = ui::label(parent, Font::TITLE, theme::INK);
    for (auto *l : {state_, power_}) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    const int th = ui::line_height(Font::TITLE);
    place(state_, w / 2, y0 + bh - th - lh, w / 2 - pad, lh);
    place(power_, w / 2, y0 + bh - th, w / 2 - pad, th);
    battery(parent, pad, y0 + bh + 6, w - 2 * pad, bar);
    left_ = ui::label(parent, Font::BODY, theme::MUTED);
    time_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(time_, LV_TEXT_ALIGN_RIGHT, 0);
    place(left_, pad, y0 + bh + 6 + bar + 6, w / 2 - pad, lh);
    place(time_, w / 2, y0 + bh + 6 + bar + 6, w / 2 - pad, lh);
    on_tick(0);
  }
  void on_state(JsonObjectConst data) override {
    soc_v_ = entity_value(data, "soc", Unit::SOC);
    watts_ = entity_value(data, "battery", Unit::POWER);
    if (invert_) watts_ = -watts_;
    capacity_ = entity_value(data, "capacity", Unit::ENERGY);
    reserve_ = entity_value(data, "reserve", Unit::SOC);
    if (!std::isfinite(reserve_)) reserve_ = 0;
    JsonObjectConst sun = data["entities"]["sun"];
    sun_up_ = strcmp(sun["state"] | "", "above_horizon") == 0;
    sunset_ = iso_epoch(sun["attributes"]["next_setting"] | static_cast<const char *>(nullptr));
    mean_.add(watts_, tessera::epoch());
    const float in = entity_value(data, "charge_today", Unit::ENERGY), out = entity_value(data, "discharge_today", Unit::ENERGY);
    if (std::isfinite(in) && std::isfinite(out)) plugin_->store_in = in, plugin_->store_out = out;
    on_tick(0);
  }
  void on_tick(uint32_t) override {
    const Mode mode = battery_mode(watts_, soc_v_);
    const char *const words[] = {"--", T("charging"), T("discharging"), T("full"), T("idle")};
    const char *word = words[static_cast<int>(mode)];
    char buf[40];
    if (std::isfinite(soc_v_)) snprintf(buf, sizeof(buf), "%.0f %%", soc_v_);
    else snprintf(buf, sizeof(buf), "-- %%");
    say(soc_, buf);
    // On the light look the leaf's own colour is too faint for words: ink there, as the flow tile does.
    if (theme::dark) ui::set_tone(soc_, Tone::NORMAL, theme::ENERGY_BATTERY_OUT);
    else ui::set_color(soc_, theme::INK);
    char kw[16];
    kw_text(watts_, kw, sizeof(kw));
    paint_cells(mode);
    if (compact_) {
      if (mode == Mode::CHARGING || mode == Mode::DISCHARGING) snprintf(buf, sizeof(buf), "%s %s kW", mode == Mode::CHARGING ? "+" : "-", kw);
      else snprintf(buf, sizeof(buf), "%s", word);
      say(state_, buf);
      ui::set_color(state_, theme::MUTED);
      return;
    }
    say(state_, word);
    ui::set_color(state_, theme::MUTED);
    snprintf(buf, sizeof(buf), "%s kW", kw);
    say(power_, buf);
    ui::set_color(power_, theme::INK);
    // usableKwh = capacity * (soc - reserve); nothing is shown without a capacity sensor.
    if (std::isfinite(capacity_) && std::isfinite(soc_v_))
      snprintf(buf, sizeof(buf), T("energy_left"), capacity_ * std::max(0.0f, soc_v_ - reserve_) / 100, capacity_);
    else snprintf(buf, sizeof(buf), "-- kWh");
    say(left_, buf);
    ui::set_color(left_, theme::MUTED);
    // battery.runtime: "Lasts until" / "Full at", and fullVerdict's "Not full today" past a day or, by day, past sunset.
    const uint32_t now = tessera::epoch();
    const float hours = hours_left(mode, soc_v_, reserve_, capacity_, mean_.m, mean_.spread());
    buf[0] = 0;
    if (now && std::isfinite(hours)) {
      const uint32_t at = now + static_cast<uint32_t>(hours * 3600);
      const bool full = mode == Mode::CHARGING;
      if (full && (hours > 24 || (sun_up_ && sunset_ > now && at > sunset_))) snprintf(buf, sizeof(buf), "%s", T("not_full"));
      else snprintf(buf, sizeof(buf), T(full ? "full_at" : "lasts_until"), tessera::clock_text(at).c_str());
    }
    say(time_, buf);
    ui::set_color(time_, theme::MUTED);
  }
  void on_theme() override { on_tick(0); }
  void on_tap() override;
  void set_inversion(bool battery) { invert_ = battery; }
  bool live_{false};  // a card's store: the cell under way pulses in its own colour

 private:
  // The original's battery (viewBox 340 x 54): a casing 42 high, stroke 2 at 55 %, rx 6; the pole 9 x 14, 4 off it;
  // the cells inside, 28 of the 42 high, square-ish corners (rx 2), one per kWh.
  void battery(lv_obj_t *parent, int x, int y, int w, int h) {
    const int pole_w = std::max(3, h * 9 / 42), pole_h = std::max(4, h * 14 / 42), pole_gap = std::max(2, h * 4 / 42);
    // The card's store: an arrow beside the pole, into it while charging, out of it while giving.
    const int arrow_w = live_ ? ui::line_height(Font::ICON_SMALL) : 0;
    const int shell_w = std::max(10, w - pole_w - pole_gap - arrow_w);
    if (live_) {
      arrow_ = ui::label(parent, Font::ICON_SMALL, theme::MUTED);
      lv_obj_set_style_text_align(arrow_, LV_TEXT_ALIGN_CENTER, 0);
      place(arrow_, x + shell_w + pole_gap + pole_w, y + (h - arrow_w) / 2, arrow_w, arrow_w);
    }
    shell_ = lv_obj_create(parent);
    lv_obj_remove_style_all(shell_);
    lv_obj_remove_flag(shell_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(shell_, std::max(3, h * 6 / 42), 0);
    lv_obj_set_style_border_width(shell_, 2, 0);
    lv_obj_set_style_border_opa(shell_, LV_OPA_COVER * 55 / 100, 0);
    place(shell_, x, y, shell_w, h);
    pole_ = ui::block(parent, theme::MUTED);
    lv_obj_set_style_radius(pole_, 2, 0);
    put(pole_, LV_STYLE_BG_OPA, LV_OPA_COVER * 55 / 100, 0);  // no layer, as the shell's border
    place(pole_, x + shell_w + pole_gap, y + (h - pole_h) / 2, pole_w, pole_h);
    const int inset = std::max(3, h * 7 / 42);
    cells_x_ = x + std::max(4, h * 5 / 42);
    cells_w_ = shell_w - 2 * (cells_x_ - x);
    cells_y_ = y + inset;
    cells_h_ = std::max(2, h - 2 * inset);
    for (int i = 0; i < MAX_CELLS; ++i) {
      cells_[i] = ui::block(parent, theme::TRACK);
      fills_[i] = ui::block(parent, theme::ENERGY_BATTERY_OUT);
      for (auto *o : {cells_[i], fills_[i]}) lv_obj_set_style_radius(o, 2, 0);
    }
  }
  // segments(): equal cells, the last one partly filled; the tone as the original's: sun while charging, the grid's
  // red down at the reserve, the leaf otherwise.
  void paint_cells(Mode mode) {
    if (!shell_) return;
    lv_obj_set_style_border_color(shell_, ui::color(theme::MUTED), 0);
    ui::set_fill(pole_, Tone::NORMAL, theme::MUTED);
    const Fill fill_tone = cell_tone(mode, soc_v_, reserve_);
    const Paint paint = fill_tone == Fill::SUN ? Paint{Tone::NORMAL, theme::ENERGY_SOLAR}
                      : fill_tone == Fill::LOW ? Paint{Tone::ALERT, theme::ENERGY_GRID}
                                               : Paint{Tone::NORMAL, theme::ENERGY_BATTERY_OUT};
    const int n = segment_count(capacity_);
    const float pitch = static_cast<float>(cells_w_) / n;
    const int cw = std::max(2, static_cast<int>(pitch - std::max(2.0f, cells_h_ * 3.1f / 28)));
    const float exact = std::isfinite(soc_v_) ? std::clamp(soc_v_, 0.0f, 100.0f) / 100.0f * n : 0;
    // The card's live cell (live_): the one being filled or emptied, in the sun's orange while charging and the grid's
    // red while giving to the house, pulsing; the others keep the store's colour.
    const bool moving = live_ && (mode == Mode::CHARGING || mode == Mode::DISCHARGING);
    const int at = moving ? std::clamp(static_cast<int>(std::ceil(exact)) - 1, 0, n - 1) : -1;
    if (pulsing_ != at) {
      if (pulsing_ >= 0) ui::still(fills_[pulsing_]);
      if (at >= 0) ui::animate(fills_[at], tessera::Motion::PULSE, 1600);
      pulsing_ = at;
    }
    const Paint calm = fill_tone == Fill::LOW ? paint : Paint{Tone::NORMAL, theme::ENERGY_BATTERY_OUT};
    const Paint hot = mode == Mode::CHARGING ? Paint{Tone::NORMAL, theme::ENERGY_SOLAR} : Paint{Tone::ALERT, theme::ENERGY_GRID};
    if (arrow_) {
      say(arrow_, moving ? ui::icon(mode == Mode::CHARGING ? 0xF004D : 0xF0054) : "");  // arrow-left in, arrow-right out
      const Paint a = theme::dark || mode != Mode::CHARGING ? hot : Paint{Tone::NORMAL, theme::INK};  // orange too faint on light
      ui::set_tone(arrow_, a.tone, a.role);
    }
    for (int i = 0; i < MAX_CELLS; ++i) {
      show(cells_[i], i < n);
      const float part = std::clamp(exact - i, 0.0f, 1.0f);
      show(fills_[i], i < n && part > 0);
      const int x = cells_x_ + static_cast<int>(i * pitch);
      place(cells_[i], x, cells_y_, cw, cells_h_);
      place(fills_[i], x, cells_y_, std::max(3, static_cast<int>(cw * part + 0.5f)), cells_h_);
      ui::set_fill(cells_[i], Tone::NORMAL, theme::TRACK);
      const Paint p = !live_ ? paint : i == at ? hot : calm;
      ui::set_fill(fills_[i], p.tone, p.role);
      // The emptying cell shows its whole outline faint, so it reads as the one giving.
      if (i == at) show(fills_[i], true), place(fills_[i], x, cells_y_, std::max(3, static_cast<int>(cw * std::max(part, 0.15f) + 0.5f)), cells_h_);
    }
  }
  static constexpr int MAX_CELLS = 20;
  PowerOrigin *plugin_;
  lv_obj_t *soc_{}, *state_{}, *power_{}, *left_{}, *time_{}, *shell_{}, *pole_{}, *cells_[MAX_CELLS]{}, *fills_[MAX_CELLS]{};
  int cells_x_{0}, cells_y_{0}, cells_w_{1}, cells_h_{1}, tile_{-1};
  float soc_v_{NAN}, watts_{NAN}, capacity_{NAN}, reserve_{0};
  Mean mean_;
  uint32_t sunset_{0};
  bool compact_{false}, invert_{false}, sun_up_{false};
  int pulsing_{-1};
  lv_obj_t *arrow_{};
  tessera::Text soc_entity_;
};

// Tap on the battery: the store on top, today's balance, then the 12-hour charge.
class StoreCard : public tessera::Card {
 public:
  StoreCard(PowerOrigin *p, bool battery) : store_(p), chart_(p) { store_.set_inversion(battery); store_.live_ = true; }
  void open(const tessera::CardContext &c) override {
    const int lh = ui::line_height(Font::BODY);
    const int chart_h = std::clamp(c.height / 6, 48, 120);
    const int chart_block = 2 * lh + chart_h + 6;
    const int store_h = std::max(1, c.height - 4 * lh - 12 - chart_block);
    store_.draw(c.parent, c.width, store_h);
    for (int i = 0; i < 4; ++i) {
      rows_[i] = ui::label(c.parent, Font::BODY, i ? theme::INK : theme::MUTED);
      place(rows_[i], 8, store_h + 4 + i * lh, c.width - 16, lh);
    }
    update();
    chart_.build(c.parent, c.entity ? c.entity : "", store_h + 12 + 4 * lh, c.width, c.height);
  }
  void on_state(JsonObjectConst data) override {
    store_.on_state(data);
    charged_ = entity_value(data, "charge_today", Unit::ENERGY);
    given_ = entity_value(data, "discharge_today", Unit::ENERGY);
    capacity_ = entity_value(data, "capacity", Unit::ENERGY);
    price_ = entity_value(data, "import_price", Unit::PRICE);
    chart_.set_unit(data["entities"]["soc"]["attributes"]["unit_of_measurement"] | "");
    update();
    chart_.update();
  }
  void on_tick(uint32_t epoch) override {
    store_.on_tick(epoch);
    chart_.tick(epoch);
  }
  void on_theme() override {
    store_.on_theme();
    for (int i = 0; i < 4; ++i) if (rows_[i]) ui::set_color(rows_[i], i ? theme::INK : theme::MUTED);
    chart_.theme();
  }

 private:
  void update() {
    char text[96];
    auto kwh = [](float v, char *b, size_t n) { if (std::isfinite(v)) snprintf(b, n, "%.2f kWh", v); else snprintf(b, n, "-- kWh"); };
    char a[24], b[24];
    say(rows_[0], T("today"));
    kwh(charged_, a, sizeof(a));
    kwh(given_, b, sizeof(b));
    snprintf(text, sizeof(text), T("charged"), a, b);
    say(rows_[1], text);
    const float c = cycles(given_, capacity_);
    if (std::isfinite(c)) snprintf(text, sizeof(text), T("cycles"), c);
    else snprintf(text, sizeof(text), "%s", T("cycles_none"));
    say(rows_[2], text);
    // "saved": what the energy given today would have cost from the grid at the current price.
    const float saved = estimate(given_, price_);
    if (std::isfinite(saved)) snprintf(text, sizeof(text), T("avoided"), saved);
    else snprintf(text, sizeof(text), "%s", T("avoided_none"));
    say(rows_[3], text);
  }
  Store store_;
  SocChart chart_;
  lv_obj_t *rows_[4]{};
  float charged_{NAN}, given_{NAN}, capacity_{NAN}, price_{NAN};
};

// The original's week: a bar per day for what the roof made, a dot above for how much of the house it carried
// (green from 80 %), today bright, the others faint; the head says the mean of both.
class Days : public tessera::Tile {
 public:
  Days(PowerOrigin *p, bool forecast = false) : plugin_(p), forecast_(forecast) {}
  ~Days() override {
    if (plugin_->week_tile == this) plugin_->week_tile = nullptr, plugin_->week_at = 0;
    if (plugin_->card_from == this) plugin_->card_from = nullptr;
  }
  void create(const tessera::TileContext &c) override {
    hold_is_tap(c.parent);
    const char *names[] = {"solar_today", "home_today", "import_today"};
    tile_ = c.tile;
    if (tile_ >= 0) for (int i = 0; i < 3; ++i) ids_[i] = c.options[names[i]] | "";  // a card's copy has them (seed)
    const int lh = ui::line_height(Font::BODY), pad = 8;
    title_ = ui::label(c.parent, Font::BODY, theme::INK);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_RIGHT, 0);
    narrow_ = c.width < 300;  // 1x1, 1x2: the head keeps its numbers, drops its words
    place(title_, pad, pad, c.width * 2 / 5 - pad, lh);
    place(note_, c.width * 2 / 5, pad, c.width * 3 / 5 - pad, lh);
    left_ = pad;
    slot_ = (c.width - 2 * pad) / 7.0f;
    top_ = pad + lh + 10;
    base_ = c.height - pad - lh - 4;
    for (int k = 0; k < 7; ++k) {
      bars_[k] = ui::block(c.parent, theme::ENERGY_SOLAR);
      dots_[k] = ui::block(c.parent, theme::ENERGY_BATTERY_OUT);
      lv_obj_set_style_radius(bars_[k], 2, 0);
      lv_obj_set_style_radius(dots_[k], LV_RADIUS_CIRCLE, 0);
      days_[k] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(days_[k], LV_TEXT_ALIGN_CENTER, 0);
      place(days_[k], left_ + static_cast<int>(k * slot_), base_ + 4, static_cast<int>(slot_), lh);
      // Room for a figure per day (a card, a tall tile): kWh over the bar, the bars start a line lower.
      if (slot_ >= 34 && c.height >= 200) {
        vals_[k] = ui::label(c.parent, Font::BODY, theme::MUTED);
        lv_obj_set_style_text_align(vals_[k], LV_TEXT_ALIGN_CENTER, 0);
      }
    }
    if (vals_[0]) top_ += lh;
    update();
  }
  void on_state(JsonObjectConst data) override {
    const char *names[] = {"solar_today", "home_today", "import_today"};
    for (int i = 0; i < 3; ++i) {
      JsonObjectConst a = data["entities"][names[i]]["attributes"];
      climbs_[i] = strcmp(a["state_class"] | "", "total_increasing") == 0;
      scale_[i] = value("1", a["unit_of_measurement"] | static_cast<const char *>(nullptr), Unit::ENERGY);
    }
    stated_ = true;
  }
  // A card's copy starts from the tile's meters (seed): their ids and kinds, so it may ask at once.
  void seed(const tessera::Tile *from) {
    if (!from) return;
    const auto *d = static_cast<const Days *>(from);
    for (int i = 0; i < 3; ++i) ids_[i] = d->ids_[i], climbs_[i] = d->climbs_[i], scale_[i] = d->scale_[i];
    stated_ = d->stated_;
  }
  // Once an hour, as the original: the days since local midnight six days ago, by day.
  // ponytail: midnight from the clock, a DST week asks from an hour off; the rows still land on their days.
  void on_tap() override {
    if (tile_ < 0) return;
    plugin_->card_from = this;
    tessera::open_card(plugin_->plugin_id(), forecast_ ? "forecast_week" : "week_detail", "", tile_);
  }
  void on_tick(uint32_t now) override {
    if (forecast_) {
      plugin_->want_forecast(now);
      if (seen_ != plugin_->forecast_version) update();
      return;
    }
    if (seen_ != plugin_->week_version) update();
    if (plugin_->week_tile == this && now - plugin_->week_at > 60) plugin_->week_tile = nullptr, plugin_->week_at = 0;
    if (!now || !stated_ || ids_[0].empty() || plugin_->week_tile || (plugin_->week_at && now - plugin_->week_at < 3600)) return;
    const tessera::LocalTime t = tessera::local_time(now);
    JsonDocument doc;
    doc["ask"] = "recorder/statistics_during_period";
    JsonObject d = doc["data"].to<JsonObject>();
    d["start_time"] = history_iso_utc(now - (t.hour * 3600 + t.minute * 60 + t.second) - 6 * 86400);
    d["end_time"] = history_iso_utc(now);
    JsonArray list = d["statistic_ids"].to<JsonArray>();
    for (auto &id : ids_) if (!id.empty()) list.add(id.c_str());
    d["period"] = "day";
    JsonArray types = d["types"].to<JsonArray>();
    types.add("change");
    types.add("state");
    if (!plugin_->ask(doc.as<JsonObjectConst>())) return;
    plugin_->week_tile = this;
    plugin_->week_at = now;
  }
  void answer(bool ok, JsonObjectConst result) {
    plugin_->week_tile = nullptr;
    const char *const ids[3] = {ids_[0].c_str(), ids_[1].c_str(), ids_[2].c_str()};
    if (!ok || !week_parse(result, ids, climbs_, scale_, plugin_->week)) plugin_->week = Week{};
    ++plugin_->week_version;
    update();
  }
  void on_theme() override { update(); }

 private:
  void update() {
    Week fw;
    if (forecast_) {
      const uint32_t now = tessera::epoch();
      const tessera::LocalTime t = tessera::local_time(now);
      forecast_week(plugin_->forecast(), now - (t.hour * 3600 + t.minute * 60 + t.second), fw);
    }
    const Week &week_ = forecast_ ? fw : plugin_->week;  // one week per screen, whichever tile asked
    seen_ = forecast_ ? plugin_->forecast_version : plugin_->week_version;
    say(title_, T(forecast_ ? "forecast" : "week"));
    ui::set_color(title_, theme::INK);
    float top = 0.1f, sum = 0, share_sum = 0;
    int known = 0, shares = 0;
    float share[7];
    for (int k = 0; k < week_.n; ++k) {
      const float roof = week_.kwh[0][k], house = week_.kwh[1][k], got = week_.kwh[2][k];
      share[k] = std::isfinite(house) && house > 0 && std::isfinite(got) ? std::clamp(1 - got / house, 0.0f, 1.0f) : NAN;
      if (std::isfinite(roof)) { top = std::max(top, roof); sum += roof; ++known; }
      if (std::isfinite(share[k])) { share_sum += share[k]; ++shares; }
    }
    char buf[48] = "";
    const char *kwh = narrow_ ? "" : " kWh";
    if (known && shares) snprintf(buf, sizeof(buf), "\xC3\x98 %.1f%s \xC2\xB7 %.0f %%", sum / known, kwh, 100 * share_sum / shares);
    else if (known) snprintf(buf, sizeof(buf), "\xC3\x98 %.1f%s", sum / known, kwh);
    else if (forecast_ ? !plugin_->forecast().n : ids_[0].empty() && ids_[1].empty() && ids_[2].empty())
      snprintf(buf, sizeof(buf), "%s", T(forecast_ ? "no_forecast" : "no_sensor"));
    say(note_, buf);
    ui::set_color(note_, theme::MUTED);
    // The original's SVG: a bar half a slot wide from the base, (base - 10) high at the week's best, the dot 6 above.
    const int span = std::max(4, base_ - top_ - 8);
    for (int k = 0; k < 7; ++k) {
      const bool has = k < week_.n, today = k == (forecast_ ? 0 : week_.n - 1);
      const float roof = has ? week_.kwh[0][k] : NAN;
      const int x = left_ + static_cast<int>(k * slot_ + slot_ * 0.25f), w = std::max(2, static_cast<int>(slot_ * 0.5f));
      const int h = std::isfinite(roof) ? std::max(1, static_cast<int>(span * roof / top)) : 0;
      show(bars_[k], std::isfinite(roof));
      place(bars_[k], x, base_ - h, w, h);
      ui::set_fill(bars_[k], Tone::NORMAL, theme::ENERGY_SOLAR);
      put(bars_[k], LV_STYLE_BG_OPA, today ? LV_OPA_COVER : LV_OPA_COVER * 45 / 100, 0);
      const bool dot = has && std::isfinite(share[k]);
      show(dots_[k], dot);
      const int lh = ui::line_height(Font::BODY);
      if (vals_[k]) {
        char v[12] = "";
        if (std::isfinite(roof)) snprintf(v, sizeof(v), "%.1f", roof);
        say(vals_[k], v);
        ui::set_color(vals_[k], today ? theme::INK : theme::MUTED);
        place(vals_[k], left_ + static_cast<int>(k * slot_), base_ - h - lh - 2, static_cast<int>(slot_), lh);
      }
      place(dots_[k], x + w / 2 - 3, base_ - h - 12 - (vals_[k] ? lh : 0), 6, 6);
      if (dot) ui::set_fill(dots_[k], share[k] >= 0.8f ? Tone::NORMAL : Tone::ALERT,
                             share[k] >= 0.8f ? theme::ENERGY_BATTERY_OUT : theme::ENERGY_GRID);
      put(dots_[k], LV_STYLE_BG_OPA, today ? LV_OPA_COVER : LV_OPA_COVER * 55 / 100, 0);
      std::string day;
      if (has) day = std::string(T("weekdays")).substr(2 * (tessera::local_time(week_.day[k] + 12 * 3600).weekday - 1), 2);
      say(days_[k], day);
      ui::set_color(days_[k], today ? theme::INK : theme::MUTED);
    }
  }
  PowerOrigin *plugin_;
  bool forecast_, stated_{false};
  int tile_{-1};
  tessera::Text ids_[3];
  bool climbs_[3]{};
  float scale_[3]{1, 1, 1};
  int seen_{-1};
  bool narrow_{false};
  int left_{0}, top_{0}, base_{0};
  float slot_{1};
  lv_obj_t *title_{}, *note_{}, *bars_[7]{}, *dots_[7]{}, *days_[7]{}, *vals_[7]{};
};

// "Where to": the devices you name, the loudest first with a bar for their share of the house, the rest last.
// ponytail: devices are options, not read from the Energy dashboard (its list does not fit the screen's 3.2 KB
// answers); pick top-level devices only, a sub-meter beside its parent counts twice.
class Where : public tessera::Tile {
 public:
  static constexpr int DEVICES = 11, ROWS = 12;
  explicit Where(PowerOrigin *p) : plugin_(p) {}
  ~Where() override { if (plugin_->card_from == this) plugin_->card_from = nullptr; }
  void create(const tessera::TileContext &c) override {
    hold_is_tap(c.parent);
    const int lh = ui::line_height(Font::BODY), pad = 8;
    w_ = c.width;
    tile_ = c.tile;
    title_ = ui::label(c.parent, Font::BODY, theme::INK);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_RIGHT, 0);
    narrow_ = c.width < 300;  // 1x1, 1x2: the head keeps its numbers, drops its words
    place(title_, pad, pad, c.width * 2 / 5 - pad, lh);
    place(note_, c.width * 2 / 5, pad, c.width * 3 / 5 - pad, lh);
    const int pitch = lh + 6;
    rows_ = std::clamp((c.height - pad - lh - 6) / pitch, 1, ROWS);
    for (int i = 0; i < rows_; ++i) {
      const int y = pad + lh + 6 + i * pitch;
      names_[i] = ui::label(c.parent, Font::BODY, theme::INK);
      lv_label_set_long_mode(names_[i], LV_LABEL_LONG_DOT);
      track_[i] = ui::block(c.parent, theme::TRACK);
      fill_[i] = ui::block(c.parent, theme::MUTED);
      watts_[i] = ui::label(c.parent, Font::BODY, theme::INK);
      lv_obj_set_style_text_align(watts_[i], LV_TEXT_ALIGN_RIGHT, 0);
      for (auto *b : {track_[i], fill_[i]}) lv_obj_set_style_radius(b, 2, 0);
      // name | bar | watts; narrow tiles give the name and the watts the room
      bx_ = c.width * (narrow_ ? 45 : 40) / 100 + 4;
      bw_ = c.width * (narrow_ ? 18 : 40) / 100 - 8;
      place(names_[i], pad, y, bx_ - 4 - pad, lh);
      place(track_[i], bx_, y + lh / 2 - 3, bw_, 6);
      place(watts_[i], bx_ + bw_ + 2, y, c.width - bx_ - bw_ - 2 - pad, lh);
      y_[i] = y + lh / 2 - 3;
    }
    update();
  }
  void on_state(JsonObjectConst data) override {
    const uint32_t now = tessera::epoch();
    house_.add(entity_value(data, "home", Unit::POWER), now);
    for (int i = 0; i < DEVICES; ++i) {
      char key[4];
      snprintf(key, sizeof(key), "d%d", i + 1);
      JsonObjectConst e = data["entities"][key];
      dev_[i] = e["name"] | "";
      mean_[i].add(entity_value(data, key, Unit::POWER), now);
    }
    update();
  }
  void on_theme() override { update(); }
  void on_tap() override {
    if (tile_ < 0) return;
    plugin_->card_from = this;
    tessera::open_card(plugin_->plugin_id(), "devices_detail", "", tile_);
  }
  void seed(const tessera::Tile *from) {
    if (!from) return;
    const auto *o = static_cast<const Where *>(from);
    house_ = o->house_;
    for (int i = 0; i < DEVICES; ++i) mean_[i] = o->mean_[i], dev_[i] = o->dev_[i];
  }

 private:
  void update() {
    say(title_, T("where_to"));
    ui::set_color(title_, theme::INK);
    float w[DEVICES];
    for (int i = 0; i < DEVICES; ++i) w[i] = mean_[i].m;
    const float house = house_.m;
    // As many devices as the rows hold, tile or card, the rest last.
    const Ranking r = rank_devices(w, DEVICES, house, rows_ - 1, 0);
    std::string head = narrow_ ? "" : "\xC3\x98 15 min";
    if (std::isfinite(house)) head += (narrow_ ? "" : " \xC2\xB7 ") + power(house);
    bool any = std::isfinite(house);
    for (int i = 0; i < DEVICES; ++i) any |= !dev_[i].empty();
    if (!any) head = T("no_sensor");
    say(note_, head);
    ui::set_color(note_, theme::MUTED);
    // Every share is of the house; without a house reading, of the biggest.
    const float whole = std::isfinite(house) && house > 0 ? house : std::max(1.0f, r.named ? w[r.order[0]] : 1.0f);
    const int bar = bw_;
    for (int i = 0; i < rows_; ++i) {
      const bool named = i < r.named, rest = i == r.named && (std::isfinite(r.rest) || r.reachable > r.named);
      for (auto *o : {names_[i], track_[i], fill_[i], watts_[i]}) show(o, named || rest);
      if (!named && !rest) continue;
      const float v = named ? w[r.order[i]] : r.rest;
      char buf[40];
      if (named) snprintf(buf, sizeof(buf), "%s", device_label(dev_[r.order[i]].c_str()).c_str());
      else if (r.reachable > r.named) snprintf(buf, sizeof(buf), T("rest_n"), r.reachable - r.named);
      else snprintf(buf, sizeof(buf), "%s", T("rest"));
      say(names_[i], buf);
      ui::set_color(names_[i], named ? theme::INK : theme::MUTED);
      say(watts_[i], power(v));
      ui::set_color(watts_[i], named ? theme::INK : theme::MUTED);
      ui::set_fill(track_[i], Tone::NORMAL, theme::TRACK);
      const int fw = std::isfinite(v) ? static_cast<int>(bar * std::min(1.0f, v / whole)) : 0;
      show(fill_[i], fw > 0);
      place(fill_[i], bx_, y_[i], fw, 6);
      ui::set_fill(fill_[i], Tone::NORMAL, named ? theme::INK : theme::MUTED);
    }
  }
  PowerOrigin *plugin_;
  Mean house_, mean_[DEVICES];
  tessera::Text dev_[DEVICES];
  int rows_{1}, w_{1}, bx_{0}, bw_{1}, tile_{-1}, y_[ROWS]{};
  bool narrow_{false};
  lv_obj_t *title_{}, *note_{}, *names_[ROWS]{}, *track_[ROWS]{}, *fill_[ROWS]{}, *watts_[ROWS]{};
};

// The PV forecast of the day (Solcast): today while its sun is still to come, else tomorrow; hourly bars, the hours
// gone solid and the ones to come faint (as the original's produced/expected band). A tap opens Solcast's week.
class Sun : public tessera::Tile {
 public:
  static constexpr int HOURS = 24;
  explicit Sun(PowerOrigin *p) : plugin_(p) {}
  void create(const tessera::TileContext &c) override {
    hold_is_tap(c.parent);
    tile_ = c.tile;
    const int lh = ui::line_height(Font::BODY), pad = 8;
    w_ = c.width - 2 * pad;
    title_ = ui::label(c.parent, Font::BODY, theme::INK);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_RIGHT, 0);
    place(title_, pad, pad, c.width * 3 / 5 - pad, lh);
    place(note_, c.width * 3 / 5, pad, c.width * 2 / 5 - pad, lh);
    top_ = pad + lh + 8;
    base_ = c.height - pad - lh - 4;
    for (auto *&b : bars_) {
      b = ui::block(c.parent, theme::ENERGY_SOLAR);
      lv_obj_set_style_radius(b, 2, 0);
      show(b, false);
    }
    const lv_text_align_t align[] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT};
    for (int i = 0; i < 3; ++i) {
      foot_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(foot_[i], align[i], 0);
      place(foot_[i], pad + i * w_ / 3, base_ + 4, w_ / 3, lh);
    }
    update(tessera::epoch());
  }
  void on_tick(uint32_t now) override {
    plugin_->want_forecast(now);
    if (seen_ != plugin_->forecast_version || now / 600 != shown_) update(now);
  }
  void on_theme() override { update(tessera::epoch()); }
  void on_tap() override {
    if (tile_ >= 0) tessera::open_card(plugin_->plugin_id(), "forecast_week", "", tile_);
  }

 private:
  void update(uint32_t now) {
    seen_ = plugin_->forecast_version;
    shown_ = now / 600;
    const PvForecast &f = plugin_->forecast();
    const tessera::LocalTime t = tessera::local_time(now);
    const uint32_t midnight = now - (t.hour * 3600 + t.minute * 60 + t.second);
    float kw[HOURS];
    auto day = [&](uint32_t from) {  // hourly means; true when some sun is still to come after now
      forecast_hours(f, from, kw);
      bool ahead = false;
      for (int h = 0; h < HOURS; ++h) ahead |= std::isfinite(kw[h]) && kw[h] > 0.01f && from + (h + 1) * 3600 > now;
      return ahead;
    };
    const bool today = day(midnight);
    const uint32_t from = today ? midnight : midnight + 86400;
    if (!today) day(from);
    int first = HOURS, last = -1;
    float top = 0.1f, total = 0, left = 0;
    for (int h = 0; h < HOURS; ++h) {
      if (!std::isfinite(kw[h]) || kw[h] <= 0.01f) continue;
      first = std::min(first, h), last = h;
      top = std::max(top, kw[h]);
      total += kw[h];
      if (from + (h + 1) * 3600 > now) left += kw[h] * std::min(1.0f, (from + (h + 1) * 3600 - now) / 3600.0f);
    }
    say(title_, T(today ? "forecast_today" : "forecast_tomorrow"));
    ui::set_color(title_, theme::INK);
    char buf[32];
    if (last >= 0) snprintf(buf, sizeof(buf), "%.1f kWh", total);
    else snprintf(buf, sizeof(buf), "%s", f.n ? "-- kWh" : T("no_forecast"));
    say(note_, buf);
    ui::set_color(note_, theme::MUTED);
    const int n = last - first + 1, pad = 8;
    const float slot = n > 0 ? static_cast<float>(w_) / n : 1;
    for (int k = 0; k < HOURS; ++k) {
      const int h = first + k;
      show(bars_[k], k < n);
      if (k >= n) continue;
      const float v = std::isfinite(kw[h]) ? kw[h] : 0;
      const int bh = std::max(1, static_cast<int>((base_ - top_) * v / top));
      place(bars_[k], pad + static_cast<int>(k * slot + slot * 0.15f), base_ - bh, std::max(2, static_cast<int>(slot * 0.7f)), bh);
      ui::set_fill(bars_[k], Tone::NORMAL, theme::ENERGY_SOLAR);
      put(bars_[k], LV_STYLE_BG_OPA, from + (h + 1) * 3600 <= now ? LV_OPA_COVER : LV_OPA_COVER * 45 / 100, 0);
    }
    say(foot_[0], n > 0 ? tessera::clock_text(from + first * 3600) : "");
    say(foot_[2], n > 0 ? tessera::clock_text(from + (last + 1) * 3600) : "");
    char rest[32] = "";
    if (today && last >= 0 && w_ >= 280) snprintf(rest, sizeof(rest), T("left_kwh"), left);  // a narrow tile: the hours only
    say(foot_[1], rest);
    for (auto *l : foot_) ui::set_color(l, theme::MUTED);
  }
  PowerOrigin *plugin_;
  int tile_{-1}, w_{1}, top_{0}, base_{1}, seen_{-1};
  uint32_t shown_{0};
  lv_obj_t *title_{}, *note_{}, *bars_[HOURS]{}, *foot_[3]{};
};

// A tile's view the size of a card: Woche, Wohin and the forecast's week open as themselves, on the whole glass.
class Big : public tessera::Card {
 public:
  // The card hears of its tile's data only when it changes: `inner` starts as a copy of the tile's state (seed).
  Big(PowerOrigin *p, tessera::Tile *inner) : plugin_(p), inner_(inner) { plugin_->card_from = nullptr; }
  ~Big() override { delete inner_; }
  void open(const tessera::CardContext &c) override {
    const tessera::TileContext t{c.parent, c.width, c.height, 6, 4, "", "", -1, JsonObjectConst()};
    inner_->create(t);
  }
  void on_state(JsonObjectConst data) override { inner_->on_state(data); }
  void on_tick(uint32_t epoch) override { inner_->on_tick(epoch); }
  void on_theme() override { inner_->on_theme(); }

 private:
  PowerOrigin *plugin_;
  tessera::Tile *inner_;
};

void Store::on_tap() {
  plugin_->card_battery = invert_;
  tessera::open_card(plugin_->plugin_id(), "battery_detail", roomy::plain(soc_entity_), tile_);
}
void Flow::on_tap() {
  // The middle: four rings -> the one ring, whose middle then steps through what it can show (free by day, the
  // house, the cost with prices) as the original's ring.tap cycle, and back to the four rings after the last.
  if (swaps(tessera::tap_point())) {
    const bool producing = std::isfinite(values_[0]) && values_[0] > 150;
    const bool priced = std::isfinite(price_[0]);
    int next = one_ring_ ? centre_ + 1 : 0;
    if (next == 1 && !producing) ++next;  // by night the first view is already the house
    if (next == 2 && !priced) ++next;
    one_ring_ = next <= 2;
    centre_ = one_ring_ ? next : 0;
    on_tick(0);
    return;
  }
  plugin_->card_grid = invert_grid_;
  plugin_->card_battery = invert_battery_;
  plugin_->card_from = this;
  tessera::open_card(plugin_->plugin_id(), "detail", roomy::plain(home_entity_), tile_);
}
void PowerOrigin::setup() {
  self_ = this;
  for (auto &row : hour_mean) for (float &v : row) v = NAN;
  add_tile("flow", [this]() { return new Flow(this); });
  add_card("detail", [this]() { return new FlowCard(this, card_grid, card_battery); });
  add_tile("battery", [this]() { return new Store(this); });
  add_card("battery_detail", [this]() { return new StoreCard(this, card_battery); });
  add_tile("week", [this]() { return new Days(this); });
  add_tile("devices", [this]() { return new Where(this); });
  add_tile("forecast", [this]() { return new Sun(this); });
  add_card("week_detail", [this]() { auto *d = new Days(this); d->seed(card_from); return new Big(this, d); });
  add_card("devices_detail", [this]() { auto *w = new Where(this); w->seed(card_from); return new Big(this, w); });
  add_card("forecast_week", [this]() { return new Big(this, new Days(this, true)); });
}
uint32_t PowerOrigin::ask(JsonObjectConst request) { return week_re_ = tessera::send(this, request); }
uint32_t PowerOrigin::request_history(const std::string &statistic_id, uint32_t now) {
  return history_.request([this](JsonObjectConst request) { return tessera::send(this, request); }, statistic_id, now);
}
// Solcast's forecast from local midnight for seven days, as half-hour kW (manifest `answers`: start, pv); without
// Solcast, the Energy dashboard's forecast (on_message). ponytail: midnight from the clock (a DST night is an hour off).
uint32_t PowerOrigin::request_forecast(uint32_t now) {
  const tessera::LocalTime t = tessera::local_time(now);
  const uint32_t midnight = pv_from_ = now - (t.hour * 3600 + t.minute * 60 + t.second);
  pv_energy_ = false;
  JsonDocument doc;
  doc["ask"] = "call_service:solcast_solar.query_forecast_data";
  JsonObject data = doc["data"].to<JsonObject>();
  data["start_date_time"] = history_iso_utc(midnight);
  data["end_date_time"] = history_iso_utc(midnight + 7 * 86400);
  forecast_re_ = tessera::send(this, doc.as<JsonObjectConst>());
  return forecast_re_;
}
// Today's hours since local midnight, one id per request (three ids' rows would pass the 3.2 KB answer cap), asked
// again when an hour turns; a request that has no answer in a minute starts over.
void PowerOrigin::want_hours(const char *const (&ids)[3], uint32_t now) {
  if (now < 86400 || !*ids[0]) return;
  if (hour_step_ >= 0 ? now - hours_at_ < 60 : hour_floor(now) == hours_hour_) return;
  for (int i = 0; i < 3; ++i) hour_ids_[i] = ids[i];
  const tessera::LocalTime t = tessera::local_time(now);
  hours_from = now - (t.hour * 3600 + t.minute * 60 + t.second);
  hours_hour_ = hour_floor(now);
  for (auto &row : hour_mean) for (float &v : row) v = NAN;
  ask_hour(0, now);
}
void PowerOrigin::ask_hour(int i, uint32_t now) {
  while (i < 3 && hour_ids_[i].empty()) ++i;
  hour_step_ = -1;
  if (i == 3 || hours_hour_ <= hours_from) { ++hours_version; return; }
  JsonDocument doc;
  doc["ask"] = "recorder/statistics_during_period";
  JsonObject d = doc["data"].to<JsonObject>();
  d["start_time"] = history_iso_utc(hours_from);
  d["end_time"] = history_iso_utc(hours_hour_);
  d["statistic_ids"].to<JsonArray>().add(hour_ids_[i]);
  d["period"] = "hour";
  d["types"].to<JsonArray>().add("mean");
  hours_re_ = tessera::send(this, doc.as<JsonObjectConst>());
  if (hours_re_) hour_step_ = i, hours_at_ = now;
}
void PowerOrigin::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  if (re && re == hours_re_ && hour_step_ >= 0) {
    hours_re_ = 0;
    const int i = hour_step_;
    if (message["ok"] | false) hours_parse(message["result"].as<JsonObjectConst>(), hours_from, hour_ids_[i].c_str(), hour_mean[i]);
    ask_hour(i + 1, tessera::epoch());
    tessera::refresh();
    return;
  }
  if (re && re == week_re_) {
    week_re_ = 0;
    if (week_tile) static_cast<Days *>(week_tile)->answer(message["ok"] | false, message["result"].as<JsonObjectConst>());
    return;
  }
  if (re && re == forecast_re_) {
    forecast_re_ = 0;
    const bool ok = message["ok"] | false;
    forecast_ = pv_energy_ ? energy_forecast_parse(ok, message["result"].as<JsonObjectConst>(), pv_from_)
                           : forecast_parse(ok, message["result"].as<JsonObjectConst>());
    if (!forecast_.n && !pv_energy_) {
      pv_energy_ = true;
      JsonDocument doc;
      doc["ask"] = "energy/solar_forecast";
      forecast_re_ = tessera::send(this, doc.as<JsonObjectConst>());
      return;
    }
    ++forecast_version;
    tessera::refresh();
    return;
  }
  if (history_.receive(re, message["ok"] | false, message["result"].as<JsonObjectConst>())) tessera::refresh();
}
}  // namespace esphome::power_origin
