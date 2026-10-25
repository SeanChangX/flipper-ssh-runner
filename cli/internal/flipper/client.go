// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"strings"
	"time"
)

// Port is the serial connection (go.bug.st/serial.Port, or a fake in tests).
type Port interface {
	io.ReadWriteCloser
	SetReadTimeout(t time.Duration) error
}

// Entry is one file or directory on the Flipper.
type Entry struct {
	Path  string
	IsDir bool
	Size  uint64
}

// Client is one RPC session.
type Client struct {
	p     Port
	rx    []byte
	cmdID uint64
	rpc   bool
}

const chunk = 512 // storage write and read chunk, as qFlipper uses

// New starts an RPC session on an open port (the Flipper's CLI switches to RPC on
// "start_rpc_session").
func New(p Port) (*Client, error) {
	c := &Client{p: p}
	if _, err := p.Write([]byte("\r")); err != nil {
		return nil, err
	}
	if err := c.readUntil(">: ", 3*time.Second); err != nil {
		return nil, fmt.Errorf("no Flipper CLI prompt (is qFlipper or another program using the port?): %w", err)
	}
	if _, err := p.Write([]byte("start_rpc_session\r")); err != nil {
		return nil, err
	}
	if err := c.readUntil("start_rpc_session\r\n", 3*time.Second); err != nil {
		return nil, err
	}
	c.rpc = true
	c.rx = nil
	return c, nil
}

// Close ends the session (back to the CLI) and closes the port.
func (c *Client) Close() error {
	if c.rpc {
		id := c.send(fStopSession, nil, false)
		_, _ = c.wait(id, 2*time.Second)
		c.rpc = false
	}
	return c.p.Close()
}

func (c *Client) readUntil(marker string, timeout time.Duration) error {
	end := time.Now().Add(timeout)
	var buf []byte
	tmp := make([]byte, 256)
	_ = c.p.SetReadTimeout(50 * time.Millisecond)
	for time.Now().Before(end) {
		n, err := c.p.Read(tmp)
		if err != nil {
			return err
		}
		buf = append(buf, tmp[:n]...)
		if bytes.Contains(buf, []byte(marker)) {
			return nil
		}
	}
	return fmt.Errorf("timed out waiting for %q", strings.TrimSpace(marker))
}

func (c *Client) frame(content int, body []byte, id uint64, hasNext bool) []byte {
	msg := fVarint(nil, fCommandID, id)
	msg = fBytes(msg, content, body)
	if hasNext {
		msg = fVarint(msg, fHasNext, 1)
	}
	return append(appendVarint(nil, uint64(len(msg))), msg...)
}

func (c *Client) send(content int, body []byte, hasNext bool) uint64 {
	c.cmdID++
	_, _ = c.p.Write(c.frame(content, body, c.cmdID, hasNext))
	return c.cmdID
}

func (c *Client) next(timeout time.Duration) (message, error) {
	end := time.Now().Add(timeout)
	tmp := make([]byte, 4096)
	_ = c.p.SetReadTimeout(50 * time.Millisecond)
	for {
		if l, n, ok := readVarint(c.rx); ok && uint64(len(c.rx)-n) >= l {
			raw := c.rx[n : n+int(l)]
			c.rx = append([]byte(nil), c.rx[n+int(l):]...)
			return decode(raw)
		}
		if time.Now().After(end) {
			return nil, errors.New("the Flipper did not answer (RPC timeout)")
		}
		n, err := c.p.Read(tmp)
		if err != nil {
			return nil, err
		}
		c.rx = append(c.rx, tmp[:n]...)
	}
}

type reply struct {
	status uint64
	parts  []message
}

func (c *Client) wait(id uint64, timeout time.Duration) (reply, error) {
	var r reply
	for {
		m, err := c.next(timeout)
		if err != nil {
			return r, err
		}
		if m.uint(fCommandID) != id {
			continue // a stray message (e.g. an earlier command's late reply)
		}
		r.status = m.uint(fStatus)
		r.parts = append(r.parts, m)
		if m.uint(fHasNext) == 0 {
			return r, nil
		}
	}
}

func (c *Client) call(content int, body []byte, what string, timeout time.Duration, ok ...uint64) (reply, error) {
	r, err := c.wait(c.send(content, body, false), timeout)
	if err != nil {
		return r, fmt.Errorf("%s: %w", what, err)
	}
	if r.status != statusOK {
		for _, s := range ok {
			if r.status == s {
				return r, nil
			}
		}
		return r, &StatusError{r.status, what}
	}
	return r, nil
}

// Ping checks the session answers.
func (c *Client) Ping() error {
	r, err := c.call(fPingReq, fString(nil, 1, "ping"), "ping", 3*time.Second)
	if err != nil {
		return err
	}
	if string(r.parts[len(r.parts)-1].sub(fPingResp).bytes(1)) != "ping" {
		return errors.New("ping: wrong answer")
	}
	return nil
}

// DeviceInfo is the Flipper's key/value device information (name, firmware, ...).
func (c *Client) DeviceInfo() (map[string]string, error) {
	r, err := c.call(fDeviceInfoReq, nil, "device info", 10*time.Second)
	if err != nil {
		return nil, err
	}
	out := map[string]string{}
	for _, p := range r.parts {
		d := p.sub(fDeviceInfoRsp)
		if k := d.bytes(1); k != nil {
			out[string(k)] = string(d.bytes(2))
		}
	}
	return out, nil
}

// AppRunning: an app holds the loader (writing its files under it is unsafe).
func (c *Client) AppRunning() (bool, error) {
	r, err := c.call(fAppLockReq, nil, "app status", 3*time.Second)
	if err != nil {
		return false, err
	}
	return r.parts[len(r.parts)-1].sub(fAppLockResp).uint(1) != 0, nil
}

// Stat returns the entry, or an error IsNotExist when there is none.
func (c *Client) Stat(path string) (Entry, error) {
	r, err := c.call(fStatReq, fString(nil, 1, path), "stat "+path, 5*time.Second)
	if err != nil {
		return Entry{}, err
	}
	f := r.parts[len(r.parts)-1].sub(fStatResp).sub(1)
	return Entry{Path: path, IsDir: f.uint(1) == 1, Size: f.uint(3)}, nil
}

// List is a directory's entries.
func (c *Client) List(path string) ([]Entry, error) {
	r, err := c.call(fListReq, fString(nil, 1, path), "list "+path, 20*time.Second)
	if err != nil {
		return nil, err
	}
	var out []Entry
	for _, p := range r.parts {
		for _, fb := range p.sub(fListResp).all(1) {
			f, err := decode(fb)
			if err != nil {
				return nil, fmt.Errorf("list %s: %w", path, err)
			}
			out = append(out, Entry{Path: strings.TrimRight(path, "/") + "/" + string(f.bytes(2)),
				IsDir: f.uint(1) == 1, Size: f.uint(3)})
		}
	}
	return out, nil
}

// Read is a whole file.
func (c *Client) Read(path string) ([]byte, error) {
	r, err := c.call(fReadReq, fString(nil, 1, path), "read "+path, 30*time.Second)
	if err != nil {
		return nil, err
	}
	var out []byte
	for _, p := range r.parts {
		out = append(out, p.sub(fReadResp).sub(1).bytes(4)...)
	}
	return out, nil
}

// MD5 is the file's md5 as the Flipper computes it (hex).
func (c *Client) MD5(path string) (string, error) {
	r, err := c.call(fMd5Req, fString(nil, 1, path), "md5 "+path, 30*time.Second)
	if err != nil {
		return "", err
	}
	return string(r.parts[len(r.parts)-1].sub(fMd5Resp).bytes(1)), nil
}

// Write replaces a file, then checks the Flipper's md5 of it.
func (c *Client) Write(path string, data []byte) error {
	c.cmdID++
	id := c.cmdID
	for off := 0; ; off += chunk {
		end := off + chunk
		if end > len(data) {
			end = len(data)
		}
		body := fString(nil, 1, path)
		body = fBytes(body, 2, fBytes(nil, 4, data[off:end]))
		if _, err := c.p.Write(c.frame(fWriteReq, body, id, end < len(data))); err != nil {
			return err
		}
		if end >= len(data) {
			break
		}
	}
	r, err := c.wait(id, 30*time.Second)
	if err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}
	if r.status != statusOK {
		return &StatusError{r.status, "write " + path}
	}
	sum, err := c.MD5(path)
	if err != nil {
		return err
	}
	want := md5.Sum(data)
	if sum != hex.EncodeToString(want[:]) {
		return fmt.Errorf("write %s: the file on the Flipper does not match (md5)", path)
	}
	return nil
}

// Mkdir creates a directory; an existing one is fine.
func (c *Client) Mkdir(path string) error {
	_, err := c.call(fMkdirReq, fString(nil, 1, path), "mkdir "+path, 5*time.Second, statusExist)
	return err
}

// Delete removes a file or (recursive) a directory tree; a missing one is fine.
func (c *Client) Delete(path string, recursive bool) error {
	body := fString(nil, 1, path)
	if recursive {
		body = fVarint(body, 2, 1)
	}
	_, err := c.call(fDeleteReq, body, "delete "+path, 30*time.Second, statusNotExist)
	return err
}
