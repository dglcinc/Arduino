# Arduino

Sketches for the pivac monitoring system: the AirMaster (a Wi-Fi bridge for the Unico S.M.A.R.T. Control Board), the air-handler sensor nodes, the pressure boards, the domestic water meter and the Chiltrix Modbus bridge. `CLAUDE.md` describes each board and how to build it; `AirMaster/README.md` covers building, labelling and flashing AirMaster units. The companion repository is [pivac](https://github.com/dglcinc/pivac), which holds the Raspberry Pi software and the board designs.

## Licensing

This repository is free for personal and other noncommercial use. Commercial use, including installing it for a customer, building it into a product, or selling units running this firmware, needs a separate licence from the copyright holder; ask through [github.com/dglcinc](https://github.com/dglcinc).

| Content | Licence |
|---------|---------|
| Sketches, firmware, scripts, the AirMaster web page | PolyForm Noncommercial 1.0.0 (`LICENSE`) |
| Documents | CC BY-NC 4.0 (`LICENSE-DOCS`) |

Source files carry an `SPDX-License-Identifier` line. Libraries the sketches include, such as the board cores, TinyUSB, OneWire and ModbusMaster, are not part of this repository and keep their own licences.
