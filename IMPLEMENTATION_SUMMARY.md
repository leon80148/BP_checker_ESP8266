# Implementation Summary

- Added a minimal GitHub Actions workflow at `.github/workflows/ci.yml`.
- The workflow compiles the existing `bp_ckecker8266` Arduino sketch for a NodeMCU-style ESP8266 board using `arduino/compile-sketches`.
- The CI installs the ESP8266 Boards Manager package, the local `lib/` headers, and the ArduinoJson dependency; it does not require secrets, hardware, deployment steps, or device upload credentials.
- The sketch now includes local project headers through the Arduino library include path so CI can compile from the sketch directory.
- Removed the static first-boot AP password and now generate a random default that is saved to EEPROM when no saved AP password exists.
- Documented the generated first-boot AP password flow and the expected post-setup AP password change.
