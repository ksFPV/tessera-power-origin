#pragma once
// The battery SoC's last twelve completed hours as hourly means, from Home Assistant's recorder statistics.
//
// The app asks exactly one statistic id per request. Home Assistant answers a dict keyed by that id, and the
// app's `bounded` (3.2 KB) cuts every dict key to 32 bytes. A sensor id is often longer, so the key's text is
// useless; there is exactly one key, and its value is the rows. Two keys is a malformed answer: refuse it.
//
// Rows carry `start` and `end` (milliseconds since 1970 since HA 2023.9, seconds before; Tessera's own server.py
// reads them the same way) and `mean`. An ISO text time is refused, never guessed. A missing mean is
// a real gap, not a zero. A malformed row refuses the whole answer, so the chart shows nothing rather than a
// plausible-looking lie.
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>

namespace esphome::power_origin {

constexpr int HISTORY_SLOTS = 12;

struct HistorySlot {
  uint32_t start = 0;  // epoch seconds of the hour's start
  float mean = 0.0f;   // SoC in %, the hour's mean
  bool known = false;
};

struct HistoryWindow {
  uint32_t from = 0;  // start of the first slot, epoch seconds
  HistorySlot slots[HISTORY_SLOTS];
};

inline uint32_t hour_floor(uint32_t t) { return t - t % 3600; }

// ISO-8601 in UTC, for Home Assistant's start_time and end_time.
inline std::string history_iso_utc(uint32_t t) {
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char buf[24];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// A statistics timestamp in seconds: Home Assistant writes milliseconds, older versions seconds. Reject
// anything that is not a plausible date (2020 to 2100) so a missing field cannot become 1970.
inline bool history_epoch(JsonVariantConst value, uint32_t &out) {
  if (!value.is<double>()) return false;
  const double raw = value.as<double>();
  if (!std::isfinite(raw) || raw <= 0) return false;
  const double seconds = raw > 1e11 ? raw / 1000.0 : raw;
  if (seconds < 1600000000.0 || seconds > 4102444800.0) return false;
  out = static_cast<uint32_t>(seconds);
  return true;
}

// The answer must hold exactly one statistic id, and its value must be a list of rows. `from` and `to` are the
// query's bounds: twelve completed hours, from an hour boundary to the next. A row outside them (the current,
// incomplete hour at `to`) is ignored. Two rows in one slot refuse the answer.
// `key`: the asked id as the app cuts it (32 bytes); an answer of several ids (the week's, in the simulator) is read by
// it, an answer of one by its only member.
inline bool history_parse(JsonObjectConst result, uint32_t from, uint32_t to, HistoryWindow &out, const char *key = nullptr) {
  out = HistoryWindow{};
  out.from = from;
  if (from % 3600 != 0 || to != from + HISTORY_SLOTS * 3600) return false;
  if (result.isNull()) return false;
  JsonVariantConst rows = key && *key ? result[key] : JsonVariantConst();
  if (rows.isNull()) {
    auto it = result.begin();
    if (it == result.end()) return false;
    rows = it->value();
    ++it;
    if (it != result.end()) return false;  // more than the one statistic id we asked for
  }
  if (!rows.is<JsonArrayConst>()) return false;
  bool seen[HISTORY_SLOTS] = {};
  for (JsonVariantConst element : rows.as<JsonArrayConst>()) {
    if (!element.is<JsonObjectConst>()) return false;
    JsonObjectConst row = element.as<JsonObjectConst>();
    uint32_t start = 0, end = 0;
    if (!history_epoch(row["start"], start) || !history_epoch(row["end"], end)) return false;
    if (end <= start || end - start > 2 * 3600) return false;
    if (start < from || start >= to) continue;
    const uint32_t slot = (start - from) / 3600;
    if (slot >= HISTORY_SLOTS) continue;
    if (seen[slot]) return false;
    seen[slot] = true;
    out.slots[slot].start = from + slot * 3600;
    JsonVariantConst mean_value = row["mean"];
    if (mean_value.isNull()) continue;  // a gap inside a valid row
    if (!mean_value.is<double>()) return false;
    const double mean = mean_value.as<double>();
    if (!std::isfinite(mean)) return false;
    out.slots[slot].mean = static_cast<float>(mean);
    out.slots[slot].known = true;
  }
  return true;
}

// A day's hourly means of one statistic (power-origin-card hours.ts): slot h is the hour from `from + h * 3600`, NAN
// where no row came. `key`: the asked id, read as the app cuts it (32 bytes); an answer of one id by its only member.
inline void hours_parse(JsonObjectConst result, uint32_t from, const char *key, float (&out)[24]) {
  for (float &v : out) v = NAN;
  char cut[33];
  snprintf(cut, sizeof(cut), "%s", key);
  JsonVariantConst rows = result[cut];
  if (rows.isNull() && result.size() == 1) rows = result.begin()->value();
  for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
    uint32_t s = 0;
    if (!history_epoch(row["start"], s) || s < from || !row["mean"].is<double>()) continue;
    const uint32_t h = (s - from) / 3600;
    if (h < 24) out[h] = row["mean"].as<float>();
  }
}

// Solcast's half hours from the manifest's mapped answer {start: epoch, pv: [kW, ...]}. A missing value is a gap
// (NAN), never a zero; anything malformed leaves no forecast.
struct PvForecast {
  static constexpr int SLOTS = 7 * 48;  // a week of half hours: 336 numbers, ~1.8 KB mapped
  uint32_t from = 0;
  int n = 0;
  float kw[SLOTS]{};
};
// The forecast's hourly kW of the day from `from` (local midnight): the mean of each hour's two halves, NAN where none.
inline void forecast_hours(const PvForecast &f, uint32_t from, float (&kw)[24]) {
  for (int h = 0; h < 24; ++h) {
    float sum = 0;
    int got = 0;
    for (int j = 0; j < 2; ++j) {
      const uint32_t s = from + h * 3600 + j * 1800;
      const int64_t i = (static_cast<int64_t>(s) - f.from) / 1800;
      if (s < f.from || i >= f.n || !std::isfinite(f.kw[i])) continue;
      sum += f.kw[i], ++got;
    }
    kw[h] = got ? sum / got : NAN;
  }
}
inline PvForecast forecast_parse(bool ok, JsonObjectConst result) {
  PvForecast f;
  uint32_t from = 0;
  if (!ok || !history_epoch(result["start"], from) || !result["pv"].is<JsonArrayConst>()) return f;
  f.from = from;
  for (JsonVariantConst v : result["pv"].as<JsonArrayConst>()) {
    if (f.n == PvForecast::SLOTS) break;
    f.kw[f.n++] = v.is<double>() && std::isfinite(v.as<double>()) ? v.as<float>() : NAN;
  }
  return f;
}

// The week (power-origin-card _fetchWeek): seven days of roof, house and import, from one answer of period "day" for up
// to three ids, read by their keys as the app cuts them (32 bytes). dayTotal: a meter that only climbs gives its day as
// `change`, one that resets unsaid at midnight as the day's last `state`. A day without a row stays NAN.
struct Week {
  int n = 0;
  uint32_t day[7]{};
  float kwh[3][7]{};  // roof, house, import
};
// The forecast's days from `midnight` on as a Week of roof kWh (the half hours' kW / 2), the others NAN.
// ponytail: days of 86400 s, a DST change puts an hour on the wrong day.
inline void forecast_week(const PvForecast &f, uint32_t midnight, Week &out) {
  out = Week{};
  for (auto &row : out.kwh) for (float &v : row) v = NAN;
  for (int i = 0; i < f.n; ++i) {
    const uint32_t s = f.from + i * 1800;
    if (s < midnight || !std::isfinite(f.kw[i])) continue;
    const int k = static_cast<int>((s - midnight) / 86400);
    if (k >= 7) break;
    for (; out.n <= k; ++out.n) out.day[out.n] = midnight + out.n * 86400, out.kwh[0][out.n] = 0;
    out.kwh[0][k] += f.kw[i] / 2;
  }
}
inline bool week_parse(JsonObjectConst result, const char *const (&ids)[3], const bool (&climbs)[3], const float (&scale)[3],
                       Week &out) {
  out = Week{};
  for (auto &row : out.kwh) for (float &v : row) v = NAN;
  if (result.isNull()) return false;
  uint32_t starts[16];
  int ns = 0;
  auto which = [&](const char *key) {
    for (int i = 0; i < 3; ++i)
      if (ids[i] && *ids[i] && strncmp(ids[i], key, 32) == 0 && strlen(key) == std::min<size_t>(strlen(ids[i]), 32)) return i;
    return -1;
  };
  for (JsonPairConst kv : result) {
    if (which(kv.key().c_str()) < 0) continue;
    if (!kv.value().is<JsonArrayConst>()) return false;
    for (JsonVariantConst row : kv.value().as<JsonArrayConst>()) {
      uint32_t s = 0;
      if (!history_epoch(row["start"], s)) return false;
      if (std::find(starts, starts + ns, s) == starts + ns && ns < 16) starts[ns++] = s;
    }
  }
  std::sort(starts, starts + ns);
  out.n = std::min(7, ns);
  for (int k = 0; k < out.n; ++k) out.day[k] = starts[ns - out.n + k];
  for (JsonPairConst kv : result) {
    const int i = which(kv.key().c_str());
    if (i < 0) continue;
    for (JsonVariantConst row : kv.value().as<JsonArrayConst>()) {
      uint32_t s = 0;
      history_epoch(row["start"], s);
      const int k = std::find(out.day, out.day + out.n, s) - out.day;
      if (k >= out.n) continue;
      const float change = row["change"].is<double>() ? row["change"].as<float>() : NAN;
      const float state = row["state"].is<double>() ? row["state"].as<float>() : NAN;
      const float v = climbs[i] || !std::isfinite(state) ? change : std::max(0.0f, state);
      if (std::isfinite(v)) out.kwh[i][k] = v * scale[i];
    }
  }
  return true;
}

// One request and its one answer. `re` is the app's request id; an answer to an older request is ignored, as
// is an answer that arrives after the card asked again. The window is fixed when the request goes out, so a
// reply that crosses an hour boundary cannot shift every slot.
class HistorySession {
 public:
  using Sender = std::function<uint32_t(JsonObjectConst)>;

  // Ask for the twelve completed hours before `now`. Returns the request id, 0 when it was not sent.
  uint32_t request(const Sender &send, const std::string &statistic_id, uint32_t now) {
    if (statistic_id.empty() || now < 13 * 3600) return 0;
    const uint32_t to = hour_floor(now);
    const uint32_t from = to - HISTORY_SLOTS * 3600;
    window_ = HistoryWindow{};
    window_.from = from;
    snprintf(key_, sizeof(key_), "%s", statistic_id.c_str());
    ready_ = false;
    answered_ = false;
    pending_ = false;
    expected_ = 0;
    JsonDocument doc;
    doc["ask"] = "recorder/statistics_during_period";
    JsonObject data = doc["data"].to<JsonObject>();
    data["start_time"] = history_iso_utc(from);
    data["end_time"] = history_iso_utc(to);
    data["statistic_ids"].to<JsonArray>().add(statistic_id);
    data["period"] = "hour";
    data["types"].to<JsonArray>().add("mean");
    const uint32_t re = send(doc.as<JsonObjectConst>());
    if (re == 0) return 0;
    expected_ = re;
    sent_ = now;
    pending_ = true;
    return re;
  }

  // True when `re` was this session's current request and the answer was dealt with (good or bad). False for
  // a stale or unknown answer, which must not touch the chart.
  bool receive(uint32_t re, bool ok, JsonObjectConst result) {
    if (!pending_ || expected_ == 0 || re != expected_) return false;
    pending_ = false;
    expected_ = 0;
    answered_ = true;
    if (!ok) {
      ready_ = false;
      return true;
    }
    HistoryWindow parsed;
    if (!history_parse(result, window_.from, window_.from + HISTORY_SLOTS * 3600, parsed, key_)) {
      ready_ = false;
      return true;
    }
    window_ = parsed;
    ready_ = true;
    return true;
  }

  // No answer `timeout` seconds after the request: give up, as a failed answer. A late reply is then stale.
  bool expire(uint32_t now, uint32_t timeout) {
    if (!pending_ || now < sent_ + timeout) return false;
    pending_ = false;
    expected_ = 0;
    answered_ = true;
    ready_ = false;
    return true;
  }

  // The card closed: forget the window, and any reply still on its way.
  void cancel() { *this = HistorySession{}; }

  bool pending() const { return pending_; }
  bool ready() const { return ready_; }
  bool answered() const { return answered_; }
  const HistoryWindow &window() const { return window_; }

 private:
  HistoryWindow window_{};
  char key_[33]{};
  uint32_t expected_ = 0, sent_ = 0;
  bool pending_ = false;
  bool ready_ = false;
  bool answered_ = false;
};

}  // namespace esphome::power_origin
