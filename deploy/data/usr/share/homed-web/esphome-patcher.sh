#!/bin/sh

BASE=$(dirname "${0}")
[ -f "${BASE}/js/app.js" ] || { echo "homed-web not found"; exit 1; }
grep -q 'let shortNames' "${BASE}/js/app.js" && { echo "Your homed-web is too old, update first"; exit 1; }
grep -q esphome "${BASE}/index.html" && echo "index.html already patched" } || sed '/zigbee.js/a <script src="js/services/esphome.js"></script>' -i "${BASE}/index.html"
grep -q esphome "${BASE}/js/app.js" && echo "js/app.js already patched" } || sed "/case 'zigbee/a case 'esphome':    this.services[service] = new ESPHome(this, list[2]); break;
; /let names.*recorder/s|custom'|custom', 'esphome'|; /let short/s|'cust'|'cust', 'esp'|;" -i "${BASE}/js/app.js"
echo done
