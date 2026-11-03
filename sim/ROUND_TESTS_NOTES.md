# Round Tests Migration Notes

## Overview
Migrated simulator tests from the legacy vertical scrolling Rooms list to the round-first Rooms dial, and transitioned Home's room-device and Scenes lists to shared chord-fitted rows.

## Task 1: Dial Test Migrations
1. **`sim/tests/test_shell.c` (`offline_contract_{default,ax3_bold_rt,reduce_motion}`)**:
   - Replaced assertions expecting cached room rows ("Living Room") and disabled scene capsules ("Good Night") with assertions verifying that the dial ring and scenes capsule are absent.
   - Asserted that the house caption ("Offline") and the connection explanation ("Connect your Mac to load Home.") are displayed.
   - Preserved shell offline restrictions (`!orb_home_take_request`).

2. **`sim/tests/test_scroll_fixtures.c` (`scroll_fixtures`)**:
   - Removed `ORB_SCROLL_ROOMS` from the list-scroll fixture test loop, since Rooms is now a dial rather than a vertical scrolling list.
   - Retained `orb_scroll_surface_parse` verification for all surfaces, and retained room-device scroll coverage in the Home test suite.

3. **`sim/tests/test_nav_shell.c` (`nav_shell`)**:
   - Removed legacy Favourites-to-Rooms section navigation and list scroll owner assertions.
   - Verified that Rooms has no native scroll owner (`scroll_list == NULL`), that the dial owner exists, and that rim steps (`orb_shell_twist`) step focus between rooms ("Living Room" <-> "Bedroom").

4. **`sim/tests/test_list_scroll.c` (`list_scroll`, `list_scroll_ax3`)**:
   - Removed `"home-rooms"` from the scrollable surface fixture table, while keeping `"home-room"` room-device list scroll coverage.

## Task 2: Chord-Fitted Rows for Home Lists
1. **`firmware/components/orb_home/orb_home_view.c`**:
   - Updated `HOME_ROW_WIDTH` from 340 to the native 418 px width.
   - In `home_list_finish()`, opted into chord fitting using `orb_round_list_apply` with `.margin = 24, .fade = 38, .stepped = true, .chord_fit = true`, matching Settings in `orb_workspace.c`.
   - In `room_view()`, aligned trailing accessory labels (`val`) to the right edge via `lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0)` so that `chord_fit` dynamically tracks trailing accessory bounds during scroll.

2. **Test Updates**:
   - Updated row width assertions in `sim/tests/test_home_view.c:157` and `sim/tests/test_home_shell.c:234` from 340 to 418 px.

## Verification
- Built and ran all simulator tests using `make sim-test`.
- All 904 tests passed (100% pass rate).
