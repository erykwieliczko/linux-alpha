.. SPDX-License-Identifier: GPL-2.0-or-later

J700 C1FE multitouch first light
===============================

Scope and selection
-------------------

The MacBook Neo J700 C1FE personality uses a distinct MTP report layout.
The experimental board description opts in with a ``multi-touch`` child
compatible with ``apple,j700-multitouch`` below the DockChannel HID device.
The magicmouse driver checks this only in its BUS_HOST trackpad path, after
the normal HID_TYPE_SPI_MOUSE check. An unknown product ID, missing STM
interface or matching packet length alone does not select this format.

Legacy devices retain the 38-byte header, area-based contact filtering and
existing size, orientation and pressure reporting. A malformed selected
C1FE report never falls through to the legacy decoder.

Supported captured format
-------------------------

Offsets include the report ID at byte zero. Multibyte fields are little-endian.
The supported profile has a 32-byte header, N 30-byte contact records and an
eight-byte opaque suffix:

===================  ================================================
Offset / size        Required value or use
===================  ================================================
0 / 1                Report ID 0x75
2 / 1                Observed header length 32
3 / 1                Observed signature 4; no version meaning assigned
16 / 4               Contact-section length, exactly 30 * N
20 / 2               Suffix length, exactly 8
22 / 1               Declared contact count N, at most MAX_CONTACTS (16)
23 / 1               Bit zero reports BTN_MOUSE (BTN_LEFT)
32 + 30*i / 30       Contact record i, for i < N
32 + 30*N / 8        Opaque suffix, excluded from contact decoding
===================  ================================================

Total payload length must equal 32 + 30*N + 8. Validate the complete shape
before reading contacts or emitting events. A failed validation emits neither
touches nor synthetic releases. These strict section and signature checks
describe the captured C1FE profile, not a universal guarantee about future
J700 firmware. The reference behavior conveyed through the cleanroom handoff
accepts a broader length-based shape; no available capture requires that
broader acceptance here. Sixteen contacts is a software capacity, not a
measured hardware maximum.

Contact semantics
-----------------

Within each record, signed 16-bit X and Y are at offsets +4 and +6. Y is
negated after widening, using the existing MTP coordinate convention and
dimensions query (feature report 0xd9), not the observed trace extrema as
sensor bounds. Contact presence follows the declared count. Opaque candidate
area, identity or state fields must not suppress a declared contact.

Use the existing position-based slot assignment and drop-unused behavior.
Every valid report synchronizes a complete frame, including count zero,
which releases previous contacts. A lower-count frame drops unmatched slots.
The primary button is independent of contact count. Do not derive additional
buttons from the suffix or use opaque record bytes as persistent slot IDs.

The variant reports positions, tracking and the primary button. It does not
advertise touch area, tool width, orientation or pressure: those legacy field
interpretations are not established for C1FE, which has no force sensor.
Generic HID parsing must not leave unsupported ABS capabilities enabled.

The existing BUS_HOST registration of report 0x75 is necessary even when it
is absent from the static descriptor. This parser introduces no firmware
enable command or lifecycle change. Startup reports 0x60 and 0x52 are not
touch or release reports. The separate SPI reset/enable path must not be
applied to them. Firmware loading, Power Method 2 and retained MTP ownership
remain the transport's responsibility.

Provenance and validation
-------------------------

The implementation uses the approved cleanroom handoff's behavioral review
of the J700 parser at revision ddb87a170804f05ee56b15f314ef15cda3b2d812,
together with complete touch-only payload captures. No executable firmware
was disassembled. This document preserves the contract without requiring
the original temporary handoff files or access to restricted source.

The 2026-09-07 capture contains 74 complete packets: two startup reports and
72 runtime reports. All runtime reports satisfy the framing equations above.
They contain 77 declared records, including 12 with zero in the candidate
area field. C1FE retains all 77; legacy-format equivalents retain the earlier
area filter and produce 65 contacts. The random, unlabelled trace does not
independently establish physical axis orientation or click meaning.

The sibling linux-enablement-mac-alpha repository contains the capture and
``tests/test_magicmouse_c1fe.py``. It compiles the actual report handler with
host input-core mocks under ASan/UBSan. Replay covers coordinates, count-driven
presence, buttons, unsupported geometry/pressure event omission, opaque-field
mutations, truncations, section inconsistencies, capacity bounds, signed
coordinate extremes, zero-contact clicks and legacy regression cases.
Mock slot assignment does not test the input core's tracking algorithm or
prove the final advertised capabilities of a registered device.

On 2026-09-07 the operator confirmed that the revised decoder worked in the
RAM-booted Linux framebuffer contact demo, after earlier confirming keyboard
input. This is a functional first-light hardware result, not a formal axis
calibration, desktop/libinput, palm-rejection, suspend or cold-takeover test.
Existing-machine hardware regression testing is not implied by host replay.
