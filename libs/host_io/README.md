# host_io — Freshwater PC host transports

Optional companion to `libs/protocol`. Owns OS serial ports and the two
pipes Freshwater apps use:

| Folder / namespace | Role |
| ----------------- | ---- |
| `host_io::SerialPort` | Shared byte pipe (termios / Win32) |
| `host_io::rs485` | Tagged multi-drop link, `Rs485Transport`, `Bus` bootstrap |
| `host_io::usb` | CDC console transport + `wl` wave upload |

Depends on `protocol::protocol` (typed clients + wire encoding).

```cmake
add_subdirectory(path/to/libs/host_io)
target_link_libraries(your_app PRIVATE host_io::host_io)
```

```cpp
#include "host_io/rs485/bus.hpp"
#include "host_io/usb/wave_upload.hpp"

host_io::rs485::Bus bus;
host_io::usb::WaveUploader up(port);
```

Apps that only need command strings should depend on **protocol alone**.
