#include "../components/power_origin/history_logic.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace esphome::power_origin;

static const uint32_t HOUR = 3600;
static const uint32_t NOW = 1793700000;  // 2026-11-03 12:00:00 UTC, not on a boundary on purpose
static const uint32_t FROM = hour_floor(NOW) - HISTORY_SLOTS * HOUR;
static const uint32_t TO = hour_floor(NOW);

static JsonDocument doc;

static JsonObjectConst reply(const char *key, const char *rows) {
  doc.clear();
  deserializeJson(doc, std::string("{\"") + key + "\":" + rows + "}");
  return doc.as<JsonObjectConst>();
}

static std::string row(uint32_t start_s, uint32_t end_s, const char *mean, bool ms = true) {
  char buf[160];
  if (mean)
    snprintf(buf, sizeof(buf), "{\"start\":%llu,\"end\":%llu,\"mean\":%s}",
             (unsigned long long) start_s * (ms ? 1000ULL : 1ULL),
             (unsigned long long) end_s * (ms ? 1000ULL : 1ULL), mean);
  else
    snprintf(buf, sizeof(buf), "{\"start\":%llu,\"end\":%llu,\"mean\":null}",
             (unsigned long long) start_s * (ms ? 1000ULL : 1ULL),
             (unsigned long long) end_s * (ms ? 1000ULL : 1ULL));
  return buf;
}

static std::string rows_of(const std::string *items, int n) {
  std::string out = "[";
  for (int i = 0; i < n; ++i) out += (i ? "," : "") + items[i];
  return out + "]";
}

// A mock of the app + Home Assistant for one request: it reads the window out of the request the code built,
// answers it hour by hour from `means` (INT_MIN is an hour Home Assistant has no row for), and returns the
// answer as the app's bounded() would shape it: one key, cut to 32 bytes. The key is deliberately not the
// statistic id, to prove the parser follows the sole key rather than its text.
struct MockHomeAssistant {
  static constexpr int GAP = -1000000;
  int means[HISTORY_SLOTS] = {};
  uint32_t next = 0;
  JsonDocument request;

  uint32_t send(JsonObjectConst r) {
    request.clear();
    request.set(r);
    return ++next;
  }

  static uint32_t iso_epoch(const char *text) {
    std::tm tm{};
    if (!text || !strptime(text, "%Y-%m-%dT%H:%M:%SZ", &tm)) return 0;
    return static_cast<uint32_t>(timegm(&tm));
  }

  std::string answer(bool extra_current_hour = false) const {
    const uint32_t from = iso_epoch(request["data"]["start_time"] | "");
    const uint32_t to = iso_epoch(request["data"]["end_time"] | "");
    std::string body = "{\"sensor.a_truncated_statistic_key\":[";
    bool first = true;
    auto add = [&](uint32_t start, int mean) {
      if (mean == GAP) return;
      if (!first) body += ",";
      first = false;
      char rowbuf[128];
      snprintf(rowbuf, sizeof(rowbuf), "{\"start\":%llu,\"end\":%llu,\"mean\":%d}",
               (unsigned long long) start * 1000ULL, (unsigned long long) (start + HOUR) * 1000ULL, mean);
      body += rowbuf;
    };
    for (int i = 0; i < HISTORY_SLOTS; ++i) add(from + i * HOUR, means[i]);
    if (extra_current_hour) add(to, 777);
    return body + "]}";
  }
};

// With a file of the app's real bounded() output and its window start (from tests/history_bounded_check.py):
// the parser must take all twelve completed hours of it and ignore the current one.
static int parse_file(const char *path, uint32_t from) {
  FILE *f = std::fopen(path, "rb");
  assert(f);
  std::string text;
  char buf[512];
  for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
  std::fclose(f);
  JsonDocument file;
  assert(!deserializeJson(file, text));
  HistoryWindow w;
  assert(history_parse(file.as<JsonObjectConst>(), from, from + HISTORY_SLOTS * HOUR, w));
  for (int i = 0; i < HISTORY_SLOTS; ++i) assert(w.slots[i].known && w.slots[i].mean > 40 && w.slots[i].mean < 90);
  std::printf("history_logic_test: bounded file ok (12/12 hours, last %.2f %%)\n", w.slots[11].mean);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3) return parse_file(argv[1], static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)));
  // Twelve completed hours, key already cut to 32 bytes by the app's bounded(): the key text is irrelevant.
  {
    std::string rows[13];
    for (int i = 0; i < 13; ++i) rows[i] = row(FROM + i * HOUR, FROM + (i + 1) * HOUR, std::to_string(100 + i).c_str());
    HistoryWindow w;
    assert(history_parse(reply("sensor.truncated_battery_power_12", rows_of(rows, 12).c_str()), FROM, TO, w));
    for (int i = 0; i < 12; ++i) assert(w.slots[i].known && w.slots[i].mean == 100 + i);
    // A 13th row at `to` is the current incomplete hour: ignored, not put in slot 11.
    std::string all[13];
    for (int i = 0; i < 13; ++i) all[i] = rows[i];
    HistoryWindow w2;
    assert(history_parse(reply("sensor.truncated_battery_power_12", rows_of(all, 13).c_str()), FROM, TO, w2));
    assert(w2.slots[11].known && w2.slots[11].mean == 111);
  }

  // An empty list is a valid answer with gaps, not a parse failure.
  {
    HistoryWindow w;
    assert(history_parse(reply("sensor.battery_power", "[]"), FROM, TO, w));
    for (int i = 0; i < 12; ++i) assert(!w.slots[i].known);
  }

  // A missing mean is a gap; the other slots remain.
  {
    std::string rows[3] = {row(FROM, FROM + HOUR, "120"), row(FROM + HOUR, FROM + 2 * HOUR, nullptr),
                           row(FROM + 2 * HOUR, FROM + 3 * HOUR, "-80")};
    HistoryWindow w;
    assert(history_parse(reply("sensor.battery_power", rows_of(rows, 3).c_str()), FROM, TO, w));
    assert(w.slots[0].known && w.slots[0].mean == 120);
    assert(!w.slots[1].known);
    assert(w.slots[2].known && w.slots[2].mean == -80);
  }

  // Seconds instead of milliseconds, and an offset row (a half-hour local zone) maps by its hour.
  {
    std::string rows[2] = {row(FROM + 1800, FROM + 5400, "42", false), row(FROM + 5400, FROM + 9000, "43", false)};
    HistoryWindow w;
    assert(history_parse(reply("sensor.battery_power", rows_of(rows, 2).c_str()), FROM, TO, w));
    assert(w.slots[0].known && w.slots[0].mean == 42);
    assert(w.slots[1].known && w.slots[1].mean == 43);
  }

  // Refusals: two statistic ids, malformed times, end before start, duplicate slot, bad mean type.
  {
    HistoryWindow w;
    assert(!history_parse(reply("sensor.battery_power", "[{\"start\":1,\"end\":2,\"mean\":1}]"), FROM, TO, w));
    doc.clear();
    deserializeJson(doc, "{\"a\":[{\"start\":1,\"end\":2,\"mean\":1}],\"b\":[]}");
    assert(!history_parse(doc.as<JsonObjectConst>(), FROM, TO, w));
    std::string bad[1] = {"{\"start\":\"2026-01-01T00:00:00Z\",\"end\":1,\"mean\":1}"};
    assert(!history_parse(reply("sensor.battery_power", rows_of(bad, 1).c_str()), FROM, TO, w));
    std::string backwards[1] = {row(FROM + HOUR, FROM, "1")};
    assert(!history_parse(reply("sensor.battery_power", rows_of(backwards, 1).c_str()), FROM, TO, w));
    std::string duplicate[2] = {row(FROM, FROM + HOUR, "1"), row(FROM, FROM + HOUR, "2")};
    assert(!history_parse(reply("sensor.battery_power", rows_of(duplicate, 2).c_str()), FROM, TO, w));
    char textrow[160];
    snprintf(textrow, sizeof(textrow), "{\"start\":%llu,\"end\":%llu,\"mean\":\"1\"}",
             (unsigned long long) FROM * 1000ULL, (unsigned long long) (FROM + HOUR) * 1000ULL);
    std::string textmean[1] = {textrow};
    assert(!history_parse(reply("sensor.battery_power", rows_of(textmean, 1).c_str()), FROM, TO, w));
  }

  // The request itself: one statistic id, twelve completed hours, and a reply that arrives after a new request.
  {
    JsonDocument seen;
    uint32_t next = 40;
    HistorySession session;
    auto send = [&](JsonObjectConst request) {
      seen.clear();
      seen.set(request);
      return ++next;
    };
    const uint32_t re = session.request(send, "sensor.battery_power", NOW);
    assert(re == 41 && session.pending() && !session.ready());
    assert(std::string(seen["ask"] | "") == "recorder/statistics_during_period");
    assert(std::string(seen["data"]["start_time"] | "") == history_iso_utc(FROM));
    assert(std::string(seen["data"]["end_time"] | "") == history_iso_utc(TO));
    assert(std::string(seen["data"]["period"] | "") == "hour");
    assert(seen["data"]["statistic_ids"].size() == 1);
    assert(std::string(seen["data"]["statistic_ids"][0] | "") == "sensor.battery_power");
    assert(seen["data"]["types"].size() == 1);
    assert(std::string(seen["data"]["types"][0] | "") == "mean");

    std::string rows[12];
    for (int i = 0; i < 12; ++i) rows[i] = row(FROM + i * HOUR, FROM + (i + 1) * HOUR, std::to_string(i).c_str());
    // A stale answer (another id) is ignored, and does not clear the pending one.
    assert(!session.receive(999, true, reply("sensor.battery_power", rows_of(rows, 12).c_str())));
    assert(session.pending());
    assert(session.receive(41, true, reply("sensor.battery_power", rows_of(rows, 12).c_str())));
    assert(session.ready() && session.answered() && !session.pending() && session.window().slots[11].mean == 11);

    // A second request makes the first answer stale even if it has the old id.
    const uint32_t second = session.request(send, "sensor.battery_power", NOW + HOUR);
    assert(second == 42 && session.pending() && !session.ready());
    assert(!session.receive(41, true, reply("sensor.battery_power", rows_of(rows, 12).c_str())));
    // A failed current answer is accepted but leaves no chart.
    assert(session.receive(42, false, JsonObjectConst()));
    assert(!session.ready() && !session.pending());
  }

  // A request-aware mock: the answer is built from the window the code asked for, with one missing hour and
  // one gap value. A reply from an earlier request arrives late and is ignored; the current one fills the slots.
  {
    MockHomeAssistant ha;
    for (int i = 0; i < HISTORY_SLOTS; ++i) ha.means[i] = (i + 1) * 100;
    ha.means[4] = MockHomeAssistant::GAP;  // no row at all
    HistorySession session;
    auto sender = [&](JsonObjectConst request) { return ha.send(request); };
    const uint32_t first = session.request(sender, "sensor.a_statistic_id_longer_than_thirty_two", NOW);
    assert(first == 1 && session.pending());

    // The card closes and opens again before the first answer: a new window, a new id.
    for (int i = 0; i < HISTORY_SLOTS; ++i) ha.means[i] = -(i + 1) * 50;
    ha.means[7] = MockHomeAssistant::GAP;
    const uint32_t second = session.request(sender, "sensor.a_statistic_id_longer_than_thirty_two", NOW + HOUR);
    assert(second == 2 && session.pending() && !session.ready());

    // The first answer lands after the second request: stale, it must not touch the chart.
    JsonDocument late;
    deserializeJson(late, ha.answer());
    assert(!session.receive(first, true, late.as<JsonObjectConst>()));
    assert(session.pending());

    // The second answer, shaped with a 13th current-hour row that must not become slot 11.
    JsonDocument current;
    deserializeJson(current, ha.answer(true));
    assert(session.receive(second, true, current.as<JsonObjectConst>()));
    assert(session.ready() && !session.pending());
    for (int i = 0; i < HISTORY_SLOTS; ++i) {
      if (i == 7) assert(!session.window().slots[i].known);
      else assert(session.window().slots[i].known && session.window().slots[i].mean == -(i + 1) * 50);
    }
    assert(session.window().from == FROM + HOUR);
  }

  // No clock, no statistic id, and a send that fails: none of these send.
  {
    int calls = 0;
    HistorySession session;
    auto send = [&](JsonObjectConst) { ++calls; return 1u; };
    assert(session.request(send, "sensor.battery_power", 0) == 0);
    assert(session.request(send, "", NOW) == 0);
    auto fail = [&](JsonObjectConst) { ++calls; return 0u; };
    assert(session.request(fail, "sensor.battery_power", NOW) == 0);
    assert(calls == 1 && !session.pending());
  }

  // Timeout and card close: both drop the pending request, so its late reply is stale and cannot draw.
  {
    MockHomeAssistant ha;
    for (int i = 0; i < HISTORY_SLOTS; ++i) ha.means[i] = 40 + i;
    HistorySession session;
    auto sender = [&](JsonObjectConst request) { return ha.send(request); };
    const uint32_t first = session.request(sender, "sensor.solarman_battery", NOW);
    assert(!session.expire(NOW + 29, 30) && session.pending());
    assert(session.expire(NOW + 30, 30) && !session.pending() && session.answered() && !session.ready());
    JsonDocument late;
    deserializeJson(late, ha.answer());
    assert(!session.receive(first, true, late.as<JsonObjectConst>()) && !session.ready());

    const uint32_t second = session.request(sender, "sensor.solarman_battery", NOW);
    session.cancel();  // on_cards_closed
    assert(!session.pending() && !session.ready() && !session.answered() && session.window().from == 0);
    assert(!session.receive(second, true, late.as<JsonObjectConst>()) && !session.ready());
    assert(!session.expire(NOW + 999, 30));
  }

  {  // Solcast's mapped answer: a null half hour is a gap, a missing start is no forecast.
    JsonDocument d;
    deserializeJson(d, R"({"start":1793750400,"pv":[0.5,null,2.25]})");
    const PvForecast f = forecast_parse(true, d.as<JsonObjectConst>());
    assert(f.from == 1793750400 && f.n == 3 && f.kw[0] == 0.5f && std::isnan(f.kw[1]) && f.kw[2] == 2.25f);
    assert(forecast_parse(false, d.as<JsonObjectConst>()).n == 0);
    deserializeJson(d, R"({"pv":[1]})");
    assert(forecast_parse(true, d.as<JsonObjectConst>()).n == 0);
    // The Energy dashboard's forecast: hours of two entries summed (zones read), an hour before midnight left out.
    deserializeJson(d, R"({"e1":{"wh_hours":{"2026-11-04T02:00:00+01:00":1500,"2026-11-04T02:00:00+00:00":500,"2026-11-03T23:00:00+00:00":9}},
      "e2":{"wh_hours":{"2026-11-04T02:00:00+01:00":500}}})");
    const PvForecast e = energy_forecast_parse(true, d.as<JsonObjectConst>(), 1793750400);
    assert(e.from == 1793750400 && e.n == 6 && std::isnan(e.kw[0]) && e.kw[2] == 2.0f && e.kw[3] == 2.0f && e.kw[4] == 0.5f);
    assert(energy_forecast_parse(false, d.as<JsonObjectConst>(), 1793750400).n == 0);
    // A day's hours: by the cut key, a row before `from` or without a mean left out.
    deserializeJson(d, R"({"sensor.a_very_long_house_energy_sens":[{"start":1793746800000,"end":1793750400000,"mean":0.4},
      {"start":1793754000000,"end":1793757600000,"mean":1.5},{"start":1793757600000,"end":1793761200000}]})");
    float hrs[24];
    hours_parse(d.as<JsonObjectConst>(), 1793750400, "sensor.a_very_long_house_energy_sensor_power", hrs);
    assert(std::isnan(hrs[0]) && hrs[1] == 1.5f && std::isnan(hrs[2]) && std::isnan(hrs[23]));
    // A week from midnight: day 0 the half hours after it, the slot before midnight left out.
    PvForecast g; g.from = 100000 - 1800; g.n = 50;
    for (int i = 0; i < g.n; ++i) g.kw[i] = 1;
    float hk[24]; forecast_hours(g, 100000, hk);
    assert(hk[0] == 1 && hk[23] == 1 && std::isfinite(hk[23]));
    Week w; forecast_week(g, 100000, w);
    assert(w.n == 2 && w.kwh[0][0] == 24 && w.kwh[0][1] == 0.5f && w.day[1] == 100000 + 86400 && std::isnan(w.kwh[1][0]));
  }

  {  // The week: rows by their cut keys, `change` for a climbing meter, the last `state` for one that resets unsaid.
    JsonDocument d;
    deserializeJson(d, R"({"sensor.daily_pv_generation":[{"start":1793228400000,"end":1793314800000,"change":9.5},
      {"start":1793746800000,"end":1793833200000,"change":2.25}],
      "sensor.a_very_long_house_energy_":[{"start":1793746800000,"end":1793833200000,"change":-30,"state":8}],
      "sensor.other":[{"start":1793660400000,"end":1793746800000,"change":1}]})");
    const char *const ids[3] = {"sensor.daily_pv_generation", "sensor.a_very_long_house_energy_sensor_today", ""};
    const bool climbs[3] = {true, false, true};
    const float scale[3] = {1, 0.001f, 1};
    Week w;
    assert(week_parse(d.as<JsonObjectConst>(), ids, climbs, scale, w));
    assert(w.n == 2 && w.day[0] == 1793228400u && w.day[1] == 1793746800u);  // sensor.other is not ours
    assert(w.kwh[0][0] == 9.5f && w.kwh[0][1] == 2.25f && std::fabs(w.kwh[1][1] - 0.008f) < 1e-6f && std::isnan(w.kwh[1][0]));
    deserializeJson(d, R"({"sensor.daily_pv_generation":[{"end":1}]})");
    assert(!week_parse(d.as<JsonObjectConst>(), ids, climbs, scale, w));
    // Two ids in one answer (the simulator's): the SoC history reads its own.
    deserializeJson(d, R"({"sensor.daily_pv_generation":[],"sensor.soc":[{"start":1793750400000,"end":1793754000000,"mean":50}]})");
    HistoryWindow h;
    assert(history_parse(d.as<JsonObjectConst>(), 1793750400, 1793750400 + 12 * 3600, h, "sensor.soc") && h.slots[0].known);
    assert(!history_parse(d.as<JsonObjectConst>(), 1793750400, 1793750400 + 12 * 3600, h));
  }

  std::puts("history_logic_test: ok");
  return 0;
}
