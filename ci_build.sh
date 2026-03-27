#!/bin/bash
# CI build script — run by .github/workflows/release.yml inside the ESP-IDF Docker container.
set -e

# Remove lock file so components are resolved fresh from idf_component.yml version pins.
rm -f dependencies.lock

# Download managed components (runs CMake + component manager, no full compile).
idf.py reconfigure

# Apply the Nullable.h patch required to build esp_matter 1.4.2 with GCC 14.
# Identical to the sed applied locally after moving espressif__esp_matter to components/.
NULLABLE_H="managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/app/data-model/Nullable.h"
sed -i '116s/return.*/return true; \/\/ Temporary Hack/' "$NULLABLE_H"

idf.py build
