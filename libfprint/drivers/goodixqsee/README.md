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

The stock fprintd systemd sandbox may need a downstream device-policy drop-in:

```ini
[Service]
DeviceAllow=/dev/goodix_fp rw
DeviceAllow=char-tee rw
```

The TEE implementation is discovered by `TEE_IOC_VERSION`, so a fixed
`/dev/tee0` or `/dev/tee1` rule is not reliable.  fprintd does not need access
to `/dev/teeprivN`; that privileged node belongs only to the machine-wide
supplicant and application loader.

On distributions that install `pam_fprintd.so` without enabling it for GDM,
add a separate `/etc/pam.d/gdm-fingerprint` service:

```pam
#%PAM-1.0
auth       required    pam_fprintd.so
account    include     base-account
password   include     base-password
session    include     base-session
```

Keep this separate from `gdm-password`. GDM runs password and fingerprint
authentication in separate workers, so adding fingerprint authentication to a
shared PAM stack can delay or change unrelated password, SSH, and system
authentication paths.

The reference platform has been tested through fprintd for enumeration,
enrollment, duplicate rejection, known-finger verification, single-print
deletion, cancellation, per-sample retry feedback, and matched finger-ID
reporting. GNOME login and session unlock have been tested through the separate
GDM fingerprint PAM service.

Enrollment currently uses the reference platform's challenge-only 69-byte HAT
fallback.  `GoodixQseeTokenProvider` is the boundary for replacing it with a
complete Gatekeeper-signed HAT supplied by a separate credential service.  The
fallback does not prove recent credential knowledge and must not be described
as signed-token support.

The protocol profile must not be assumed compatible with a differently named
Goodix TA, another OEM, another Goodix sensor generation, or another TEE.  Add
positive device matching and a separate profile or driver after establishing
ABI compatibility.
