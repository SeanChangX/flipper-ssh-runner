// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"io"
	"path"
	"sort"
	"strings"
	"sync"
	"time"
)

// Fake is an in-memory Flipper for tests: the CLI prompt, start_rpc_session, and the storage
// RPCs on a map of files. It implements Port.
type Fake struct {
	mu      sync.Mutex
	Files   map[string][]byte // full path -> content
	Dirs    map[string]bool
	AppBusy bool
	in      []byte
	out     bytes.Buffer
	rpc     bool
	writes  map[uint64]*bytes.Buffer // command id -> chunks of a write in progress
	closed  bool
}

// NewFake is an empty Flipper with /ext.
func NewFake() *Fake {
	return &Fake{Files: map[string][]byte{}, Dirs: map[string]bool{"/ext": true}, writes: map[uint64]*bytes.Buffer{}}
}

func (f *Fake) SetReadTimeout(time.Duration) error { return nil }

func (f *Fake) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return nil
}

func (f *Fake) Read(b []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return 0, io.EOF
	}
	n, _ := f.out.Read(b)
	return n, nil
}

func (f *Fake) Write(b []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.in = append(f.in, b...)
	for f.step() {
	}
	return len(b), nil
}

// step handles one complete input unit; false when more bytes are needed.
func (f *Fake) step() bool {
	if !f.rpc {
		i := bytes.IndexByte(f.in, '\r')
		if i < 0 {
			return false
		}
		line := string(f.in[:i])
		f.in = f.in[i+1:]
		if line == "start_rpc_session" {
			f.out.WriteString("start_rpc_session\r\n")
			f.rpc = true
		} else {
			f.out.WriteString("\r\n>: ")
		}
		return true
	}
	l, n, ok := readVarint(f.in)
	if !ok || uint64(len(f.in)-n) < l {
		return false
	}
	m, err := decode(f.in[n : n+int(l)])
	f.in = f.in[n+int(l):]
	if err == nil {
		f.handle(m)
	}
	return true
}

func (f *Fake) reply(id uint64, status uint64, content int, body []byte, hasNext bool) {
	msg := fVarint(nil, fCommandID, id)
	if status != 0 {
		msg = fVarint(msg, fStatus, status)
	}
	if content != 0 {
		msg = fBytes(msg, content, body)
	}
	if hasNext {
		msg = fVarint(msg, fHasNext, 1)
	}
	f.out.Write(appendVarint(nil, uint64(len(msg))))
	f.out.Write(msg)
}

func (f *Fake) exists(p string) bool { _, ok := f.Files[p]; return ok || f.Dirs[p] }

func (f *Fake) handle(m message) {
	id := m.uint(fCommandID)
	switch {
	case m[fPingReq] != nil:
		f.reply(id, 0, fPingResp, fBytes(nil, 1, m.sub(fPingReq).bytes(1)), false)
	case m[fStopSession] != nil:
		f.reply(id, 0, 0, nil, false)
		f.rpc = false
	case m[fDeviceInfoReq] != nil:
		f.reply(id, 0, fDeviceInfoRsp, fString(fString(nil, 1, "hardware_name"), 2, "Fake"), false)
	case m[fAppLockReq] != nil:
		v := uint64(0)
		if f.AppBusy {
			v = 1
		}
		f.reply(id, 0, fAppLockResp, fVarint(nil, 1, v), false)
	case m[fStatReq] != nil:
		p := string(m.sub(fStatReq).bytes(1))
		if !f.exists(p) {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		file := fVarint(nil, 1, b2u(f.Dirs[p]))
		file = fVarint(file, 3, uint64(len(f.Files[p])))
		f.reply(id, 0, fStatResp, fBytes(nil, 1, file), false)
	case m[fListReq] != nil:
		p := strings.TrimRight(string(m.sub(fListReq).bytes(1)), "/")
		if !f.Dirs[p] {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		var names []string
		for _, all := range []map[string]bool{f.Dirs, keys(f.Files)} {
			for q := range all {
				if path.Dir(q) == p && q != p {
					names = append(names, q)
				}
			}
		}
		sort.Strings(names)
		var body []byte
		for _, q := range names {
			file := fVarint(nil, 1, b2u(f.Dirs[q]))
			file = fString(file, 2, path.Base(q))
			file = fVarint(file, 3, uint64(len(f.Files[q])))
			body = fBytes(body, 1, file)
		}
		f.reply(id, 0, fListResp, body, false)
	case m[fReadReq] != nil:
		p := string(m.sub(fReadReq).bytes(1))
		data, ok := f.Files[p]
		if !ok {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		for off := 0; ; off += chunk {
			end := off + chunk
			if end > len(data) {
				end = len(data)
			}
			f.reply(id, 0, fReadResp, fBytes(nil, 1, fBytes(nil, 4, data[off:end])), end < len(data))
			if end >= len(data) {
				return
			}
		}
	case m[fWriteReq] != nil:
		w := m.sub(fWriteReq)
		p := string(w.bytes(1))
		buf := f.writes[id]
		if buf == nil {
			buf = &bytes.Buffer{}
			f.writes[id] = buf
		}
		buf.Write(w.sub(2).bytes(4))
		if m.uint(fHasNext) != 0 {
			return
		}
		delete(f.writes, id)
		if !f.Dirs[path.Dir(p)] {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		f.Files[p] = append([]byte(nil), buf.Bytes()...)
		f.reply(id, 0, 0, nil, false)
	case m[fMd5Req] != nil:
		p := string(m.sub(fMd5Req).bytes(1))
		data, ok := f.Files[p]
		if !ok {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		sum := md5.Sum(data)
		f.reply(id, 0, fMd5Resp, fString(nil, 1, hex.EncodeToString(sum[:])), false)
	case m[fMkdirReq] != nil:
		p := string(m.sub(fMkdirReq).bytes(1))
		switch {
		case f.exists(p):
			f.reply(id, statusExist, 0, nil, false)
		case !f.Dirs[path.Dir(p)]:
			f.reply(id, statusNotExist, 0, nil, false)
		default:
			f.Dirs[p] = true
			f.reply(id, 0, 0, nil, false)
		}
	case m[fDeleteReq] != nil:
		d := m.sub(fDeleteReq)
		p := string(d.bytes(1))
		if !f.exists(p) {
			f.reply(id, statusNotExist, 0, nil, false)
			return
		}
		inside := func(q string) bool { return strings.HasPrefix(q, p+"/") }
		if f.Dirs[p] && d.uint(2) == 0 {
			for q := range f.Files {
				if inside(q) {
					f.reply(id, statusDirNotEmp, 0, nil, false)
					return
				}
			}
		}
		for q := range f.Files {
			if q == p || inside(q) {
				delete(f.Files, q)
			}
		}
		for q := range f.Dirs {
			if q == p || inside(q) {
				delete(f.Dirs, q)
			}
		}
		f.reply(id, 0, 0, nil, false)
	default:
		f.reply(id, 3, 0, nil, false) // not implemented
	}
}

func b2u(b bool) uint64 {
	if b {
		return 1
	}
	return 0
}

func keys(m map[string][]byte) map[string]bool {
	out := make(map[string]bool, len(m))
	for k := range m {
		out[k] = false
	}
	return out
}
