// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"errors"
	"fmt"
	"io/fs"
	"path/filepath"
	"sort"
	"strings"

	"go.bug.st/serial"
)

// The Flipper Zero's USB CDC interface.
const (
	vid = "0483"
	pid = "5740"
)

// FindPorts lists the serial ports that are Flipper Zeros, by USB id where the system tells
// it, else by the names the Flipper's port gets (Linux by-id, macOS cu.usbmodemflip*).
func FindPorts() []string {
	out := usbPorts()
	if len(out) == 0 {
		for _, g := range []string{"/dev/serial/by-id/usb-Flipper_Devices*", "/dev/cu.usbmodemflip*"} {
			m, _ := filepath.Glob(g)
			out = append(out, m...)
		}
	}
	sort.Strings(out)
	return out
}

// listPorts: every serial port the system has (a variable for the tests).
var listPorts = serial.GetPortsList

// portError says what went wrong with opening `path` in the tool's own words, naming the
// port: a missing port lists the ports there are.
func portError(path string, err error) error {
	var pe *serial.PortError
	code := serial.PortErrorCode(-1)
	if errors.As(err, &pe) {
		code = pe.Code()
	}
	switch {
	case code == serial.PortNotFound || errors.Is(err, fs.ErrNotExist):
		found := "none"
		if ports, _ := listPorts(); len(ports) > 0 {
			sort.Strings(ports)
			found = strings.Join(ports, ", ")
		}
		return fmt.Errorf("%s: no such serial port (found: %s)", path, found)
	case code == serial.PortBusy:
		return fmt.Errorf("%s: the port is in use (close qFlipper or any other program using it)", path)
	case code == serial.PermissionDenied || errors.Is(err, fs.ErrPermission):
		return fmt.Errorf("%s: permission denied (on Linux, join the dialout group and log in again)", path)
	}
	return fmt.Errorf("%s: %w", path, err)
}

// Open connects to `path`, or to the only Flipper found when path is "".
func Open(path string) (*Client, string, error) {
	if path == "" {
		ports := FindPorts()
		switch len(ports) {
		case 0:
			return nil, "", errors.New("no Flipper Zero found on USB: plug it in and unlock it (and close qFlipper)")
		case 1:
			path = ports[0]
		default:
			return nil, "", errors.New("more than one Flipper found: choose one with --port (" + strings.Join(ports, ", ") + ")")
		}
	}
	p, err := serial.Open(path, &serial.Mode{BaudRate: 230400})
	if err != nil {
		return nil, path, portError(path, err)
	}
	c, err := New(p)
	if err != nil {
		p.Close()
		return nil, path, err
	}
	return c, path, nil
}
