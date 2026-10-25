// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"errors"
	"io/fs"
	"testing"
)

func TestPortErrorNamesThePortAndListsOthers(t *testing.T) {
	old := listPorts
	defer func() { listPorts = old }()
	listPorts = func() ([]string, error) { return []string{"COM8", "COM5", "COM7"}, nil }
	missing := &fs.PathError{Op: "open", Path: "COM99", Err: fs.ErrNotExist}
	if got := portError("COM99", missing).Error(); got != "COM99: no such serial port (found: COM5, COM7, COM8)" {
		t.Fatalf("%q", got)
	}
	listPorts = func() ([]string, error) { return nil, nil }
	if got := portError("/dev/ttyACM9", missing).Error(); got != "/dev/ttyACM9: no such serial port (found: none)" {
		t.Fatalf("%q", got)
	}
	denied := &fs.PathError{Op: "open", Path: "/dev/ttyACM0", Err: fs.ErrPermission}
	if got := portError("/dev/ttyACM0", denied).Error(); got != "/dev/ttyACM0: permission denied (on Linux, join the dialout group and log in again)" {
		t.Fatalf("%q", got)
	}
	if got := portError("COM3", errors.New("boom")).Error(); got != "COM3: boom" {
		t.Fatalf("%q", got)
	}
}

func TestOpenMissingPort(t *testing.T) {
	old := listPorts
	defer func() { listPorts = old }()
	listPorts = func() ([]string, error) { return []string{"/dev/ttyACM0"}, nil }
	_, _, err := Open("/dev/sshrunner-no-such-port")
	if err == nil || err.Error() != "/dev/sshrunner-no-such-port: no such serial port (found: /dev/ttyACM0)" {
		t.Fatalf("%v", err)
	}
}
