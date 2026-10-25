//go:build !darwin

// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"strings"

	"go.bug.st/serial/enumerator"
)

// usbPorts: the serial ports whose USB id is a Flipper's.
func usbPorts() []string {
	var out []string
	if list, err := enumerator.GetDetailedPortsList(); err == nil {
		for _, p := range list {
			if p.IsUSB && strings.EqualFold(p.VID, vid) && strings.EqualFold(p.PID, pid) {
				out = append(out, p.Name)
			}
		}
	}
	return out
}
