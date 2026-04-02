# NWS Temperature Conversion and Kalshi Market Settlement

## Overview

This document explains the temperature conversion issues between NWS (National Weather Service) data feeds and how they affect Kalshi weather market settlements. Understanding these nuances is critical for trading temperature markets.

## The Core Problem: Celsius/Fahrenheit Conversion Chain

ASOS (Automated Surface Observation System) stations internally measure and store temperatures in **whole degree Fahrenheit**. However, the data undergoes multiple conversions before being displayed publicly:

### The Conversion Chain

1. **Original Measurement**: Sensor records temperature, stored internally as whole °F
2. **Transmission**: Converted to Celsius for METAR transmission
3. **Rounding**: Rounded to whole °C (precision loss occurs here)
4. **Display**: Converted back to °F for websites like weather.gov

### Example: The "Rounding Mirage"t

| Step | Value | Notes |
|------|-------|-------|
| Sensor reading | 42°F | Original measurement |
| Convert to Celsius | 5.56°C | (42 - 32) × 5/9 |
| Round to whole °C | 6°C | Standard rounding |
| Convert back to °F | 42.8°F | 6 × 9/5 + 32 |
| Display (rounded) | **43°F** | What you see on weather.gov |

**Result**: The timeseries shows 43°F, but the actual sensor reading was 42°F!

## Data Sources and Their Precision

### 1. Hourly METARs (Higher Precision)

- Issued around :51-:54 past each hour
- Temperature rounded to nearest 0.1°F before Celsius conversion
- Often includes **T-group** in remarks (e.g., `T01170100`)
- T-group provides temperature to 0.1°C precision
- **Use T-group values for accurate Fahrenheit conversion when available**

### 2. 5-Minute Observations (Lower Precision)

- More frequent but less precise
- Rounds 1-minute average to whole °F first
- Then converts to whole °C
- **No T-group available** - cannot recover original precision
- Creates larger potential discrepancies
- Iowa Environmental Mesonet stores these as missing due to unreliability

### 3. CLI (Climatological Report) - The Gold Standard

- Official daily summary issued the following morning
- Uses **original sensor data** without extra rounding layers
- Quality-controlled high and low temperatures
- Covers 12:00 AM to 11:59 PM Local Standard Time
- **This is what Kalshi uses for settlement**

### 4. DSM (Daily Summary Message)

- Preliminary summaries issued throughout the day
- Shows highest temperature observed up to issuance time
- **NOT used for final settlement**

## How Kalshi Settles Weather Markets

From the [Kalshi Help Center](https://help.kalshi.com/markets/popular-markets/weather-markets):

> "The only source used for settlement is the NWS Daily Climate Report."

### Key Settlement Rules

1. Markets settle based on the **final NWS Daily Climate Report (CLI)**
2. CLI is typically released the following morning
3. Settlement may be delayed if:
   - High temperature is inconsistent with 6-hr or 24-hr METAR highs
   - Final CLI value differs from preliminary reports
4. Apps like AccuWeather, iOS Weather, or Google Weather **do not determine outcomes**

### Chicago Market Specifics

- Series ticker: `KXHIGHCHI`
- Station: KORD (O'Hare) or KMDW (Midway)
- Market closes: 5:59 AM UTC the following day

## Practical Implications for Trading

### Why Timeseries ≠ Settlement Value

When you see a temperature on [weather.gov timeseries](https://www.weather.gov/wrh/timeseries), understand that:

1. It has gone through the C→F→round→C→round→F conversion chain
2. The displayed value could be 1°F different from the original sensor reading
3. The CLI will report the **original measurement**, not the rounded display value

### Edge Cases Near Degree Boundaries

Temperatures near degree boundaries (like 42/43°F) are particularly affected:

| Actual Temp | After Conversion | Displayed As |
|-------------|------------------|--------------|
| 41°F | 5°C → 41°F | 41°F |
| 42°F | 5.56°C → 6°C → 42.8°F | 43°F |
| 43°F | 6.11°C → 6°C → 42.8°F | 43°F |
| 44°F | 6.67°C → 7°C → 44.6°F | 45°F |

### Trading Strategy Considerations

1. **Don't trust the timeseries display** for exact values
2. **Monitor hourly METARs with T-groups** for more accurate readings
3. **Understand that experienced traders** price in the conversion discrepancy
4. **The CLI is the only thing that matters** for settlement
5. **Settlement can differ from real-time observations** due to quality control

## References

- [Weather Markets | Kalshi Help Center](https://help.kalshi.com/markets/popular-markets/weather-markets)
- [Understanding NWS Data & Reports | wethr.net](https://wethr.net/edu/nws-data-guide)
- [IEM METAR Datasets](https://mesonet.agron.iastate.edu/info/datasets/metar.html)
- [METAR FAQ | NCEI](https://www.ncdc.noaa.gov/wdcmet/data-access-search-viewer-tools/metar-terminal-aerodrome-forecast-taf-faq)
- [NWS Temperature Converter](https://www.weather.gov/epz/wxcalc_tempconvert)
- [NDST Rounding Advice (PDF)](https://www.ncei.noaa.gov/pub/data/uscrn/documentation/program/NDST_Rounding_Advice.pdf)

## Summary

The key takeaway: **what you see on weather.gov timeseries is not necessarily what Kalshi will settle on**. The CLI uses original sensor data without the rounding artifacts introduced by the Celsius conversion chain. Experienced traders understand this and price markets accordingly, which is why you may see apparent "arbitrage" opportunities that aren't actually mispriced.



https://wethr.net/edu/market-bots#bot-guide