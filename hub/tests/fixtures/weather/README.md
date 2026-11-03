# Open-Meteo offline recordings

These public Berlin responses were recorded on 2026-10-28. Tests freeze their
clock at the recorded `current.time`; no test contacts Open-Meteo.

- `berlin_geocode.json`: `https://geocoding-api.open-meteo.com/v1/search?name=Berlin&count=1&language=en&format=json`
- `berlin_forecast_c.json` and `berlin_forecast_f.json`: `https://api.open-meteo.com/v1/forecast`
  with `latitude=52.52`, `longitude=13.41`,
  `current=temperature_2m,weather_code,is_day`,
  `hourly=temperature_2m,weather_code,is_day`,
  `daily=temperature_2m_max,temperature_2m_min`, `timezone=auto`,
  `timeformat=unixtime`, `forecast_days=2`, and
  `temperature_unit=celsius` or `fahrenheit` respectively.

Source and attribution: [Open-Meteo](https://open-meteo.com/),
[forecast API documentation](https://open-meteo.com/en/docs),
[geocoding documentation](https://open-meteo.com/en/docs/geocoding-api).
Open-Meteo weather data is offered under CC BY 4.0.
