#include "../components/power_origin/visual_logic.h"
#include <cassert>
#include <limits>
#include <cstring>
int main() {
  {  using namespace esphome::power_origin;  // rankDevices: 25 W floor, limit, rest = house - named, NAN unreachable
    const float w[] = {300, NAN, 2000, 10, 800};
    Ranking r = rank_devices(w, 5, 3500, 5);
    assert(r.reachable == 4 && r.named == 3 && r.order[0] == 2 && r.order[1] == 4 && r.order[2] == 0 && r.rest == 400);
    r = rank_devices(w, 5, NAN, 1);
    assert(r.named == 1 && std::isnan(r.rest));
    assert(rank_devices(w, 5, 1000, 5).rest == 0);
    assert(dominant_source(1000, 800, 0) == 2 && dominant_source(1000, -50, -700) == 1 && dominant_source(1000, 0, 500) == 0);
    assert(dominant_source(NAN, 0, 0) == -1 && dominant_source(1000, NAN, NAN) == 0);
    assert(std::fabs(autarky(1, 4) - 0.75f) < 1e-6f && std::isnan(autarky(1, 0)) && autarky(5, 4) == 0);
    assert(std::fabs(self_use(1, 1, 4) - 0.75f) < 1e-6f && std::isnan(self_use(4, 0, 4)));
    assert(std::fabs(self_use(2, 3, 10, 4) - 0.8f) < 1e-6f);  // pv = 10 - 2 + 3 + 4 = 15, 12 of it home
    assert(std::fabs(cost_per_hour(2000, 0.3f, 0.07f) - 0.6f) < 1e-6f && cost_per_hour(-1000, 0.3f, 0.07f) < 0);
    {
      float pv[24];
      for (int i = 0; i < 24; ++i) pv[i] = (i >= 10 && i <= 14) || i == 17 ? 3 : 0;
      int a, b; float pk;
      assert(best_window(pv, 0.5f, 0, 1, a, b, pk) && a == 10 && b == 14 && pk == 2.5f);
      assert(best_window(pv, 0.5f, 15, 1, a, b, pk) && a == 17 && b == 17);
      assert(!best_window(pv, 0.5f, 18, 1, a, b, pk));
    }
    assert(soc_trend(40, 45) == 1 && soc_trend(45, 40) == 2 && soc_trend(40, 40.2f) == 0 && soc_trend(NAN, 3) == 0);
    float st[3], ln[3];
    assert(ring_parts({300, NAN, 100}, st, ln) == 2 && ln[1] == 0 && std::fabs(ln[0] - 0.735f) < 1e-4f && std::fabs(st[2] - 0.745f) < 1e-4f);
    assert(ring_parts({500, 0, 0}, st, ln) == 1 && ln[0] == 1 && ring_parts({0, 0, 0}, st, ln) == 0);
    assert(device_label("Keller Rack Keller Rack Leistung") == "Keller Rack" && device_label("Ladeleistung") == "Ladeleistung");
    assert(device_label("Büro-Klima Leistung") == "Büro-Klima" && device_label("Fridge power") == "Fridge");
  }
  {  using namespace esphome::power_origin;  // runtime: 14.21 kWh at 50 %, reserve 15 %, 1 kW out -> 4.97 h; to full at 2 kW -> 3.55 h; jumpy or idle -> none.
    assert(std::fabs(hours_left(Mode::DISCHARGING, 50, 15, 14.21f, -1000, 0.1f) - 4.9735f) < 1e-3f);
    assert(std::fabs(hours_left(Mode::CHARGING, 50, 15, 14.21f, 2000, 0) - 3.5525f) < 1e-3f);
    assert(std::isnan(hours_left(Mode::DISCHARGING, 50, 15, 14.21f, -1000, 0.9f)));
    assert(std::isnan(hours_left(Mode::IDLE, 50, 15, 14.21f, 20, 0)));
    assert(std::isnan(hours_left(Mode::DISCHARGING, 50, 15, NAN, -1000, 0)));
    Mean m;  // 1 kW for 15 min after 0 W: 1 - 1/e of the way.
    m.add(0, 1000); m.add(1000, 1001); m.add(1000, 1901);
    assert(std::fabs(m.m - 632.1f) < 1);
    assert(iso_epoch("2026-11-04T15:52:31.123+00:00") == 1793807551u);
    assert(iso_epoch("2026-11-04T16:52:31+01:00") == 1793807551u);
    assert(iso_epoch("1970-01-01T00:00:00Z") == 0 && iso_epoch("soon") == 0 && iso_epoch(nullptr) == 0);
  }
  using esphome::power_origin::soc_angle;
  using namespace esphome::power_origin;
  assert(compact_layout(218, 118));
  assert(compact_layout(448, 118));
  assert(compact_layout(218, 248));
  assert(!compact_layout(448, 248));
  assert(!compact_layout(448, 183));
  assert(compact_layout(448, 118));
  assert(value("14.2", "%", Unit::SOC) == 14.2f);
  assert(std::isnan(value("14.2", "kWh", Unit::SOC)));
  assert(std::isnan(value("14.2", nullptr, Unit::SOC)));
  assert(std::isnan(value("unknown", "%", Unit::SOC)));
  assert(std::isnan(value("101", "%", Unit::SOC)));
  assert(value("1.5", "kW", Unit::POWER) == 1500);
  assert(value("1500", "W", Unit::POWER) == 1500);
  assert(value("1500", "Wh", Unit::ENERGY) == 1.5f);
  assert(value("1.5", "kWh", Unit::ENERGY) == 1.5f);
  assert(std::isnan(value("-1", "kWh", Unit::ENERGY)));
  assert(std::isnan(value("2x", "W", Unit::POWER)));
  assert(value("0.32", "EUR/kWh", Unit::PRICE) == 0.32f);
  assert(std::isnan(value("0.32", "ct/kWh", Unit::PRICE)));
  assert(std::isnan(estimate(NAN, 0.32f)));
  assert(estimate(2, -0.1f) < 0);
  assert(estimate(2, 0.1f) - estimate(5, 0.2f) < 0);
  auto a = house_shares(1000, 0, -1000, 0); assert(a.known && a.battery == 1 && a.grid == 0);
  a = house_shares(1000, 400, 0, 600); assert(a.grid == 0.4f && a.solar == 0.6f);
  a = house_shares(1000, -200, -300, 1000); assert(a.grid == 0 && a.battery == 0.3f);
  a = house_shares(1000, NAN, -300, 0); assert(!a.known);
  a = house_shares(0, 0, 0, 0); assert(!a.known);
  assert(!house_shares(1000, 0, 0, NAN).known);
  float imp, exp; assert(day_shares(4, 2, imp, exp) && imp == 0.5f && exp == 0.25f);
  assert(!day_shares(NAN, 2, imp, exp));
  assert(soc_angle(0) == 0);
  assert(soc_angle(50) == 180);
  assert(soc_angle(100) == 360);
  assert(soc_angle(-1) == -1);
  assert(soc_angle(101) == -1);
  assert(soc_angle(std::numeric_limits<float>::quiet_NaN()) == -1);
  assert(soc_angle(std::numeric_limits<float>::infinity()) == -1);
  char kw[16];
  kw_text(1400, kw, sizeof(kw)); assert(!strcmp(kw, "1.40"));
  kw_text(-90, kw, sizeof(kw)); assert(!strcmp(kw, "0.09"));
  kw_text(12345, kw, sizeof(kw)); assert(!strcmp(kw, "12.3"));
  kw_text(NAN, kw, sizeof(kw)); assert(!strcmp(kw, "--"));
  assert(battery_mode(-487, 21) == Mode::DISCHARGING);
  assert(battery_mode(500, 50) == Mode::CHARGING);
  assert(battery_mode(500, 99.5f) == Mode::FULL);
  assert(battery_mode(-500, 100) == Mode::DISCHARGING);
  assert(battery_mode(40, 50) == Mode::IDLE);
  assert(battery_mode(NAN, 50) == Mode::IDLE);
  assert(battery_mode(0, NAN) == Mode::UNKNOWN);
  assert(segment_count(14.21f) == 14 && segment_count(3) == 6 && segment_count(40) == 20 && segment_count(NAN) == 10);
  assert(std::fabs(cycles(12.253f, 14.21f) - 0.8623f) < 0.001f);
  assert(std::isnan(cycles(5, 0)) && std::isnan(cycles(NAN, 14)));
  assert(cell_tone(Mode::CHARGING, 10, 15) == Fill::SUN);
  assert(cell_tone(Mode::DISCHARGING, 15, 15) == Fill::LOW && cell_tone(Mode::IDLE, 20, 15) == Fill::LOW);
  assert(cell_tone(Mode::IDLE, 21, 15) == Fill::LEAF && cell_tone(Mode::IDLE, 5, 0) == Fill::LOW);
  assert(cell_tone(Mode::UNKNOWN, NAN, 0) == Fill::LEAF);
  assert(chart_top_w(0) == 1000 && chart_top_w(NAN) == 1000 && chart_top_w(1000) == 1000 && chart_top_w(1001) == 2000);
}
