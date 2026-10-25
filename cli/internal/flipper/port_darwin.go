// SPDX-License-Identifier: GPL-3.0-only
package flipper

// usbPorts: macOS lists USB ids only through cgo (IOKit); the Flipper's port name
// (/dev/cu.usbmodemflip_<name>1, in FindPorts) finds it without, so the build stays static.
func usbPorts() []string { return nil }
