# cardlink — Freshwater PC host transports

Optional companion to `protocol/libs/cardproto`. Owns OS serial ports and the
pipes Freshwater apps use:

| Namespace | Role |
| --------- | ---- |
| `cardlink::SerialPort` | Shared byte pipe (termios / Win32) |
| `cardlink::rs485` | Tagged multi-drop link, `Rs485Transport`, `Bus` bootstrap |
| `cardlink::usb` | CDC console transport + `al`/`bl` sample upload (`AttackUploader`, `BodyUploader`) |

Depends on `cardproto::cardproto` (typed clients + wire encoding).

```cmake
add_subdirectory(path/to/protocol/libs/cardlink)
target_link_libraries(your_app PRIVATE cardlink::cardlink)
```

```cpp
#include "cardlink/rs485/bus.hpp"
#include "cardlink/usb/attack_upload.hpp"

cardlink::rs485::Bus bus;
cardlink::usb::AttackUploader up(port);
```

`cardlink::usb::WaveUploader` targets the removed firmware `wl` command and
is pending removal — do not build new code on it.

Apps that only need command strings should depend on **cardproto alone**.
