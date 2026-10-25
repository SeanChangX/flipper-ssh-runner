// Package flipper talks to a Flipper Zero over USB with its protobuf RPC (the protocol
// qFlipper and Flipper Lab use): storage list, stat, read, write, mkdir, delete and md5, and
// whether an app is running. A small hand-written codec covers the few messages needed
// (field numbers from the public flipperzero-protobuf schema), so no generated code.
//
// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"errors"
	"fmt"
)

// Main message fields (flipper.proto).
const (
	fCommandID     = 1
	fStatus        = 2
	fHasNext       = 3
	fPingReq       = 5
	fPingResp      = 6
	fListReq       = 7
	fListResp      = 8
	fReadReq       = 9
	fReadResp      = 10
	fWriteReq      = 11
	fDeleteReq     = 12
	fMkdirReq      = 13
	fMd5Req        = 14
	fMd5Resp       = 15
	fAppLockReq    = 17
	fAppLockResp   = 18
	fStopSession   = 19
	fStatReq       = 24
	fStatResp      = 25
	fDeviceInfoReq = 32
	fDeviceInfoRsp = 33
)

// Status codes (flipper.proto CommandStatus) the client handles by name.
const (
	statusOK        = 0
	statusExist     = 6
	statusNotExist  = 7
	statusDirNotEmp = 18
)

var statusNames = map[uint64]string{
	1: "error", 2: "decode error", 3: "not implemented", 4: "busy", 5: "storage not ready",
	6: "already exists", 7: "does not exist", 8: "invalid parameter", 9: "access denied",
	10: "invalid name", 11: "storage error", 13: "file already open", 18: "directory not empty",
}

// StatusError is a non-OK reply.
type StatusError struct {
	Status uint64
	What   string
}

func (e *StatusError) Error() string {
	name := statusNames[e.Status]
	if name == "" {
		name = fmt.Sprintf("status %d", e.Status)
	}
	return fmt.Sprintf("%s: %s", e.What, name)
}

// IsNotExist tells a missing file or directory from other errors.
func IsNotExist(err error) bool {
	var s *StatusError
	return errors.As(err, &s) && s.Status == statusNotExist
}

func appendVarint(b []byte, v uint64) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

func readVarint(b []byte) (v uint64, n int, ok bool) {
	for shift := uint(0); n < len(b) && shift < 64; shift += 7 {
		c := b[n]
		n++
		v |= uint64(c&0x7f) << shift
		if c < 0x80 {
			return v, n, true
		}
	}
	return 0, 0, false
}

func fVarint(b []byte, num int, v uint64) []byte {
	return appendVarint(appendVarint(b, uint64(num)<<3), v)
}

func fBytes(b []byte, num int, data []byte) []byte {
	b = appendVarint(b, uint64(num)<<3|2)
	b = appendVarint(b, uint64(len(data)))
	return append(b, data...)
}

func fString(b []byte, num int, s string) []byte { return fBytes(b, num, []byte(s)) }

// message is a decoded protobuf message: varints as uint64, length-delimited as []byte.
type message map[int][]any

func decode(b []byte) (message, error) {
	m := message{}
	for len(b) > 0 {
		key, n, ok := readVarint(b)
		if !ok {
			return nil, errors.New("bad field key")
		}
		b = b[n:]
		num, wt := int(key>>3), key&7
		switch wt {
		case 0:
			v, n, ok := readVarint(b)
			if !ok {
				return nil, errors.New("bad varint")
			}
			m[num] = append(m[num], v)
			b = b[n:]
		case 2:
			l, n, ok := readVarint(b)
			if !ok || uint64(len(b)-n) < l {
				return nil, errors.New("truncated field")
			}
			m[num] = append(m[num], b[n:n+int(l)])
			b = b[n+int(l):]
		case 5:
			if len(b) < 4 {
				return nil, errors.New("truncated fixed32")
			}
			b = b[4:]
		case 1:
			if len(b) < 8 {
				return nil, errors.New("truncated fixed64")
			}
			b = b[8:]
		default:
			return nil, fmt.Errorf("wire type %d", wt)
		}
	}
	return m, nil
}

func (m message) uint(num int) uint64 {
	if v, ok := m[num]; ok && len(v) > 0 {
		if u, ok := v[0].(uint64); ok {
			return u
		}
	}
	return 0
}

func (m message) bytes(num int) []byte {
	if v, ok := m[num]; ok && len(v) > 0 {
		if b, ok := v[0].([]byte); ok {
			return b
		}
	}
	return nil
}

func (m message) all(num int) [][]byte {
	var out [][]byte
	for _, v := range m[num] {
		if b, ok := v.([]byte); ok {
			out = append(out, b)
		}
	}
	return out
}

func (m message) sub(num int) message {
	s, _ := decode(m.bytes(num))
	if s == nil {
		return message{}
	}
	return s
}
