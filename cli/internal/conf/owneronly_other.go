//go:build !windows

// SPDX-License-Identifier: GPL-3.0-only
package conf

// ownerOnly: on Unix the 0700 the folder is made with already does it.
func ownerOnly(dir string) error { return nil }
