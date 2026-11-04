#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "history_logic.h"

namespace esphome::power_origin {
class PowerOrigin : public Component, public tessera::Plugin {
 public:
  void setup() override;
  void on_message(JsonObjectConst message) override;
  void on_cards_closed() override { history_.cancel(); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  bool card_grid{false}, card_battery{false};
  // Ask for the twelve completed hours of the SoC statistic. The answer comes to on_message.
  uint32_t request_history(const std::string &statistic_id, uint32_t now);
  void expire_history(uint32_t now) { if (history_.expire(now, 30)) tessera::refresh(); }
  bool history_pending() const { return history_.pending(); }
  bool history_ready() const { return history_.ready(); }
  bool history_answered() const { return history_.answered(); }
  const HistoryWindow &history() const { return history_.window(); }
  // Solcast's week from local midnight, asked again when a half hour turns; whoever shows it calls this each tick.
  void want_forecast(uint32_t now) { if (now >= 86400 && now / 1800 != pv_asked_) pv_asked_ = now / 1800, request_forecast(now); }
  const PvForecast &forecast() const { return forecast_; }
  int forecast_version{0};
  tessera::Tile *card_from{nullptr};
  float store_in{NAN}, store_out{NAN};  // the store's kWh in and out today, from the battery tile (exact self-use)
  // The four rings' hour strip: house, grid and store as today's hourly means (sensor units), NAN where none.
  void want_hours(const char *const (&ids)[3], uint32_t now);
  float hour_mean[3][24];
  uint32_t hours_from{0};
  int hours_version{0};  // the tile whose tap opens a Big card now; the card starts from its state
  // The week tile's one request: its answer goes to the tile that asked, while it lives.
  uint32_t ask(JsonObjectConst request);
  tessera::Tile *week_tile{nullptr};  // asking now
  Week week;
  uint32_t week_at{0};
  int week_version{0};

 private:
  HistorySession history_;
  PvForecast forecast_;
  uint32_t request_forecast(uint32_t now);
  void ask_hour(int i, uint32_t now);
  std::string hour_ids_[3];
  int hour_step_{-1};
  uint32_t hours_hour_{0}, hours_at_{0}, hours_re_{0};
  uint32_t forecast_re_{0}, week_re_{0}, pv_asked_{0}, pv_from_{0};
  bool pv_energy_{false};  // the forecast asked is the Energy dashboard's, after Solcast gave none
};
}  // namespace esphome::power_origin
