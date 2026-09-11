# Changelog

## [V2.1.1] - 11-09-2026
- Add restart Modal on Zigbee-Settings change.

## [V2.1.0] - 11-12-2025
- Fixed project versioning: the last release should have been a minor update, not a patch.
- The power LED now remains permanently on in router mode. 
- Fixed a bug where incorrect information was displayed during CC update.

## [V2.0.4] - 09-12-2025
- Fixed issue that prevented Router mode from being usable.
- Added LED indicators to provide visual feedback during ZigBee updates.
- Disallowed ZigBee chip updates while hosts are connected to avoid undefined states in host applications.
- Applied several web-interface adjustments when not operating in Coordinator mode.

## [V2.0.3] - 30-10-2025
- Fixed a bug where you were unable to successfully update the ZigBee firmware.
- Added a push restart-notification to the Zigbee/Serial setting tab.

## [V2.0.2] - 11-07-2025
- Adapted the versioning within the coordinator to that of the Github releases.
- Added a line to the configuration generator for the zStack adapter.
- Changed the text in the update-available window.
- Adapted the device model name on the status page.