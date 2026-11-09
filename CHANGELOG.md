# Changelog

## 0.10.3

- Where to shows on the tile as many devices as its rows hold, as the card does, small ones included; before, the tile
  put every device under 25 W into the rest even with room to spare.

## 0.10.2

- The energy tile can be sized again when it was placed at 1x1: its smallest size is 2x1 (house and battery charge),
  so the right edge makes it 2x1 and the bottom edge 2x2. The editor places a plugin's tile at 1x1 and lets an edge grow
  one direction at a time, which a 2x2 minimum never allowed (reported on a Guition JC8012P4A1, 10.1").

## 0.10.1

- The PV forecast works without Solcast: when Solcast does not answer, the plugin reads the forecast of the Energy
  dashboard (Forecast.Solar, Open-Meteo Solar Forecast or any integration that provides one). That one covers today
  and tomorrow only; in summer the app's answer cap may cut tomorrow's afternoon.
- Week and Where to say "No sensor set" when no sensor is chosen, the forecast "No forecast" when none came.

## 0.10.0

- Smoother: the tiles set only what changed. LVGL drew a part again on every show and style set, so the energy
  tile was drawn whole every second (80-180 ms); faded parts no longer go through a layer (dark look, store pole).
- The one ring as the original's: an outer band with the house's day (sun, store, grid), and a tap on its middle
  steps through free, house and cost per hour before going back to the four rings.
- Self-use is exact when the battery tile knows the store's day: pv = home - import + export + in - out.

## 0.9.4

- The energy tile starts at 2x2: smaller, its four rings and hours do not fit.

## 0.9.3

- A hold now does what a tap does, on every tile of the plugin (LVGL called a 400 ms press a long press).
- Fix: the energy tile still opened only now and then. Its four scaled ring icons each went through a layer of
  their own, so the pressed tile took 440 ms to draw and most taps became a long press. The icons are drawn plain.

## 0.9.2

- Fix: the energy tile did not open. Its split hour bars (72 more parts) made the pressed tile redraw so slowly that
  every tap lasted over 400 ms, a long press. The tile shows one part an hour again; the card keeps the split.

## 0.9.1

- Fix: taps on the energy tile got lost; the hour bars restyled about 100 parts every second and held the screen up
  for 200 ms. Only an hour that changed is drawn again.

## 0.9.0

- Energy card: hour bars split into PV, battery and grid as the original's; a tap on an hour names its figures;
  autarky and self-use today, the grid's cost per hour now, and the best PV-surplus hours from Solcast.
- The energy tile's bars are split the same way.
- Battery card: an arrow at the pole (in while charging, out while giving), the 12-hour bars in the store's colours.

## 0.8.4

- The energy card shows today's hours as large bars instead of the four rings, which the tile already shows.
- The battery card's cell under way pulses: orange while charging, red while giving to the house.

## 0.8.3

- Today's hours as dots: one even row on the axis, coloured by source; no heights.

## 0.8.2

- Today's hours as dots (default), bars or off, in the energy tile's settings; its two sign toggles became one choice.

## 0.8.1

- The four rings show the day's hours beside PV (0-12 left, 12-24 right): each hour in the colour of the source that
  carried most of it, its height the house's mean; the hour under way live.
- Where to's card names every device it has room for, quiet ones too.

## 0.8.0

- Week, Where to and the new PV forecast open large on a tap; Where to names as many devices as its rows hold.
- PV forecast tile (Solcast): today's hours while the sun is still to come, then tomorrow's; its card shows the week.
- The energy tile swaps four rings and the original's one ring on a tap on the middle ring; it starts with four.

## 0.7.2

- Where to names a device without the trailing "Leistung"/"Power" and without a name Home Assistant repeats.

## 0.7.1

- Week and Where to scale like the other tiles: 1x1 up to 6x4 and full; narrow tiles keep the numbers and shorten the head.

## 0.7.0

- New tile "Week" after the original's week: a bar per day for the roof, a dot for the self-supplied share (green from 80 %), today bright, the head with both means; from the recorder's day statistics once an hour.
- New tile "Where to" after the original's devices block: up to 11 named power sensors averaged over 15 min, the five loudest from 25 W with a bar for their share of the house, the rest last.
- The SoC history reads its own id out of an answer that holds several.

## 0.6.0

- Battery: "Lasts until HH:MM" while discharging, "Full at HH:MM" while charging, from the quarter-hour mean as the original's battery.runtime; "Not full today" past a day or, by day, past sunset (new option "Sun", default sun.sun). Nothing when the draw is too jumpy or there is no capacity.

## 0.5.1

- German: the screen's texts follow the screen's language (translations/de.json, `screen`), with a decimal comma; the editor's labels in German too. English stays the fallback.

## 0.5.0

- Energy card: "History & today" after the original's hour view instead of the SoC bars (the battery card keeps those): the house's hourly means for 6 h and live now, Solcast's PV forecast for the next 6 h as line and area, a "now" marker, "PV now", a grid import/export badge, today's use and the PV still expected today.
- New option "House energy today". The forecast comes from Solcast's `query_forecast_data` action.
- Skipped: the original's expected consumption (needs a multi-day profile the screen's 3.2 KB answers cannot carry).

## 0.4.2

- "Reserve" is a sensor now (e.g. the inverter's min SoC), not a number typed in; it reaches the card with the tile's states.

## 0.4.1

- Battery tile: the original's battery casing with pole around the cells, also on the small sizes (1x1 shows figure, battery, power).
- Cell colour as the original: sun while charging, red at or below reserve + 5 %, leaf otherwise. New option "Reserve" (%); energy left counts above it.

## 0.4.0

- New tile "Battery": state of charge, charging/discharging with power, one cell per kWh of capacity (6-20, as the original's segmentCount), energy left from capacity x SoC. Compact 1x1 form.
- Tap opens the battery card: the store, today's charged/discharged energy, cycles (discharged / capacity), grid purchase avoided at the current import price, and the 12-hour SoC history.
- The 12-hour SoC chart is shared by both cards (no behaviour change for the energy flow card).

## 0.3.2
- Redraw the four rings and connectors after the original card's flow geometry: 4-unit tracks and fills, source-coloured bold figures, dotted connectors with filled arrowheads in the flowing participant's colour, MDI sun/pylon/house/battery icons.
- Show power in kW with two decimals below 10 kW, one above (original `formatPower`).

## 0.3.1
- Draw the battery SoC's last twelve completed hours as hourly means (0–100 %) in the detail card, from Home Assistant recorder statistics.
- Request one statistic id asynchronously; accept the sole response key whatever `bounded` did to its text, and refuse a multi-key or malformed answer. Missing hours stay gaps. A late (30 s), stale or after-close reply is ignored; the card asks again when the hour turns.

## 0.3.0
- Show daily grid energy and estimated balance at selectable current prices in the detail card.

## 0.2.2
- Rework the native four-circle flow layout, source colors and separate units; support 2×2 on a 2×4 grid.

## 0.2.1
- Allow smaller tiles, with a compact house power and battery SoC summary; tap for the four-ring detail.

## 0.2.0
- Select seven sensors per tile with searchable entity pickers; reselect them after updating.
- Battery charge requires a percent sensor.

## 0.1.1
- Tap the tile to open a larger energy-flow card.
- Improve layout at larger tile sizes.

## 0.1.0
- Four-node power flow with optional battery state of charge.
