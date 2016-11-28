// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package blowfish

import "testing"

type CryptTest struct {
	key []byte
	in  []byte
	out []byte
}

// Test vector values are from http://www.schneier.com/code/vectors.txt.
var encryptTests = []CryptTest{
	{
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x4E, 0xF9, 0x97, 0x45, 0x61, 0x98, 0xDD, 0x78}},
	{
		[]byte{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		[]byte{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		[]byte{0x51, 0x86, 0x6F, 0xD5, 0xB8, 0x5E, 0xCB, 0x8A}},
	{
		[]byte{0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
		[]byte{0x7D, 0x85, 0x6F, 0x9A, 0x61, 0x30, 0x63, 0xF2}},
	{
		[]byte{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
		[]byte{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
		[]byte{0x24, 0x66, 0xDD, 0x87, 0x8B, 0x96, 0x3C, 0x9D}},

	{
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
		[]byte{0x61, 0xF9, 0xC3, 0x80, 0x22, 0x81, 0xB0, 0x96}},
	{
		[]byte{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0x7D, 0x0C, 0xC6, 0x30, 0xAF, 0xDA, 0x1E, 0xC7}},
	{
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x4E, 0xF9, 0x97, 0x45, 0x61, 0x98, 0xDD, 0x78}},
	{
		[]byte{0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10},
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0x0A, 0xCE, 0xAB, 0x0F, 0xC6, 0xA0, 0xA2, 0x8D}},
	{
		[]byte{0x7C, 0xA1, 0x10, 0x45, 0x4A, 0x1A, 0x6E, 0x57},
		[]byte{0x01, 0xA1, 0xD6, 0xD0, 0x39, 0x77, 0x67, 0x42},
		[]byte{0x59, 0xC6, 0x82, 0x45, 0xEB, 0x05, 0x28, 0x2B}},
	{
		[]byte{0x01, 0x31, 0xD9, 0x61, 0x9D, 0xC1, 0x37, 0x6E},
		[]byte{0x5C, 0xD5, 0x4C, 0xA8, 0x3D, 0xEF, 0x57, 0xDA},
		[]byte{0xB1, 0xB8, 0xCC, 0x0B, 0x25, 0x0F, 0x09, 0xA0}},
	{
		[]byte{0x07, 0xA1, 0x13, 0x3E, 0x4A, 0x0B, 0x26, 0x86},
		[]byte{0x02, 0x48, 0xD4, 0x38, 0x06, 0xF6, 0x71, 0x72},
		[]byte{0x17, 0x30, 0xE5, 0x77, 0x8B, 0xEA, 0x1D, 0xA4}},
	{
		[]byte{0x38, 0x49, 0x67, 0x4C, 0x26, 0x02, 0x31, 0x9E},
		[]byte{0x51, 0x45, 0x4B, 0x58, 0x2D, 0xDF, 0x44, 0x0A},
		[]byte{0xA2, 0x5E, 0x78, 0x56, 0xCF, 0x26, 0x51, 0xEB}},
	{
		[]byte{0x04, 0xB9, 0x15, 0xBA, 0x43, 0xFE, 0xB5, 0xB6},
		[]byte{0x42, 0xFD, 0x44, 0x30, 0x59, 0x57, 0x7F, 0xA2},
		[]byte{0x35, 0x38, 0x82, 0xB1, 0x09, 0xCE, 0x8F, 0x1A}},
	{
		[]byte{0x01, 0x13, 0xB9, 0x70, 0xFD, 0x34, 0xF2, 0xCE},
		[]byte{0x05, 0x9B, 0x5E, 0x08, 0x51, 0xCF, 0x14, 0x3A},
		[]byte{0x48, 0xF4, 0xD0, 0x88, 0x4C, 0x37, 0x99, 0x18}},
	{
		[]byte{0x01, 0x70, 0xF1, 0x75, 0x46, 0x8F, 0xB5, 0xE6},
		[]byte{0x07, 0x56, 0xD8, 0xE0, 0x77, 0x47, 0x61, 0xD2},
		[]byte{0x43, 0x21, 0x93, 0xB7, 0x89, 0x51, 0xFC, 0x98}},
	{
		[]byte{0x43, 0x29, 0x7F, 0xAD, 0x38, 0xE3, 0x73, 0xFE},
		[]byte{0x76, 0x25, 0x14, 0xB8, 0x29, 0xBF, 0x48, 0x6A},
		[]byte{0x13, 0xF0, 0x41, 0x54, 0xD6, 0x9D, 0x1A, 0xE5}},
	{
		[]byte{0x07, 0xA7, 0x13, 0x70, 0x45, 0xDA, 0x2A, 0x16},
		[]byte{0x3B, 0xDD, 0x11, 0x90, 0x49, 0x37, 0x28, 0x02},
		[]byte{0x2E, 0xED, 0xDA, 0x93, 0xFF, 0xD3, 0x9C, 0x79}},
	{
		[]byte{0x04, 0x68, 0x91, 0x04, 0xC2, 0xFD, 0x3B, 0x2F},
		[]byte{0x26, 0x95, 0x5F, 0x68, 0x35, 0xAF, 0x60, 0x9A},
		[]byte{0xD8, 0x87, 0xE0, 0x39, 0x3C, 0x2D, 0xA6, 0xE3}},
	{
		[]byte{0x37, 0xD0, 0x6B, 0xB5, 0x16, 0xCB, 0x75, 0x46},
		[]byte{0x16, 0x4D, 0x5E, 0x40, 0x4F, 0x27, 0x52, 0x32},
		[]byte{0x5F, 0x99, 0xD0, 0x4F, 0x5B, 0x16, 0x39, 0x69}},
	{
		[]byte{0x1F, 0x08, 0x26, 0x0D, 0x1A, 0xC2, 0x46, 0x5E},
		[]byte{0x6B, 0x05, 0x6E, 0x18, 0x75, 0x9F, 0x5C, 0xCA},
		[]byte{0x4A, 0x05, 0x7A, 0x3B, 0x24, 0xD3, 0x97, 0x7B}},
	{
		[]byte{0x58, 0x40, 0x23, 0x64, 0x1A, 0xBA, 0x61, 0x76},
		[]byte{0x00, 0x4B, 0xD6, 0xEF, 0x09, 0x17, 0x60, 0x62},
		[]byte{0x45, 0x20, 0x31, 0xC1, 0xE4, 0xFA, 0xDA, 0x8E}},
	{
		[]byte{0x02, 0x58, 0x16, 0x16, 0x46, 0x29, 0xB0, 0x07},
		[]byte{0x48, 0x0D, 0x39, 0x00, 0x6E, 0xE7, 0x62, 0xF2},
		[]byte{0x75, 0x55, 0xAE, 0x39, 0xF5, 0x9B, 0x87, 0xBD}},
	{
		[]byte{0x49, 0x79, 0x3E, 0xBC, 0x79, 0xB3, 0x25, 0x8F},
		[]byte{0x43, 0x75, 0x40, 0xC8, 0x69, 0x8F, 0x3C, 0xFA},
		[]byte{0x53, 0xC5, 0x5F, 0x9C, 0xB4, 0x9F, 0xC0, 0x19}},
	{
		[]byte{0x4F, 0xB0, 0x5E, 0x15, 0x15, 0xAB, 0x73, 0xA7},
		[]byte{0x07, 0x2D, 0x43, 0xA0, 0x77, 0x07, 0x52, 0x92},
		[]byte{0x7A, 0x8E, 0x7B, 0xFA, 0x93, 0x7E, 0x89, 0xA3}},
	{
		[]byte{0x49, 0xE9, 0x5D, 0x6D, 0x4C, 0xA2, 0x29, 0xBF},
		[]byte{0x02, 0xFE, 0x55, 0x77, 0x81, 0x17, 0xF1, 0x2A},
		[]byte{0xCF, 0x9C, 0x5D, 0x7A, 0x49, 0x86, 0xAD, 0xB5}},
	{
		[]byte{0x01, 0x83, 0x10, 0xDC, 0x40, 0x9B, 0x26, 0xD6},
		[]byte{0x1D, 0x9D, 0x5C, 0x50, 0x18, 0xF7, 0x28, 0xC2},
		[]byte{0xD1, 0xAB, 0xB2, 0x90, 0x65, 0x8B, 0xC7, 0x78}},
	{
		[]byte{0x1C, 0x58, 0x7F, 0x1C, 0x13, 0x92, 0x4F, 0xEF},
		[]byte{0x30, 0x55, 0x32, 0x28, 0x6D, 0x6F, 0x29, 0x5A},
		[]byte{0x55, 0xCB, 0x37, 0x74, 0xD1, 0x3E, 0xF2, 0x01}},
	{
		[]byte{0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0xFA, 0x34, 0xEC, 0x48, 0x47, 0xB2, 0x68, 0xB2}},
	{
		[]byte{0x1F, 0x1F, 0x1F, 0x1F, 0x0E, 0x0E, 0x0E, 0x0E},
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0xA7, 0x90, 0x79, 0x51, 0x08, 0xEA, 0x3C, 0xAE}},
	{
		[]byte{0xE0, 0xFE, 0xE0, 0xFE, 0xF1, 0xFE, 0xF1, 0xFE},
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0xC3, 0x9E, 0x07, 0x2D, 0x9F, 0xAC, 0x63, 0x1D}},
	{
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		[]byte{0x01, 0x49, 0x33, 0xE0, 0xCD, 0xAF, 0xF6, 0xE4}},
	{
		[]byte{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0xF2, 0x1E, 0x9A, 0x77, 0xB7, 0x1C, 0x49, 0xBC}},
	{
		[]byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
		[]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		[]byte{0x24, 0x59, 0x46, 0x88, 0x57, 0x54, 0x36, 0x9A}},
	{
		[]byte{0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10},
		[]byte{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		[]byte{0x6B, 0x5C, 0x5A, 0x9C, 0x5D, 0x9E, 0x0A, 0x5A}},
}

func TestCipherEncrypt(t *testing.T) {
	for i, tt := range encryptTests {
		c, err := NewCipher(tt.key)
		if err != nil {
			t.Errorf("NewCipher(%d bytes) = %s", len(tt.key), err)
			continue
		}
		ct := make([]byte, len(tt.out))
		c.Encrypt(ct, tt.in)
		for j, v := range ct {
			if v != tt.out[j] {
				t.Errorf("Cipher.Encrypt, test vector #%d: cipher-text[%d] = %#x, expected %#x", i, j, v, tt.out[j])
				break
			}
		}
	}
}

func TestCipherDecrypt(t *testing.T) {
	for i, tt := range encryptTests {
		c, err := NewCipher(tt.key)
		if err != nil {
			t.Errorf("NewCipher(%d bytes) = %s", len(tt.key), err)
			continue
		}
		pt := make([]byte, len(tt.in))
		c.Decrypt(pt, tt.out)
		for j, v := range pt {
			if v != tt.in[j] {
				t.Errorf("Cipher.Decrypt, test vector #%d: plain-text[%d] = %#x, expected %#x", i, j, v, tt.in[j])
				break
			}
		}
	}
}

func TestSaltedCipherKeyLength(t *testing.T) {
	if _, err := NewSaltedCipher(nil, []byte{'a'}); err != KeySizeError(0) {
		t.Errorf("NewSaltedCipher with short key, gave error %#v, expected %#v", err, KeySizeError(0))
	}

	// A 57-byte key. One over the typical blowfish restriction.
	key := []byte("012345678901234567890123456789012345678901234567890123456")
	if _, err := NewSaltedCipher(key, []byte{'a'}); err != nil {
		t.Errorf("NewSaltedCipher with long key, gave error %#v", err)
	}
}

// Test vectors generated with Blowfish from OpenSSH.
var saltedVectors = [][8]byte{
	{0x0c, 0x82, 0x3b, 0x7b, 0x8d, 0x01, 0x4b, 0x7e},
	{0xd1, 0xe1, 0x93, 0xf0, 0x70, 0xa6, 0xdb, 0x12},
	{0xfc, 0x5e, 0xba, 0xde, 0xcb, 0xf8, 0x59, 0xad},
	{0x8a, 0x0c, 0x76, 0xe7, 0xdd, 0x2c, 0xd3, 0xa8},
	{0x2c, 0xcb, 0x7b, 0xee, 0xac, 0x7b, 0x7f, 0xf8},
	{0xbb, 0xf6, 0x30, 0x6f, 0xe1, 0x5d, 0x62, 0xbf},
	{0x97, 0x1e, 0xc1, 0x3d, 0x3d, 0xe0, 0x11, 0xe9},
	{0x06, 0xd7, 0x4d, 0xb1, 0x80, 0xa3, 0xb1, 0x38},
	{0x67, 0xa1, 0xa9, 0x75, 0x0e, 0x5b, 0xc6, 0xb4},
	{0x51, 0x0f, 0x33, 0x0e, 0x4f, 0x67, 0xd2, 0x0c},
	{0xf1, 0x73, 0x7e, 0xd8, 0x44, 0xea, 0xdb, 0xe5},
	{0x14, 0x0e, 0x16, 0xce, 0x7f, 0x4a, 0x9c, 0x7b},
	{0x4b, 0xfe, 0x43, 0xfd, 0xbf, 0x36, 0x04, 0x47},
	{0xb1, 0xeb, 0x3e, 0x15, 0x36, 0xa7, 0xbb, 0xe2},
	{0x6d, 0x0b, 0x41, 0xdd, 0x00, 0x98, 0x0b, 0x19},
	{0xd3, 0xce, 0x45, 0xce, 0x1d, 0x56, 0xb7, 0xfc},
	{0xd9, 0xf0, 0xfd, 0xda, 0xc0, 0x23, 0xb7, 0x93},
	{0x4c, 0x6f, 0xa1, 0xe4, 0x0c, 0xa8, 0xca, 0x57},
	{0xe6, 0x2f, 0x28, 0xa7, 0x0c, 0x94, 0x0d, 0x08},
	{0x8f, 0xe3, 0xf0, 0xb6, 0x29, 0xe3, 0x44, 0x03},
	{0xff, 0x98, 0xdd, 0x04, 0x45, 0xb4, 0x6d, 0x1f},
	{0x9e, 0x45, 0x4d, 0x18, 0x40, 0x53, 0xdb, 0xef},
	{0xb7, 0x3b, 0xef, 0x29, 0xbe, 0xa8, 0x13, 0x71},
	{0x02, 0x54, 0x55, 0x41, 0x8e, 0x04, 0xfc, 0xad},
	{0x6a, 0x0a, 0xee, 0x7c, 0x10, 0xd9, 0x19, 0xfe},
	{0x0a, 0x22, 0xd9, 0x41, 0xcc, 0x23, 0x87, 0x13},
	{0x6e, 0xff, 0x1f, 0xff, 0x36, 0x17, 0x9c, 0xbe},
	{0x79, 0xad, 0xb7, 0x40, 0xf4, 0x9f, 0x51, 0xa6},
	{0x97, 0x81, 0x99, 0xa4, 0xde, 0x9e, 0x9f, 0xb6},
	{0x12, 0x19, 0x7a, 0x28, 0xd0, 0xdc, 0xcc, 0x92},
	{0x81, 0xda, 0x60, 0x1e, 0x0e, 0xdd, 0x65, 0x56},
	{0x7d, 0x76, 0x20, 0xb2, 0x73, 0xc9, 0x9e, 0xee},
}

func TestSaltedCipher(t *testing.T) {
	var key, salt [32]byte
	for i := range key {
		key[i] = byte(i)
		salt[i] = byte(i + 32)
	}
	for i, v := range saltedVectors {
		c, err := NewSaltedCipher(key[:], salt[:i])
		if err != nil {
			t.Fatal(err)
		}
		var buf [8]byte
		c.Encrypt(buf[:], buf[:])
		if v != buf {
			t.Errorf("%d: expected %x, got %x", i, v, buf)
		}
	}
}

func BenchmarkExpandKeyWithSalt(b *testing.B) {
	key := make([]byte, 32)
	salt := make([]byte, 16)
	c, _ := NewCipher(key)
	for i := 0; i < b.N; i++ {
		expandKeyWithSalt(key, salt, c)
	}
}

func BenchmarkExpandKey(b *testing.B) {
	key := make([]byte, 32)
	c, _ := NewCipher(key)
	for i := 0; i < b.N; i++ {
		ExpandKey(key, c)
	}
}
