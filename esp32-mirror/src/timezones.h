#pragma once

// Time zones offered on the phone setup page. The page pre-selects the phone's
// own zone (the browser knows it as an IANA name): an exact name, one of the
// aliases, or else the first zone with the same UTC offset right now.
// posix = the TZ rule newlib needs (offset + daylight-saving switch dates).
// Checked against the IANA database by esp32-mirror/ota/check_timezones.py.
struct TzZone {
  const char *label;     // shown in the list
  const char *city;      // shown on the Settings tile
  const char *iana;      // stored on the device
  const char *posix;
  const char *aliases;   // other IANA names that share the rule (space-separated)
};

static const TzZone TZ_ZONES[] = {
  {"UTC", "UTC", "Etc/UTC", "UTC0", "UTC Etc/GMT GMT Etc/Universal"},
  // Americas
  {"Hawaii", "Honolulu", "Pacific/Honolulu", "HST10", ""},
  {"Alaska", "Anchorage", "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0",
   "America/Juneau America/Sitka America/Nome America/Yakutat"},
  {"US & Canada Pacific (Los Angeles, Vancouver)", "Los Angeles", "America/Los_Angeles",
   "PST8PDT,M3.2.0,M11.1.0", "America/Vancouver America/Tijuana"},
  {"Arizona (Phoenix)", "Phoenix", "America/Phoenix", "MST7", "America/Hermosillo"},
  {"US & Canada Mountain (Denver)", "Denver", "America/Denver", "MST7MDT,M3.2.0,M11.1.0",
   "America/Edmonton America/Boise"},
  {"US & Canada Central (Chicago)", "Chicago", "America/Chicago", "CST6CDT,M3.2.0,M11.1.0",
   "America/Winnipeg America/Indiana/Knox America/Menominee"},
  {"Mexico City", "Mexico City", "America/Mexico_City", "CST6",
   "America/Monterrey America/Merida America/Guatemala America/Costa_Rica"},
  {"US & Canada Eastern (New York, Toronto)", "New York", "America/New_York",
   "EST5EDT,M3.2.0,M11.1.0",
   "America/Toronto America/Detroit America/Indiana/Indianapolis "
   "America/Kentucky/Louisville America/Montreal America/Nassau"},
  {"Bogota, Lima", "Bogota", "America/Bogota", "<-05>5",
   "America/Lima America/Guayaquil America/Panama America/Cancun"},
  {"Atlantic (Halifax)", "Halifax", "America/Halifax", "AST4ADT,M3.2.0,M11.1.0",
   "America/Moncton Atlantic/Bermuda"},
  {"Caracas, La Paz, Puerto Rico", "Caracas", "America/Caracas", "<-04>4",
   "America/La_Paz America/Manaus America/Puerto_Rico America/Santo_Domingo"},
  {"Santiago", "Santiago", "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24", ""},
  {"Sao Paulo, Buenos Aires", "Sao Paulo", "America/Sao_Paulo", "<-03>3",
   "America/Argentina/Buenos_Aires America/Buenos_Aires America/Montevideo"},
  // Europe, Africa, Middle East
  {"UK, Ireland, Portugal (London)", "London", "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0",
   "Europe/Dublin Europe/Lisbon Atlantic/Canary Atlantic/Madeira Atlantic/Faroe "
   "Europe/Guernsey Europe/Jersey Europe/Isle_of_Man"},
  {"Central Europe (Berlin, Paris, Rome, Madrid)", "Berlin", "Europe/Berlin",
   "CET-1CEST,M3.5.0,M10.5.0/3",
   "Europe/Paris Europe/Rome Europe/Madrid Europe/Amsterdam Europe/Brussels "
   "Europe/Vienna Europe/Zurich Europe/Stockholm Europe/Oslo Europe/Copenhagen "
   "Europe/Warsaw Europe/Prague Europe/Budapest Europe/Belgrade Europe/Zagreb "
   "Europe/Ljubljana Europe/Bratislava Europe/Luxembourg Europe/Monaco Europe/Malta "
   "Europe/Andorra Europe/Tirane Europe/Sarajevo Europe/Skopje Europe/Podgorica "
   "Europe/Gibraltar Africa/Ceuta"},
  {"West Africa (Lagos)", "Lagos", "Africa/Lagos", "WAT-1",
   "Africa/Kinshasa Africa/Luanda Africa/Douala Africa/Algiers Africa/Tunis"},
  {"Eastern Europe (Athens, Kyiv, Bucharest)", "Athens", "Europe/Athens",
   "EET-2EEST,M3.5.0/3,M10.5.0/4",
   "Europe/Helsinki Europe/Kyiv Europe/Kiev Europe/Bucharest Europe/Sofia "
   "Europe/Riga Europe/Tallinn Europe/Vilnius Asia/Nicosia Asia/Famagusta"},
  {"Israel (Jerusalem)", "Jerusalem", "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0",
   "Asia/Tel_Aviv"},
  {"Lebanon (Beirut)", "Beirut", "Asia/Beirut", "EET-2EEST,M3.5.0/0,M10.5.0/0", ""},
  {"Egypt (Cairo)", "Cairo", "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", ""},
  {"South Africa (Johannesburg)", "Johannesburg", "Africa/Johannesburg", "SAST-2",
   "Africa/Harare Africa/Maputo Africa/Lusaka Africa/Kigali Africa/Gaborone "
   "Africa/Windhoek Africa/Tripoli"},
  {"Turkey (Istanbul)", "Istanbul", "Europe/Istanbul", "<+03>-3", "Asia/Istanbul"},
  {"Moscow, Minsk", "Moscow", "Europe/Moscow", "MSK-3", "Europe/Minsk Europe/Simferopol"},
  {"Riyadh, Amman, Baghdad", "Riyadh", "Asia/Riyadh", "<+03>-3",
   "Asia/Amman Asia/Baghdad Asia/Kuwait Asia/Qatar Asia/Bahrain Asia/Aden Asia/Damascus"},
  {"East Africa (Nairobi)", "Nairobi", "Africa/Nairobi", "EAT-3",
   "Africa/Addis_Ababa Africa/Dar_es_Salaam Africa/Kampala Africa/Mogadishu"},
  {"Iran (Tehran)", "Tehran", "Asia/Tehran", "<+0330>-3:30", ""},
  {"Dubai, Baku, Tbilisi", "Dubai", "Asia/Dubai", "<+04>-4",
   "Asia/Muscat Asia/Baku Asia/Tbilisi Asia/Yerevan Indian/Mauritius"},
  // Asia, Pacific
  {"Pakistan (Karachi)", "Karachi", "Asia/Karachi", "PKT-5", ""},
  {"India, Sri Lanka", "Kolkata", "Asia/Kolkata", "IST-5:30", "Asia/Calcutta Asia/Colombo"},
  {"Bangladesh (Dhaka)", "Dhaka", "Asia/Dhaka", "<+06>-6", "Asia/Thimphu"},
  {"Bangkok, Jakarta, Hanoi", "Bangkok", "Asia/Bangkok", "<+07>-7",
   "Asia/Jakarta Asia/Ho_Chi_Minh Asia/Saigon Asia/Phnom_Penh Asia/Vientiane"},
  {"China, Hong Kong, Taiwan", "Shanghai", "Asia/Shanghai", "CST-8",
   "Asia/Hong_Kong Asia/Taipei Asia/Macau Asia/Chongqing"},
  {"Singapore, Manila, Perth", "Singapore", "Asia/Singapore", "<+08>-8",
   "Asia/Kuala_Lumpur Asia/Manila Asia/Makassar Australia/Perth"},
  {"Japan, Korea (Tokyo, Seoul)", "Tokyo", "Asia/Tokyo", "JST-9", "Asia/Seoul"},
  {"Australia Central (Adelaide)", "Adelaide", "Australia/Adelaide",
   "ACST-9:30ACDT,M10.1.0,M4.1.0/3", "Australia/Broken_Hill"},
  {"Australia Central (Darwin)", "Darwin", "Australia/Darwin", "ACST-9:30", ""},
  {"Queensland (Brisbane)", "Brisbane", "Australia/Brisbane", "AEST-10",
   "Australia/Lindeman Pacific/Guam Pacific/Port_Moresby"},
  {"Australia Eastern (Sydney, Melbourne)", "Sydney", "Australia/Sydney",
   "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Melbourne Australia/Canberra Australia/Hobart"},
  {"New Zealand (Auckland)", "Auckland", "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", ""},
};
static const int TZ_COUNT = sizeof(TZ_ZONES) / sizeof(TZ_ZONES[0]);
