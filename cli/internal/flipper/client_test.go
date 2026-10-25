// SPDX-License-Identifier: GPL-3.0-only
package flipper

import (
	"bytes"
	"testing"
)

func TestClientStorageOnFake(t *testing.T) {
	f := NewFake()
	c, err := New(f)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Ping(); err != nil {
		t.Fatal(err)
	}
	if err := c.Mkdir("/ext/apps_data"); err != nil {
		t.Fatal(err)
	}
	if err := c.Mkdir("/ext/apps_data"); err != nil { // already there: fine
		t.Fatal(err)
	}
	big := bytes.Repeat([]byte("0123456789"), 300) // several 512-byte chunks each way
	if err := c.Write("/ext/apps_data/a.conf", big); err != nil {
		t.Fatal(err)
	}
	got, err := c.Read("/ext/apps_data/a.conf")
	if err != nil || !bytes.Equal(got, big) {
		t.Fatalf("read back %d bytes, %v", len(got), err)
	}
	if err := c.Write("/ext/apps_data/empty", nil); err != nil {
		t.Fatal(err)
	}
	list, err := c.List("/ext/apps_data")
	if err != nil || len(list) != 2 || list[0].Path != "/ext/apps_data/a.conf" || list[0].Size != uint64(len(big)) {
		t.Fatalf("list %+v %v", list, err)
	}
	e, err := c.Stat("/ext/apps_data")
	if err != nil || !e.IsDir {
		t.Fatalf("stat %+v %v", e, err)
	}
	if _, err := c.Stat("/ext/nope"); !IsNotExist(err) {
		t.Fatalf("stat missing: %v", err)
	}
	if err := c.Delete("/ext/apps_data", false); err == nil {
		t.Fatal("non-recursive delete of a full directory succeeded")
	}
	if err := c.Delete("/ext/apps_data", true); err != nil {
		t.Fatal(err)
	}
	if err := c.Delete("/ext/apps_data", true); err != nil { // gone already: fine
		t.Fatal(err)
	}
	if busy, err := c.AppRunning(); err != nil || busy {
		t.Fatalf("app running %v %v", busy, err)
	}
	f.AppBusy = true
	if busy, _ := c.AppRunning(); !busy {
		t.Fatal("busy not seen")
	}
	if err := c.Close(); err != nil {
		t.Fatal(err)
	}
}
