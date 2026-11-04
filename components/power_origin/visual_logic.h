#pragma once
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
namespace esphome::power_origin {
enum class Unit { POWER, ENERGY, SOC, PRICE };
inline bool compact_layout(int width, int height) { return width < 300 || height < 150; }
inline float value(const char *text, const char *unit, Unit kind) {
  if (!text || !unit) return NAN;
  char *end = nullptr;
  float v = strtof(text, &end);
  if (end == text) return NAN;
  while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end || !std::isfinite(v)) return NAN;
  if (kind == Unit::SOC) return !strcmp(unit, "%") && v >= 0 && v <= 100 ? v : NAN;
  if (kind == Unit::PRICE) return (!strcmp(unit, "EUR/kWh") || !strcmp(unit, "€/kWh")) ? v : NAN;
  if (kind == Unit::ENERGY) {
    if (v < 0) return NAN;
    if (!strcmp(unit, "Wh")) return v / 1000;
    return !strcmp(unit, "kWh") ? v : NAN;
  }
  if (!strcmp(unit, "kW")) v *= 1000;
  else if (strcmp(unit, "W")) return NAN;
  return std::isfinite(v) ? v : NAN;
}
inline float estimate(float energy, float price) {
  if (!std::isfinite(energy) || energy < 0 || !std::isfinite(price)) return NAN;
  float result = energy * price;
  return std::isfinite(result) ? result : NAN;
}
// Return -1 for unavailable or invalid SoC, otherwise its angle in degrees.
inline int soc_angle(float soc) {
  return std::isfinite(soc) && soc >= 0 && soc <= 100 ? static_cast<int>(soc * 3.6f) : -1;
}
// Adapted from Power Origin v0.35 computeFlow; see LICENSE.power-origin.
// Do not assign the residual to solar when a required sensor is unknown.
struct Shares { float solar, battery, grid; bool known; };
inline Shares house_shares(float home, float grid, float battery, float solar) {
  if (!std::isfinite(home) || !std::isfinite(grid) || !std::isfinite(battery) || !std::isfinite(solar) || home <= 0)
    return {0, 0, 0, false};
  float g = std::min(home, std::max(0.0f, grid));
  float b = std::min(home-g, std::max(0.0f, -battery));
  return {(home-g-b)/home, b/home, g/home, true};
}
inline bool day_shares(float imported, float exported, float &imp, float &exp) {
  if (!std::isfinite(imported) || !std::isfinite(exported) || imported < 0 || exported < 0) return false;
  float top = std::max({imported, exported, 0.001f});
  imp = imported / top / 2; exp = exported / top / 2; return true;
}
// The flow view's figure (power-origin-card formatPower): always kW, two decimals below 10 kW, one above.
inline void kw_text(float watts, char *buf, size_t n) {
  if (!std::isfinite(watts)) { snprintf(buf, n, "--"); return; }
  float kw = std::fabs(watts) / 1000;
  snprintf(buf, n, kw < 10 ? "%.2f" : "%.1f", kw);
}
// Battery helpers after power-origin-card v0.35 src/battery.ts. Here battery power is + while charging.
// ponytail: one threshold (100 W) without the original's hysteresis; add last-mode memory if the word flickers.
enum class Mode { UNKNOWN, CHARGING, DISCHARGING, FULL, IDLE };
inline Mode battery_mode(float watts, float soc) {
  if (!std::isfinite(soc)) return Mode::UNKNOWN;
  if (std::isfinite(watts) && watts < -100) return Mode::DISCHARGING;
  if (soc >= 99) return Mode::FULL;
  if (std::isfinite(watts) && watts > 100) return Mode::CHARGING;
  return Mode::IDLE;
}
// segmentCount: one cell per kWh of capacity, 6 to 20; 10 when the capacity is unknown.
inline int segment_count(float capacity_kwh) {
  if (!std::isfinite(capacity_kwh) || capacity_kwh <= 0) return 10;
  return std::min(20, std::max(6, static_cast<int>(std::lround(capacity_kwh))));
}
// "cycles": energy given today over the usable capacity.
inline float cycles(float discharged_kwh, float capacity_kwh) {
  if (!std::isfinite(discharged_kwh) || discharged_kwh < 0 || !std::isfinite(capacity_kwh) || capacity_kwh <= 0) return NAN;
  return discharged_kwh / capacity_kwh;
}
// The battery's fill (power-origin-card renderBattery): sun while charging, grid at or below reserve + 5 %, else leaf.
enum class Fill { LEAF, SUN, LOW };
inline Fill cell_tone(Mode mode, float soc, float reserve) {
  if (mode == Mode::CHARGING) return Fill::SUN;
  return std::isfinite(soc) && soc <= reserve + 5 ? Fill::LOW : Fill::LEAF;
}
// The original's averaging window (a quarter hour), time-weighted: each reading counts for as long as it held.
struct Mean {
  float m{NAN}, m2{NAN}, last{NAN};
  uint32_t t{0};
  void add(float x, uint32_t now) {
    if (!now) return;
    if (!std::isfinite(m) && std::isfinite(x)) { m = x; m2 = x * x; }  // the first reading stands until there are more
    if (std::isfinite(last) && now > t) {
      const float a = 1 - std::exp(-static_cast<float>(now - t) / 900);
      m += a * (last - m);
      m2 += a * (last * last - m2);
    }
    last = x;
    t = now;
  }
  float spread() const { return std::fabs(m) > 0 ? std::sqrt(std::max(0.0f, m2 - m * m)) / std::fabs(m) : 0; }
};
// batteryView's hours (+ while charging): to full at the averaged rate, to the reserve at the averaged draw; NAN when
// it cannot be said honestly (no capacity, under 50 W, a draw jumpier than 60 % spread).
// ponytail: the battery's own mean draw stands in for the original's house load; it is what empties the store.
inline float hours_left(Mode mode, float soc, float reserve, float capacity_kwh, float mean_w, float spread) {
  if (!std::isfinite(soc) || !std::isfinite(capacity_kwh) || !std::isfinite(mean_w) || std::fabs(mean_w) < 50) return NAN;
  if (mode == Mode::CHARGING && mean_w > 0) return capacity_kwh * std::max(0.0f, 100 - soc) / 100 / (mean_w / 1000);
  if (mode == Mode::DISCHARGING && mean_w < 0 && spread <= 0.6f)
    return capacity_kwh * std::max(0.0f, soc - reserve) / 100 / (-mean_w / 1000);
  return NAN;
}
// An ISO moment as Home Assistant writes it ("2026-11-04T15:52:31.123+00:00") in seconds since 1970; 0 when not one.
inline uint32_t iso_epoch(const char *s) {
  int y, mo, d, h, mi, sec, oh = 0, om = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 || strlen(s) < 19) return 0;
  const char *z = s + 19;
  while (*z == '.' || (*z >= '0' && *z <= '9')) ++z;
  if (*z == '+' || *z == '-') sscanf(z + 1, "%d:%d", &oh, &om);
  const long off = (*z == '-' ? -1 : 1) * (oh * 3600L + om * 60L);
  y -= mo <= 2;  // days_from_civil
  const long era = y / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  return static_cast<uint32_t>(days * 86400 + h * 3600L + mi * 60L + sec - off);
}
// A power sensor's name as a row: without its trailing "Leistung"/"Power" and without the device name HA puts twice
// ("Keller Rack Keller Rack Leistung" -> "Keller Rack").
inline std::string device_label(std::string s) {
  for (const char *tail : {" Leistung", " Power", " power"})
    if (s.size() > strlen(tail) && s.compare(s.size() - strlen(tail), std::string::npos, tail) == 0) s.resize(s.size() - strlen(tail));
  const size_t half = s.size() / 2;
  if (s.size() % 2 == 1 && s[half] == ' ' && s.compare(0, half, s, half + 1, half) == 0) s.resize(half);
  return s;
}
// rankDevices: the loudest first, at most `limit` named and only from `threshold` W; the rest is the house less those.
struct Ranking {
  int order[16]{};
  int reachable = 0, named = 0;
  float rest = NAN;
};
inline Ranking rank_devices(const float *watts, int n, float house, int limit, float threshold = 25) {
  Ranking r;
  for (int i = 0; i < n && r.reachable < 16; ++i) if (std::isfinite(watts[i])) r.order[r.reachable++] = i;
  std::stable_sort(r.order, r.order + r.reachable, [&](int a, int b) { return watts[a] > watts[b]; });
  float known = 0;
  while (r.named < std::min(limit, r.reachable) && watts[r.order[r.named]] >= threshold) known += watts[r.order[r.named++]];
  if (std::isfinite(house)) r.rest = std::max(0.0f, house - known);
  return r;
}
// hours.ts splitHour: which source carried most of the house's power, grid first, then the store (battery + while
// charging), the sun the rest. 0 sun, 1 store, 2 grid, -1 without a house reading.
inline int dominant_source(float home, float grid, float battery) {
  const Shares s = house_shares(home, std::isfinite(grid) ? grid : 0, std::isfinite(battery) ? battery : 0, 0);
  if (!s.known) return -1;
  return s.grid >= s.solar && s.grid >= s.battery ? 2 : s.battery > s.solar ? 1 : 0;
}
// Today's autarky (how much of the house the own sun and store carried) and self-use (how much of the sun stayed
// home), 0..1, NAN without the meters. With the store's day (in - out, kWh) the sun's day is exact:
// pv = home - import + export + in - out, self-use = (pv - export) / pv; without it the store's net counts as sun.
inline float autarky(float imported, float home) {
  if (!std::isfinite(imported) || !std::isfinite(home) || home <= 0) return NAN;
  return std::clamp(1 - imported / home, 0.0f, 1.0f);
}
inline float self_use(float imported, float exported, float home, float stored = NAN) {
  if (!std::isfinite(imported) || !std::isfinite(exported) || !std::isfinite(home)) return NAN;
  if (std::isfinite(stored)) {
    const float pv = home - imported + exported + stored;
    return pv > 0 ? std::clamp((pv - exported) / pv, 0.0f, 1.0f) : NAN;
  }
  const float own = std::max(0.0f, home - imported);
  return own + exported > 0 ? own / (own + exported) : NAN;
}
// What the grid costs this hour at today's power, EUR/h: positive while buying, negative (earned) while selling.
inline float cost_per_hour(float grid_w, float import_price, float export_price) {
  if (!std::isfinite(grid_w)) return NAN;
  const float price = grid_w >= 0 ? import_price : export_price;
  return std::isfinite(price) ? grid_w / 1000 * price : NAN;
}
// The longest run of hours from `from_hour` on whose PV beats the house's `base` by `margin` kW (a dishwasher, the
// wallbox): [first, last] and its peak surplus; false when none. `margin` is the knob for what counts as worth it.
inline bool best_window(const float (&pv)[24], float base, int from_hour, float margin, int &first, int &last, float &peak) {
  first = last = -1, peak = 0;
  int a = -1;
  for (int h = std::max(0, from_hour); h <= 24; ++h) {
    const bool good = h < 24 && std::isfinite(pv[h]) && std::isfinite(base) && pv[h] - base >= margin;
    if (good && a < 0) a = h;
    if (!good && a >= 0) {
      if (h - a > last - first + 1 || first < 0) first = a, last = h - 1;
      a = -1;
    }
  }
  for (int h = first; h >= 0 && h <= last; ++h) peak = std::max(peak, pv[h] - base);
  return first >= 0;
}
// The 12-hour charge's bars by what the store did since the hour before: 1 up (charging), 2 down (giving), 0 still.
inline int soc_trend(float before, float now) {
  if (!std::isfinite(before) || !std::isfinite(now) || std::fabs(now - before) < 0.5f) return 0;
  return now > before ? 1 : 2;
}
// power-origin-card flow.ts layout(): the parts as shares of one ring, a gap of 1/100 after each when more than one
// is drawn. start/len in turns (0..1); a part of nothing (or NAN) gets len 0. Returns how many are drawn.
inline int ring_parts(const float (&p)[3], float (&start)[3], float (&len)[3]) {
  float total = 0;
  int used = 0;
  for (float v : p) if (std::isfinite(v) && v > 0) total += v, ++used;
  const float gap = used > 1 ? 0.01f : 0, usable = 1 - gap * used;
  float at = 0;
  for (int i = 0; i < 3; ++i) {
    const bool on = std::isfinite(p[i]) && p[i] > 0;
    start[i] = at;
    len[i] = on ? p[i] / total * usable : 0;
    if (on) at += len[i] + gap;
  }
  return used;
}
// The day chart's top: the next whole kW above its largest power, at least 1 kW.
inline float chart_top_w(float max_w) { return std::isfinite(max_w) && max_w > 1000 ? std::ceil(max_w / 1000) * 1000 : 1000; }
}
