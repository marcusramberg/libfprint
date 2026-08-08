# Goodix QSEE match-on-chip driver

This driver implements version 1 of the Goodix trusted-application protocol
recovered on the `goodix,gf3626` reference platform.  It binds the
`goodix_fp` misc device only when that firmware compatible is present.  The TA
filename is read from the kernel `firmware_name` sysfs attribute, populated by
the device-tree `firmware-name` property; it is not compiled into the driver.

The machine-wide QSEE supplicant must already be running.  This driver opens
only the non-supplicant QSEE client device (`/dev/teeN`) and never registers
listener services.
Print data contains a protocol-profile number, group ID, and Goodix finger ID.
Images and templates remain sealed in the configured listener storage.

Enrollment currently uses the reference platform's challenge-only 69-byte HAT
fallback.  `GoodixQseeTokenProvider` is the boundary for replacing it with a
complete Gatekeeper-signed HAT supplied by a separate credential service.  The
fallback does not prove recent credential knowledge and must not be described
as signed-token support.

The protocol profile must not be assumed compatible with a differently named
Goodix TA, another OEM, another Goodix sensor generation, or another TEE.  Add
positive device matching and a separate profile or driver after establishing
ABI compatibility.
