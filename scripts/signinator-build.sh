#!/usr/bin/env bash
# Build the ESP32-S3 receiver with the Signinator now-playing forwarder (used by .github/workflows/signinator.yml,
# inside the espressif/idf container). Output: out/airplay-signinator-esp32s3-{factory,ota}.bin
set -euo pipefail
idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3;config/sdkconfig.user.signinator" build
cd build
esptool.py --chip esp32s3 merge_bin --format raw -o merged-firmware.bin $(cat flash_args)
mkdir -p ../out
cp merged-firmware.bin ../out/airplay-signinator-esp32s3-factory.bin
app=$(python3 -c 'import json; print(json.load(open("project_description.json"))["app_bin"])')
cp "$app" ../out/airplay-signinator-esp32s3-ota.bin
