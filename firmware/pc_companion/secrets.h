#define WIFI_SSID "PUT_SSID_HERE"
#define WIFI_PASSWORD "PUT+PSK_HERE"

// Home Assistant -- used by the HA page to call services (see
// call_ha_service() in pc_companion.ino). Generate a long-lived access
// token from your HA user profile page (bottom of the page, "Long-Lived
// Access Tokens" -> Create Token) -- HA only shows it once, so copy it
// right away.
#define HA_HOST "PUT_HA_IP_OR_HOSTNAME_HERE"
#define HA_PORT 8123
#define HA_TOKEN "PUT_TOKEN_HERE"
