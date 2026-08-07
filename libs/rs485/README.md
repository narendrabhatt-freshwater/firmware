# rs485 — Freshwater host serial / bus glue

Optional companion to [`libs/protocol`](../protocol). Opens OS serial ports,
implements tagged RS485 `Link`, USB CDC helpers, `rs485::Bus` bootstrap, and
CDC wave upload.

Depends on `protocol::protocol` (typed clients + wire encoding).

Apps that only need command strings should depend on **protocol alone**.
